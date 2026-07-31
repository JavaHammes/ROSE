/*
 * User-process lifecycle and the foreground round-robin scheduler.
 *
 * A process owns a private Sv39 root, ELF pages, one user stack page, and one
 * supervisor-only trap stack. The complete user register set lives in a
 * trap_frame, allowing the trap handler to switch processes by replacing the
 * frame that assembly will restore.
 *
 * The shell can create several READY processes and later run them as one
 * foreground batch. Timer interrupts preempt U-mode, yield switches voluntarily,
 * and typed wait channels support BLOCKED processes. When no process can run,
 * assembly resumes scheduler_run_ready so supervisor mode can idle or finish.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf_loader.h"
#include "page_allocator.h"
#include "panic.h"
#include "scheduler.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "user_abi.h"
#include "user_process.h"
#include "vfs.h"
#include "virtual_memory.h"

enum {
        USER_WRITE_MAX = 1024,
        USER_WRITE_TRANSMIT_MAX = USER_WRITE_MAX * 2,
        PROCESS_LIMIT = 8,
};

enum process_state {
        /* UNUSED slots may be assigned a new PID. EXITED slots retain status
         * until the shell explicitly reaps them. */
        PROCESS_UNUSED,
        PROCESS_READY,
        PROCESS_RUNNING,
        PROCESS_BLOCKED,
        PROCESS_EXITED,
};

/*
 * User virtual-address layout:
 *
 *   0x00001000 .. 0x007fefff   ELF load range
 *   0x007ff000                 unmapped stack guard
 *   0x00800000                 one-page user stack
 *
 * The stack pointer begins just above the mapped stack page and grows down.
 */
#define USER_ADDRESS_MIN UINT64_C(0x00001000)
#define USER_STACK_GUARD_ADDRESS UINT64_C(0x007ff000)
#define USER_STACK_ADDRESS UINT64_C(0x00800000)
#define USER_STACK_TOP (USER_STACK_ADDRESS + PAGE_SIZE)

extern char text_start[];

/* Assembly transitions which save and later restore the suspended shell call. */
extern void user_mode_enter(const struct trap_frame *context,
                            uintptr_t kernel_trap_stack_top);
extern void user_mode_resume(void);
extern uintptr_t user_saved_kernel_context_sp;

struct process {
        /* Scheduler-visible identity and saved execution state. */
        uint64_t pid;
        enum process_state state;
        struct trap_frame context;
        /* Resources below remain owned until process_release_resources. */
        struct page_table *address_space;
        struct elf_loaded_image loaded_image;
        void *stack_page;
        void *kernel_trap_stack;
        uint64_t exit_status;
        bool exit_reported;

        /* Blocking syscall continuation state. A process never returns to
         * U-mode while pending_write is true. */
        enum scheduler_wait_channel wait_channel;
        bool pending_write;
        size_t write_result_length;
        size_t write_length;
        size_t write_offset;
        char write_buffer[USER_WRITE_TRANSMIT_MAX];
};

/* Fixed slots keep early process management independent of a kernel heap. */
static struct process process_table[PROCESS_LIMIT];
static struct process *active_process;
static struct process *uart_write_owner;
static uint64_t next_pid = 1U;
static uint64_t scheduler_preemptions;
static uint64_t scheduler_context_switches;
static uint64_t scheduler_blocks;

/* Freestanding replacement for clearing process records. */
static void bytes_zero(void *destination, size_t size) {
        volatile uint8_t *bytes = destination;

        for (size_t index = 0U; index < size; index++) {
                bytes[index] = 0U;
        }
}

/* trap_frame contains only integer-sized fields, so an explicit word copy is
 * sufficient in this freestanding environment. */
static void trap_frame_copy(struct trap_frame *destination,
                            const struct trap_frame *source) {
        uint64_t *destination_words = (uint64_t *)destination;
        const uint64_t *source_words = (const uint64_t *)source;

        for (size_t index = 0U;
             index < sizeof(struct trap_frame) / sizeof(uint64_t); index++) {
                destination_words[index] = source_words[index];
        }
}

/* Assert an expected user mapping during process construction. */
static void verify_user_mapping(const struct page_table *root,
                                uintptr_t virtual_address,
                                uintptr_t expected_physical_address,
                                uint64_t required_flags,
                                uint64_t forbidden_flags) {
        uintptr_t physical_address;
        uint64_t flags;

        if (!page_table_translate(root, virtual_address, &physical_address,
                                  &flags) ||
            physical_address != expected_physical_address ||
            (flags & required_flags) != required_flags ||
            (flags & forbidden_flags) != 0U) {
                panic("User address-space mapping verification failed");
        }
}

/*
 * Release every physical resource owned by a process, but preserve PID, state,
 * and exit status for ps. The user stack is verified and unmapped explicitly;
 * ELF teardown performs the same ownership check for executable/data pages.
 */
static void process_release_resources(struct process *process) {
        if (process->address_space != NULL) {
                if (process->stack_page != NULL) {
                        uintptr_t stack_physical_address;

                        if (page_table_translate(process->address_space,
                                                 USER_STACK_ADDRESS,
                                                 &stack_physical_address,
                                                 NULL) &&
                            (stack_physical_address !=
                                 (uintptr_t)process->stack_page ||
                             !page_table_unmap(process->address_space,
                                               USER_STACK_ADDRESS))) {
                                panic("User stack ownership mismatch");
                        }
                }

                elf_unload_image(process->address_space,
                                 &process->loaded_image);
                page_table_destroy(process->address_space);
                process->address_space = NULL;
        }
        if (process->stack_page != NULL) {
                page_free(process->stack_page);
                process->stack_page = NULL;
        }
        if (process->kernel_trap_stack != NULL) {
                page_free(process->kernel_trap_stack);
                process->kernel_trap_stack = NULL;
        }
}

/* Exited processes are zombies until reaped and therefore do not count as free
 * slots. This makes lifecycle state visible instead of silently discarding it. */
static struct process *process_find_available_slot(void) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_UNUSED) {
                        return process;
                }
        }

        return NULL;
}

/* Linear lookup is appropriate for the current eight-entry process table. */
static struct process *process_find_by_pid(uint64_t pid) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                if (process_table[index].state != PROCESS_UNUSED &&
                    process_table[index].pid == pid) {
                        return &process_table[index];
                }
        }

        return NULL;
}

/*
 * Construct a READY process from an ELF resolved by absolute VFS path.
 *
 * All fields are initialized before resources are acquired so the common
 * teardown path can safely handle allocation or mapping failures at any step.
 * Kernel mappings are installed supervisor-only by
 * virtual_memory_create_address_space; only the ELF and stack mappings receive
 * VM_PAGE_USER.
 */
static bool process_create(struct process *process, const char *path) {
        struct vfs_file executable;

        if (!vfs_open(path, &executable) || executable.size == 0U) {
                return false;
        }

        bytes_zero(process, sizeof(*process));
        process->pid = next_pid;
        process->state = PROCESS_READY;
        process->context.sp = USER_STACK_TOP;
        /* SPIE causes sret to enable supervisor interrupts while U-mode runs. */
        process->context.sstatus = SSTATUS_SPIE;
        process->exit_status = UINT64_MAX;
        next_pid++;
        if (next_pid == 0U) {
                next_pid = 1U;
        }

        process->address_space = virtual_memory_create_address_space();
        process->stack_page = page_alloc();
        process->kernel_trap_stack = page_alloc();

        if (process->address_space == NULL || process->stack_page == NULL ||
            process->kernel_trap_stack == NULL) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return false;
        }

        struct page_table *root = process->address_space;

        if (!page_table_map(root, USER_STACK_ADDRESS,
                            (uintptr_t)process->stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE) ||
            !elf_load_image(root, executable.data, executable.size,
                            USER_ADDRESS_MIN, USER_STACK_GUARD_ADDRESS,
                            &process->loaded_image)) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return false;
        }

        process->context.sepc = process->loaded_image.entry;

        uintptr_t unexpected_physical_address;

        /* A guard page catches a one-page stack overflow before it can reach
         * ELF memory. */
        if (page_table_translate(root, USER_STACK_GUARD_ADDRESS,
                                 &unexpected_physical_address, NULL)) {
                panic("User stack guard page is unexpectedly mapped");
        }

        verify_user_mapping(
            root, USER_STACK_ADDRESS, (uintptr_t)process->stack_page,
            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE, VM_PAGE_EXECUTE);

        /* Kernel text is mapped in every process but remains supervisor-only. */
        uintptr_t kernel_text_physical;
        uint64_t kernel_text_flags;

        if (!page_table_translate(root, (uintptr_t)text_start,
                                  &kernel_text_physical, &kernel_text_flags) ||
            (kernel_text_flags & VM_PAGE_USER) != 0U) {
                panic("Kernel text is accessible from U-mode");
        }

        return true;
}

/* Validate a complete U-mode read range before copying any bytes from it. */
static bool user_read_range_is_valid(uintptr_t user_buffer, size_t length) {
        if (active_process == NULL) {
                return false;
        }

        const struct page_table *root = active_process->address_space;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;
                uint64_t flags;

                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, &flags) ||
                    (flags & (VM_PAGE_USER | VM_PAGE_READ)) !=
                        (VM_PAGE_USER | VM_PAGE_READ)) {
                        return false;
                }

                size_t page_remaining =
                    PAGE_SIZE - ((user_buffer + offset) & (PAGE_SIZE - 1U));
                size_t chunk = length - offset;

                if (chunk > page_remaining) {
                        chunk = page_remaining;
                }
                offset += chunk;
        }

        return true;
}

/*
 * Copy a validated user write into process-owned memory. This is essential for
 * blocking: the driver and scheduler never retain a pointer into the transient
 * trap frame, and another address space may run before this syscall completes.
 */
static uint64_t syscall_write_begin(uintptr_t user_buffer, size_t length) {
        if (active_process == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        if (length > USER_WRITE_MAX ||
            (length != 0U && user_buffer > UINTPTR_MAX - (length - 1U))) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (!user_read_range_is_valid(user_buffer, length)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        struct process *process = active_process;
        const struct page_table *root = process->address_space;
        size_t transmit_length = 0U;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;

                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, NULL)) {
                        panic("Validated user mapping disappeared");
                }

                size_t page_remaining =
                    PAGE_SIZE - ((user_buffer + offset) & (PAGE_SIZE - 1U));
                size_t chunk = length - offset;

                if (chunk > page_remaining) {
                        chunk = page_remaining;
                }

                const char *characters = (const char *)physical_address;

                for (size_t index = 0U; index < chunk; index++) {
                        char character = characters[index];

                        if (character == '\n') {
                                process->write_buffer[transmit_length] = '\r';
                                transmit_length++;
                        }
                        process->write_buffer[transmit_length] = character;
                        transmit_length++;
                }

                offset += chunk;
        }

        process->pending_write = transmit_length != 0U;
        process->write_result_length = length;
        process->write_length = transmit_length;
        process->write_offset = 0U;

        return length;
}

/* Select the first READY slot after the current process, wrapping once. */
static struct process *scheduler_find_next_ready(void) {
        size_t start_index = 0U;

        if (active_process != NULL) {
                start_index = (size_t)(active_process - process_table + 1) %
                              PROCESS_LIMIT;
        }

        for (size_t offset = 0U; offset < PROCESS_LIMIT; offset++) {
                size_t index = (start_index + offset) % PROCESS_LIMIT;

                if (process_table[index].state == PROCESS_READY) {
                        return &process_table[index];
                }
        }

        return NULL;
}

/* Replace the trap frame which trap.S will restore, then select the matching
 * address space. The trap-stack pointer is filled just before trap return. */
static void scheduler_switch_to(struct process *next,
                                struct trap_frame *frame) {
        active_process = next;
        active_process->state = PROCESS_RUNNING;
        trap_frame_copy(frame, &active_process->context);
        page_table_activate(active_process->address_space);
}

/* Redirect this trap to the supervisor context suspended by user_mode_enter. */
static void scheduler_return_to_kernel(struct trap_frame *frame) {
        active_process = NULL;
        frame->sepc = (uintptr_t)user_mode_resume;
        frame->sp = user_saved_kernel_context_sp;
        frame->sstatus |= SSTATUS_SPP;
}

void scheduler_block_current(struct trap_frame *frame,
                             enum scheduler_wait_channel channel) {
        if (frame == NULL) {
                panic("Scheduler block requires a trap frame");
        }
        if (channel == SCHEDULER_WAIT_NONE || active_process == NULL ||
            active_process->state != PROCESS_RUNNING ||
            user_saved_kernel_context_sp == 0U) {
                panic_trap("Invalid scheduler block", frame);
        }

        struct process *previous = active_process;

        trap_frame_copy(&previous->context, frame);
        previous->state = PROCESS_BLOCKED;
        previous->wait_channel = channel;
        scheduler_blocks++;

        struct process *next = scheduler_find_next_ready();

        if (next != NULL) {
                scheduler_context_switches++;
                scheduler_switch_to(next, frame);
                return;
        }

        scheduler_return_to_kernel(frame);
}

size_t scheduler_wake_all(enum scheduler_wait_channel channel) {
        if (channel == SCHEDULER_WAIT_NONE) {
                return 0U;
        }

        size_t woken = 0U;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_BLOCKED &&
                    process->wait_channel == channel) {
                        process->state = PROCESS_READY;
                        process->wait_channel = SCHEDULER_WAIT_NONE;
                        woken++;
                }
        }

        return woken;
}

/* Advance one resumable write. While it is blocked, sepc continues to point at
 * ECALL; waking the process safely re-enters this continuation in a fresh trap. */
static void process_continue_write(struct trap_frame *frame) {
        struct process *process = active_process;

        if (process == NULL || !process->pending_write) {
                panic_trap("Invalid pending write", frame);
        }
        if (uart_write_owner == NULL) {
                uart_write_owner = process;
        }
        if (uart_write_owner != process) {
                scheduler_block_current(frame, SCHEDULER_WAIT_UART_TX);
                return;
        }
        if (process->write_offset == process->write_length) {
                process->pending_write = false;
                uart_write_owner = NULL;
                frame->a0 = process->write_result_length;
                frame->sepc += 4U;
                /* The final THRE interrupt may have woken contenders before
                 * ownership was released. Wake them again now that one can
                 * acquire the complete-write transaction. */
                (void)scheduler_wake_all(SCHEDULER_WAIT_UART_TX);
                return;
        }

        if (uart_tx_submit(process->write_buffer[process->write_offset])) {
                process->write_offset++;
        }

        scheduler_block_current(frame, SCHEDULER_WAIT_UART_TX);
}

/*
 * Save the interrupted process and choose another READY process. If the current
 * process is the only runnable one, leave it RUNNING without performing an
 * address-space switch. Timer-driven switches are counted separately from
 * voluntary yields.
 */
static void scheduler_reschedule(struct trap_frame *frame,
                                 bool count_preemption) {
        struct process *previous = active_process;

        trap_frame_copy(&previous->context, frame);
        previous->state = PROCESS_READY;

        struct process *next = scheduler_find_next_ready();

        if (next == NULL) {
                panic_trap("Scheduler found no runnable process", frame);
        }
        if (next == previous) {
                previous->state = PROCESS_RUNNING;
                return;
        }

        if (count_preemption) {
                scheduler_preemptions++;
        }
        scheduler_context_switches++;
        scheduler_switch_to(next, frame);
}

/*
 * Convert the active process into an EXITED zombie. If no READY process remains,
 * redirect trap return to user_mode_resume in supervisor mode. That assembly
 * continuation restores the kernel call context saved by user_mode_enter.
 */
static void user_process_finish(struct trap_frame *frame, uint64_t status) {
        if (active_process == NULL ||
            active_process->state != PROCESS_RUNNING ||
            user_saved_kernel_context_sp == 0U) {
                panic_trap("Invalid user process return state", frame);
        }

        active_process->state = PROCESS_EXITED;
        active_process->exit_status = status;

        struct process *next = scheduler_find_next_ready();

        if (next != NULL) {
                scheduler_context_switches++;
                scheduler_switch_to(next, frame);
                return;
        }

        scheduler_return_to_kernel(frame);
}

/* Print a zombie's retained status exactly once. */
static void print_process_exit(const struct process *process) {
        uart_puts("Process ");
        uart_put_uint64(process->pid);
        uart_puts(" exited with status ");
        uart_put_uint64(process->exit_status);
        uart_putc('\n');
}

static void report_and_release_exited_processes(void) {
        /* Resource teardown must wait until the scheduler has switched back to
         * the kernel root; the exiting process may still be using its page
         * tables and trap stack during the final sret. */
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state != PROCESS_EXITED) {
                        continue;
                }

                process_release_resources(process);

                if (!process->exit_reported) {
                        print_process_exit(process);
                        process->exit_reported = true;
                }
        }
}

static bool scheduler_has_blocked_processes(void) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                if (process_table[index].state == PROCESS_BLOCKED) {
                        return true;
                }
        }

        return false;
}

/*
 * Enter READY processes until no live work remains. user_mode_enter saves the
 * supervisor ABI context and sret enters U-mode; exits and all-blocked states
 * return through user_mode_resume so this loop can finish or wait for an IRQ.
 */
static bool scheduler_run_ready(bool require_preemption) {
        if (active_process != NULL) {
                uart_puts("A user process is already running\n");
                return false;
        }

        struct process *first = scheduler_find_next_ready();

        if (first == NULL) {
                uart_puts("No ready processes\n");
                report_and_release_exited_processes();
                return false;
        }

        scheduler_preemptions = 0U;
        scheduler_context_switches = 0U;
        scheduler_blocks = 0U;

        /* A blocking process may return us here many times. Wait in S-mode
         * when every process is asleep, then restore the complete saved frame
         * of whichever process an interrupt makes READY. */
        while (first != NULL || scheduler_has_blocked_processes()) {
                if (first == NULL) {
                        __asm__ volatile("wfi");
                        first = scheduler_find_next_ready();
                        continue;
                }

                active_process = first;
                active_process->state = PROCESS_RUNNING;
                user_saved_kernel_context_sp = 0U;

                /* Start a fresh quantum rather than inheriting the idle loop's
                 * timer deadline. */
                timer_schedule_next();
                page_table_activate(active_process->address_space);
                user_mode_enter(
                    &active_process->context,
                    (uintptr_t)active_process->kernel_trap_stack + PAGE_SIZE);

                if (active_process != NULL) {
                        panic("Scheduler returned while a process was running");
                }

                page_table_activate(virtual_memory_kernel_page_table());
                first = scheduler_find_next_ready();
        }

        if (require_preemption && scheduler_preemptions == 0U) {
                panic("Multitasking demo completed without timer preemption");
        }

        report_and_release_exited_processes();

        if (scheduler_context_switches != 0U) {
                uart_puts("Scheduler switches: ");
                uart_put_uint64(scheduler_context_switches);
                uart_puts(" (");
                uart_put_uint64(scheduler_preemptions);
                uart_puts(" preemptions)\n");
        }

        if (scheduler_blocks != 0U) {
                uart_puts("Scheduler blocks: ");
                uart_put_uint64(scheduler_blocks);
                uart_putc('\n');
        }

        return true;
}

/* Create a persistent READY entry without starting the scheduler. */
bool user_process_spawn(const char *path, uint64_t *pid) {
        struct process *process = process_find_available_slot();

        if (process == NULL) {
                return false;
        }
        if (!process_create(process, path)) {
                bytes_zero(process, sizeof(*process));
                return false;
        }

        if (pid != NULL) {
                *pid = process->pid;
        }

        return true;
}

/* Run all processes previously created with user_process_spawn. */
bool user_process_run_ready(void) {
        return scheduler_run_ready(false);
}

/*
 * Terminate a READY process from the shell. RUNNING cannot occur here because
 * the foreground shell is suspended whenever U-mode is executing. Status 137
 * follows the conventional 128 + SIGKILL representation.
 */
bool user_process_kill(uint64_t pid) {
        struct process *process = process_find_by_pid(pid);

        if (process == NULL || process->state != PROCESS_READY) {
                return false;
        }

        process->state = PROCESS_EXITED;
        process->exit_status = 137U;
        process_release_resources(process);
        return true;
}

/* Remove EXITED metadata and make those fixed table slots reusable. */
size_t user_process_reap_exited(void) {
        size_t reaped = 0U;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state != PROCESS_EXITED) {
                        continue;
                }

                process_release_resources(process);
                bytes_zero(process, sizeof(*process));
                reaped++;
        }

        return reaped;
}

/* Spawn and immediately run the default hello demonstration. */
void user_process_run(void) {
        uint64_t pid;

        if (!user_process_spawn("/bin/hello", &pid)) {
                uart_puts("Unable to create process; run 'reap' and retry\n");
                return;
        }

        (void)pid;
        (void)scheduler_run_ready(false);
}

/* Spawn one executable by absolute VFS path and run it immediately. */
void user_process_run_path(const char *path) {
        uint64_t pid;

        if (!user_process_spawn(path, &pid)) {
                uart_puts("Unable to load program: ");
                uart_puts(path);
                uart_putc('\n');
                return;
        }

        (void)pid;
        (void)scheduler_run_ready(false);
}

/* Build two independent processes and require at least one timer preemption. */
void user_process_run_multi(void) {
        const char *programs[] = {
            "/bin/process-a",
            "/bin/process-b",
        };

        uint64_t created_pids[2];

        /* Construct both processes before entering U-mode so a creation failure
         * cannot leave the shell suspended with a partial demonstration. */
        for (size_t index = 0U; index < 2U; index++) {
                if (!user_process_spawn(programs[index], &created_pids[index])) {
                        for (size_t created = 0U; created < index; created++) {
                                (void)user_process_kill(created_pids[created]);
                        }
                        uart_puts("Unable to create multitasking demo; run "
                                  "'reap' and retry\n");
                        return;
                }
        }

        (void)scheduler_run_ready(true);
}

bool user_process_is_active(void) {
        if (active_process == NULL) {
                return false;
        }

        return active_process->state == PROCESS_RUNNING;
}

/* Timer interrupts from supervisor mode belong to the shell/kernel and must not
 * be interpreted as user scheduling events. */
void user_process_handle_timer(struct trap_frame *frame) {
        if ((frame->sstatus & SSTATUS_SPP) != 0U || !user_process_is_active()) {
                return;
        }

        scheduler_reschedule(frame, true);
}

/* Dispatch the small user ABI. Blocking write deliberately retains ECALL in
 * sepc until its continuation has completed; other calls advance immediately. */
void user_process_handle_syscall(struct trap_frame *frame) {
        if (!user_process_is_active()) {
                panic_trap("U-mode syscall without an active process", frame);
        }

        switch (frame->a7) {
        case USER_SYSCALL_WRITE: {
                if (!active_process->pending_write) {
                        uint64_t result = syscall_write_begin(
                            (uintptr_t)frame->a0, (size_t)frame->a1);

                        if (!active_process->pending_write) {
                                frame->a0 = result;
                                frame->sepc += 4U;
                                return;
                        }
                }

                process_continue_write(frame);
                return;
        }

        case USER_SYSCALL_EXIT:
                frame->sepc += 4U;
                user_process_finish(frame, frame->a0);
                return;

        case USER_SYSCALL_YIELD:
                frame->sepc += 4U;
                scheduler_reschedule(frame, false);
                return;

        default:
                frame->sepc += 4U;
                frame->a0 =
                    (uint64_t)-(int64_t)USER_ERROR_NOT_IMPLEMENTED;
                return;
        }
}

/* A synchronous U-mode fault terminates only that process; kernel faults remain
 * fatal and are handled by the generic trap path. */
void user_process_handle_fault(struct trap_frame *frame, uint64_t cause) {
        uart_puts("Process ");
        uart_put_uint64(active_process->pid);
        uart_puts(" terminated by exception ");
        uart_put_uint64(cause);
        uart_putc('\n');

        user_process_finish(frame, 1U);
}

/* Supply trap.S with the trusted stack required if the selected process traps
 * again after sret. This is done after scheduling because the selected process
 * may differ from the one which entered the handler. */
void user_process_prepare_user_return(struct trap_frame *frame) {
        if (!user_process_is_active() ||
            active_process->kernel_trap_stack == NULL) {
                panic_trap("Invalid scheduled user return", frame);
        }

        frame->kernel_trap_stack_top =
            (uintptr_t)active_process->kernel_trap_stack + PAGE_SIZE;
}

/* Keep terminal formatting separate from the internal enum representation. */
static const char *process_state_name(enum process_state state) {
        switch (state) {
        case PROCESS_UNUSED:
                return "unused";
        case PROCESS_READY:
                return "ready";
        case PROCESS_RUNNING:
                return "running";
        case PROCESS_BLOCKED:
                return "blocked";
        case PROCESS_EXITED:
                return "exited";
        default:
                return "unknown";
        }
}

/* Display both runnable processes and zombies awaiting reap. */
void user_process_print_table(void) {
        uart_puts("PID  STATE    STATUS\n");

        bool found = false;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                const struct process *process = &process_table[index];

                if (process->state == PROCESS_UNUSED) {
                        continue;
                }

                found = true;
                uart_put_uint64(process->pid);
                uart_puts("    ");
                uart_puts(process_state_name(process->state));

                if (process->state == PROCESS_EXITED) {
                        uart_puts("   ");
                        uart_put_uint64(process->exit_status);
                }

                uart_putc('\n');
        }

        if (!found) {
                uart_puts("(no processes)\n");
        }
}
