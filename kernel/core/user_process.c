/*
 * User-process lifecycle and the foreground round-robin scheduler.
 *
 * A process owns a private Sv39 root, ELF pages, one user stack page, one
 * supervisor-only trap stack, and a small descriptor table. The complete user
 * register set lives in a trap_frame, allowing the trap handler to switch
 * processes by replacing the frame that assembly will restore.
 *
 * A userspace shell can create child processes and wait for them. Timer
 * interrupts preempt U-mode, yield switches voluntarily, and typed wait
 * channels support BLOCKED processes. When no process can run, assembly
 * resumes scheduler_run_ready so supervisor mode can idle or finish.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf_loader.h"
#include "graphics_console.h"
#include "input.h"
#include "interrupt.h"
#include "page_allocator.h"
#include "panic.h"
#include "scheduler.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "user_abi.h"
#include "user_process.h"
#include "vfs.h"
#include "virtio_gpu.h"
#include "virtual_memory.h"

enum {
        USER_IO_MAX = 1024,
        USER_WRITE_TRANSMIT_MAX = USER_IO_MAX * 2,
        USER_PATH_MAX = 64,
        PROCESS_EXECUTABLE_MAX = 16 * 1024,
        PROCESS_DESCRIPTOR_LIMIT = 8,
        PROCESS_FIRST_OPEN_DESCRIPTOR = 3,
        PROCESS_LIMIT = 8,
        PROCESS_OPEN_FILE_LIMIT = PROCESS_LIMIT * PROCESS_DESCRIPTOR_LIMIT,
        PIPE_LIMIT = 8,
        PIPE_BUFFER_SIZE = USER_IO_MAX,
        PROCESS_ARGUMENT_LIMIT = 16,
        PROCESS_ENVIRONMENT_LIMIT = 16,
};

enum descriptor_access {
        DESCRIPTOR_READ = (1U << 0),
        DESCRIPTOR_WRITE = (1U << 1),
};

enum process_state {
        /* UNUSED slots may be assigned a new PID. EXITED slots retain status
         * until their parent waits or the kernel reaps an orphan. */
        PROCESS_UNUSED,
        PROCESS_READY,
        PROCESS_RUNNING,
        PROCESS_BLOCKED,
        PROCESS_STOPPED,
        PROCESS_EXITED,
};

_Static_assert((uint32_t)USER_OPEN_READ == (uint32_t)VFS_OPEN_READ &&
                   (uint32_t)USER_OPEN_WRITE == (uint32_t)VFS_OPEN_WRITE &&
                   (uint32_t)USER_OPEN_CREATE == (uint32_t)VFS_OPEN_CREATE &&
                   (uint32_t)USER_OPEN_TRUNCATE ==
                       (uint32_t)VFS_OPEN_TRUNCATE &&
                   (uint32_t)USER_OPEN_DIRECTORY ==
                       (uint32_t)VFS_OPEN_DIRECTORY,
               "User and VFS open flags must match");

/*
 * User virtual-address layout:
 *
 *   0x00001000 .. 0x007fefff   ELF load range
 *   first page after ELF ..     growable userspace heap
 *   0x007ff000                 unmapped stack guard
 *   0x00800000                 one-page user stack
 *
 * The stack pointer begins just above the mapped stack page and grows down.
 */
#define USER_ADDRESS_MIN UINT64_C(0x00001000)
#define USER_STACK_GUARD_ADDRESS UINT64_C(0x007ff000)
#define USER_STACK_ADDRESS UINT64_C(0x00800000)
#define USER_STACK_TOP (USER_STACK_ADDRESS + PAGE_SIZE)
#define USER_GRAPHICS_ADDRESS UINT64_C(0x01000000)

extern char text_start[];

/* Assembly transitions which save and later restore the kernel scheduler. */
extern void user_mode_enter(const struct trap_frame *context,
                            uintptr_t kernel_trap_stack_top);
extern void user_mode_resume(void);
extern uintptr_t user_saved_kernel_context_sp;

struct process_open_file {
        bool used;
        uint8_t access;
        size_t references;
        uint64_t offset;
        struct process_pipe *pipe;
        struct vfs_file file;
};

struct process_pipe {
        bool used;
        size_t readers;
        size_t writers;
        size_t read_offset;
        size_t write_offset;
        size_t count;
        uint8_t buffer[PIPE_BUFFER_SIZE];
};

struct process_descriptor {
        struct process_open_file *open_file;
        bool close_on_exec;
};

struct process_signal_disposition {
        uintptr_t handler;
        uintptr_t restorer;
};

struct process {
        /* Scheduler-visible identity and saved execution state. */
        uint64_t pid;
        uint64_t parent_pid;
        uint64_t process_group;
        enum process_state state;
        struct trap_frame context;
        /* Resources below remain owned until process_release_resources. */
        struct page_table *address_space;
        struct elf_loaded_image loaded_image;
        void *stack_page;
        void *kernel_trap_stack;
        uintptr_t heap_start;
        uintptr_t heap_break;
        uint64_t exit_status;
        uint32_t termination_signal;
        uint32_t stop_signal;
        bool stop_event;
        bool continued_event;
        bool exit_reported;
        char current_directory[USER_PATH_MAX];
        struct process_descriptor descriptors[PROCESS_DESCRIPTOR_LIMIT];

        /* Dispositions survive fork. Caught handlers reset on exec, while an
         * ignored disposition remains ignored. Only one handler can be active
         * until its trusted sigreturn trampoline restores this saved frame. */
        struct process_signal_disposition
            signal_dispositions[USER_SIGNAL_MAX + 1U];
        uint64_t pending_signals;
        bool signal_active;
        struct trap_frame signal_context;

        /* Blocking syscall continuation state. A process never returns to
         * U-mode while either pending flag is true. */
        enum scheduler_wait_channel wait_channel;
        bool pending_write;
        size_t write_descriptor;
        size_t write_result_length;
        size_t write_length;
        size_t write_offset;
        char write_buffer[USER_WRITE_TRANSMIT_MAX];

        bool pending_read;
        size_t read_descriptor;
        uintptr_t read_buffer;
        size_t read_length;
};

/* A replacement image is constructed here while execve's original image is
 * still active. The single-hart kernel permits one shared transaction record
 * and avoids placing ELF ownership metadata on the one-page trap stack. */
struct process_replacement {
        struct page_table *address_space;
        struct elf_loaded_image loaded_image;
        void *stack_page;
        uintptr_t heap_start;
        uintptr_t heap_break;
        struct trap_frame context;
};

/* Fixed slots keep early process management independent of a kernel heap. */
static struct process process_table[PROCESS_LIMIT];
static struct process_pipe pipe_table[PIPE_LIMIT];
static struct process_open_file open_file_table[PROCESS_OPEN_FILE_LIMIT];
static struct process *active_process;
static struct process *uart_write_owner;
static uint64_t next_pid = 1U;
static uint64_t graphics_owner_pid;
static uint64_t terminal_foreground_process_group;
static uint64_t scheduler_preemptions;
static uint64_t scheduler_context_switches;
static uint64_t scheduler_blocks;
static uint8_t executable_buffer[PROCESS_EXECUTABLE_MAX];
static struct process_replacement exec_replacement;
static char startup_string_buffer[PAGE_SIZE];
static const char *startup_arguments[PROCESS_ARGUMENT_LIMIT];
static const char *startup_environment[PROCESS_ENVIRONMENT_LIMIT];

/* Freestanding replacement for clearing process records. */
static void bytes_zero(void *destination, size_t size) {
        volatile uint8_t *bytes = destination;

        for (size_t index = 0U; index < size; index++) {
                bytes[index] = 0U;
        }
}

static void graphics_release_if_owned(uint64_t pid) {
        if (pid == 0U || graphics_owner_pid != pid) {
                return;
        }
        graphics_owner_pid = 0U;
        input_event_clear();
        input_set_console_captured(false);
        graphics_console_init();
}

static struct process_pipe *process_pipe_allocate(void) {
        for (size_t index = 0U; index < PIPE_LIMIT; index++) {
                if (!pipe_table[index].used) {
                        struct process_pipe *pipe = &pipe_table[index];

                        bytes_zero(pipe, sizeof(*pipe));
                        pipe->used = true;
                        return pipe;
                }
        }

        return NULL;
}

static void process_pipe_retain(struct process_pipe *pipe, uint8_t access) {
        if (pipe == NULL || !pipe->used ||
            (access != DESCRIPTOR_READ && access != DESCRIPTOR_WRITE)) {
                panic("Invalid pipe endpoint retention");
        }

        if (access == DESCRIPTOR_READ) {
                pipe->readers++;
        } else {
                pipe->writers++;
        }
}

static void process_pipe_release(struct process_pipe *pipe, uint8_t access) {
        if (pipe == NULL || !pipe->used ||
            (access != DESCRIPTOR_READ && access != DESCRIPTOR_WRITE)) {
                panic("Invalid pipe endpoint release");
        }

        if (access == DESCRIPTOR_READ) {
                if (pipe->readers == 0U) {
                        panic("Pipe reader count underflow");
                }
                pipe->readers--;
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_WRITE);
        } else {
                if (pipe->writers == 0U) {
                        panic("Pipe writer count underflow");
                }
                pipe->writers--;
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_READ);
        }

        if (pipe->readers == 0U && pipe->writers == 0U) {
                bytes_zero(pipe, sizeof(*pipe));
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

static struct process_open_file *process_open_file_allocate(void) {
        for (size_t index = 0U; index < PROCESS_OPEN_FILE_LIMIT; index++) {
                if (!open_file_table[index].used) {
                        struct process_open_file *open_file =
                            &open_file_table[index];

                        bytes_zero(open_file, sizeof(*open_file));
                        open_file->used = true;
                        return open_file;
                }
        }

        return NULL;
}

static void process_descriptor_install(struct process_descriptor *descriptor,
                                       struct process_open_file *open_file) {
        if (descriptor->open_file != NULL || open_file == NULL ||
            !open_file->used ||
            open_file->references >= PROCESS_OPEN_FILE_LIMIT) {
                panic("Invalid descriptor installation");
        }

        descriptor->open_file = open_file;
        descriptor->close_on_exec = false;
        open_file->references++;
}

static void process_descriptor_close(struct process_descriptor *descriptor) {
        struct process_open_file *open_file = descriptor->open_file;

        if (open_file == NULL || !open_file->used ||
            open_file->references == 0U) {
                panic("Invalid descriptor close");
        }

        descriptor->open_file = NULL;
        descriptor->close_on_exec = false;
        open_file->references--;
        if (open_file->references == 0U) {
                if (open_file->pipe != NULL) {
                        process_pipe_release(open_file->pipe,
                                             open_file->access);
                }
                bytes_zero(open_file, sizeof(*open_file));
        }
}

static bool process_descriptors_initialize(struct process *process) {
        struct vfs_file console;

        if (vfs_open("/dev/console", VFS_OPEN_READ | VFS_OPEN_WRITE,
                     &console) != 0 ||
            console.type != VFS_NODE_CHARACTER_DEVICE ||
            console.device != VFS_DEVICE_CONSOLE) {
                return false;
        }

        for (size_t descriptor = 0U; descriptor <= USER_STDERR_FILENO;
             descriptor++) {
                struct process_open_file *open_file =
                    process_open_file_allocate();

                if (open_file == NULL) {
                        return false;
                }
                open_file->access = descriptor == USER_STDIN_FILENO
                                        ? DESCRIPTOR_READ
                                        : DESCRIPTOR_WRITE;
                open_file->file = console;
                process_descriptor_install(&process->descriptors[descriptor],
                                           open_file);
        }
        return true;
}

/* A new executable inherits descriptors not marked close-on-exec. The global
 * open-file description is shared, matching exec/spawn semantics for offsets
 * and pipe endpoint lifetime; close-on-exec remains descriptor-local. */
static bool process_descriptors_clone(struct process *destination,
                                      const struct process *source) {
        for (size_t descriptor = 0U; descriptor < PROCESS_DESCRIPTOR_LIMIT;
             descriptor++) {
                if (source->descriptors[descriptor].close_on_exec) {
                        continue;
                }
                struct process_open_file *source_open_file =
                    source->descriptors[descriptor].open_file;

                if (source_open_file == NULL) {
                        continue;
                }

                process_descriptor_install(
                    &destination->descriptors[descriptor], source_open_file);
        }

        return true;
}

/* fork duplicates descriptor-table entries, including close-on-exec flags,
 * while both tables continue to reference the same open-file descriptions. */
static void process_descriptors_fork(struct process *destination,
                                     const struct process *source) {
        for (size_t descriptor = 0U; descriptor < PROCESS_DESCRIPTOR_LIMIT;
             descriptor++) {
                const struct process_descriptor *source_descriptor =
                    &source->descriptors[descriptor];

                if (source_descriptor->open_file == NULL) {
                        continue;
                }

                process_descriptor_install(
                    &destination->descriptors[descriptor],
                    source_descriptor->open_file);
                destination->descriptors[descriptor].close_on_exec =
                    source_descriptor->close_on_exec;
        }
}

static void process_descriptors_close_all(struct process *process) {
        for (size_t index = 0U; index < PROCESS_DESCRIPTOR_LIMIT; index++) {
                if (process->descriptors[index].open_file != NULL) {
                        process_descriptor_close(&process->descriptors[index]);
                }
        }
}

static void process_descriptors_close_on_exec(struct process *process) {
        for (size_t index = 0U; index < PROCESS_DESCRIPTOR_LIMIT; index++) {
                if (process->descriptors[index].open_file != NULL &&
                    process->descriptors[index].close_on_exec) {
                        process_descriptor_close(&process->descriptors[index]);
                }
        }
}

static void process_signals_reset_on_exec(struct process *process) {
        for (size_t signal = 1U; signal <= USER_SIGNAL_MAX; signal++) {
                struct process_signal_disposition *disposition =
                    &process->signal_dispositions[signal];

                if (disposition->handler != USER_SIGNAL_IGNORE) {
                        disposition->handler = USER_SIGNAL_DEFAULT;
                }
                disposition->restorer = 0U;
        }

        process->signal_active = false;
        bytes_zero(&process->signal_context, sizeof(process->signal_context));
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

static uintptr_t align_up_to_page(uintptr_t address) {
        return (address + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
}

/* The heap starts on the first page not owned by the ELF loader. Keeping the
 * two ownership regions disjoint makes shrinking and image teardown exact. */
static uintptr_t
process_heap_start(const struct elf_loaded_image *loaded_image) {
        uintptr_t heap_start = USER_ADDRESS_MIN;

        for (size_t index = 0U; index < loaded_image->page_count; index++) {
                uintptr_t page_end =
                    loaded_image->pages[index].virtual_address + PAGE_SIZE;

                if (page_end > heap_start) {
                        heap_start = page_end;
                }
        }

        return heap_start;
}

/* Heap leaves own their physical pages. Unmap before freeing so a live page
 * table can never retain a translation to returned memory. */
static void process_free_heap_pages(struct page_table *root, uintptr_t start,
                                    uintptr_t end) {
        for (uintptr_t address = start; address < end; address += PAGE_SIZE) {
                uintptr_t physical_address;
                uint64_t flags;

                if (!page_table_translate(root, address, &physical_address,
                                          &flags) ||
                    (physical_address & (PAGE_SIZE - 1U)) != 0U ||
                    (flags & (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE)) !=
                        (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE) ||
                    (flags & VM_PAGE_EXECUTE) != 0U ||
                    !page_table_unmap(root, address)) {
                        panic("User heap ownership mismatch");
                }

                page_free((void *)physical_address);
        }
}

static void process_release_heap(struct page_table *root, uintptr_t heap_start,
                                 uintptr_t *heap_break) {
        if (root == NULL || heap_break == NULL) {
                return;
        }

        process_free_heap_pages(root, heap_start,
                                align_up_to_page(*heap_break));

        *heap_break = heap_start;
}

/* Release one user image without touching the process identity, descriptors,
 * or supervisor trap stack. This is shared by exit and execve rollback. */
static void process_release_image(struct page_table **address_space,
                                  struct elf_loaded_image *loaded_image,
                                  void **stack_page, uintptr_t *heap_start,
                                  uintptr_t *heap_break) {
        if (*address_space != NULL) {
                process_release_heap(*address_space, *heap_start, heap_break);

                if (*stack_page != NULL) {
                        uintptr_t stack_physical_address;

                        if (page_table_translate(
                                *address_space, USER_STACK_ADDRESS,
                                &stack_physical_address, NULL) &&
                            (stack_physical_address != (uintptr_t)*stack_page ||
                             !page_table_unmap(*address_space,
                                               USER_STACK_ADDRESS))) {
                                panic("User stack ownership mismatch");
                        }
                }

                elf_unload_image(*address_space, loaded_image);
                page_table_destroy(*address_space);
                *address_space = NULL;
        }
        if (*stack_page != NULL) {
                page_free(*stack_page);
                *stack_page = NULL;
        }
        *heap_start = 0U;
        *heap_break = 0U;
}

/*
 * Release every physical resource owned by a process, but preserve PID, state,
 * and exit status for ps. Descriptors close first, the user stack is verified
 * and unmapped explicitly, and ELF teardown checks executable/data ownership.
 */
static void process_release_resources(struct process *process) {
        process_descriptors_close_all(process);
        process_release_image(&process->address_space, &process->loaded_image,
                              &process->stack_page, &process->heap_start,
                              &process->heap_break);

        if (process->kernel_trap_stack != NULL) {
                page_free(process->kernel_trap_stack);
                process->kernel_trap_stack = NULL;
        }
}

/* Exited processes are zombies until reaped and therefore do not count as free
 * slots. This makes lifecycle state visible instead of silently discarding it.
 */
static struct process *process_find_available_slot(void) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_UNUSED) {
                        return process;
                }
        }

        return NULL;
}

/* A process whose parent exits becomes kernel-owned. PID 0 is the stable
 * reaper for children orphaned by the long-lived userspace shell. */
static void process_orphan_children(uint64_t parent_pid) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state != PROCESS_UNUSED &&
                    process->parent_pid == parent_pid) {
                        process->parent_pid = 0U;
                }
        }
}

static size_t string_length(const char *text) {
        size_t length = 0U;

        while (text[length] != '\0') {
                length++;
        }

        return length;
}

static bool stack_copy_string(void *stack_page, uintptr_t *cursor,
                              const char *text, uintptr_t *user_address) {
        size_t length = string_length(text) + 1U;

        if (length > *cursor - USER_STACK_ADDRESS) {
                return false;
        }

        *cursor -= length;
        char *destination = (char *)stack_page + (*cursor - USER_STACK_ADDRESS);

        for (size_t index = 0U; index < length; index++) {
                destination[index] = text[index];
        }

        *user_address = *cursor;
        return true;
}

/*
 * Build the conventional process-entry stack:
 *
 *   argc, argv[], NULL, envp[], NULL, AT_NULL
 *
 * Strings occupy the high end of the page and the pointer table remains
 * 16-byte aligned for the RISC-V C ABI. A missing startup description creates
 * the usual single argv[0] entry from the executable path.
 */
static bool
process_build_initial_stack(void *stack_page, struct trap_frame *context,
                            const char *path,
                            const struct user_process_startup *startup) {
        const char *default_arguments[] = {path};
        size_t argument_count = 1U;
        const char *const *arguments = default_arguments;
        size_t environment_count = 0U;
        const char *const *environment = NULL;

        if (startup != NULL) {
                argument_count = startup->argument_count;
                arguments = startup->arguments;
                environment_count = startup->environment_count;
                environment = startup->environment;
        }

        if (argument_count == 0U || argument_count > PROCESS_ARGUMENT_LIMIT ||
            arguments == NULL ||
            environment_count > PROCESS_ENVIRONMENT_LIMIT ||
            (environment_count != 0U && environment == NULL)) {
                return false;
        }

        uintptr_t argument_addresses[PROCESS_ARGUMENT_LIMIT];
        uintptr_t environment_addresses[PROCESS_ENVIRONMENT_LIMIT];
        uintptr_t cursor = USER_STACK_TOP;

        for (size_t index = argument_count; index != 0U; index--) {
                if (arguments[index - 1U] == NULL ||
                    !stack_copy_string(stack_page, &cursor,
                                       arguments[index - 1U],
                                       &argument_addresses[index - 1U])) {
                        return false;
                }
        }

        for (size_t index = environment_count; index != 0U; index--) {
                if (environment[index - 1U] == NULL ||
                    !stack_copy_string(stack_page, &cursor,
                                       environment[index - 1U],
                                       &environment_addresses[index - 1U])) {
                        return false;
                }
        }

        /* argc, both terminated pointer arrays, and the two-word AT_NULL
         * auxiliary-vector terminator. */
        size_t word_count =
            1U + argument_count + 1U + environment_count + 1U + 2U;
        size_t table_size = word_count * sizeof(uint64_t);

        if (table_size > cursor - USER_STACK_ADDRESS) {
                return false;
        }

        uintptr_t stack_pointer = (cursor - table_size) & ~UINT64_C(0xf);
        if (stack_pointer < USER_STACK_ADDRESS) {
                return false;
        }

        uint64_t *words = (uint64_t *)((uint8_t *)stack_page + stack_pointer -
                                       USER_STACK_ADDRESS);
        size_t word = 0U;

        words[word++] = argument_count;
        for (size_t index = 0U; index < argument_count; index++) {
                words[word++] = argument_addresses[index];
        }
        words[word++] = 0U;
        for (size_t index = 0U; index < environment_count; index++) {
                words[word++] = environment_addresses[index];
        }
        words[word++] = 0U;
        words[word++] = 0U;
        words[word] = 0U;

        context->sp = stack_pointer;
        context->a0 = argument_count;
        context->a1 = stack_pointer + sizeof(uint64_t);
        context->a2 = context->a1 + (argument_count + 1U) * sizeof(uint64_t);
        return true;
}

/* Verify the invariant mappings shared by freshly spawned and replaced
 * images. These checks are assertions because all untrusted input has already
 * passed through the mapping and ELF validators. */
static void process_verify_address_space(const struct page_table *root,
                                         const void *stack_page) {
        uintptr_t unexpected_physical_address;

        /* A guard page catches a one-page stack overflow before it can reach
         * ELF memory. */
        if (page_table_translate(root, USER_STACK_GUARD_ADDRESS,
                                 &unexpected_physical_address, NULL)) {
                panic("User stack guard page is unexpectedly mapped");
        }

        verify_user_mapping(root, USER_STACK_ADDRESS, (uintptr_t)stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE,
                            VM_PAGE_EXECUTE);

        /* Kernel text is mapped in every process but remains supervisor-only.
         */
        uintptr_t kernel_text_physical;
        uint64_t kernel_text_flags;

        if (!page_table_translate(root, (uintptr_t)text_start,
                                  &kernel_text_physical, &kernel_text_flags) ||
            (kernel_text_flags & VM_PAGE_USER) != 0U) {
                panic("Kernel text is accessible from U-mode");
        }
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
static uint64_t process_create(struct process *process, const char *path,
                               const struct user_process_startup *startup,
                               uint64_t parent_pid,
                               const struct process *descriptor_source) {
        struct vfs_file executable;
        int open_result = vfs_open(path, VFS_OPEN_READ, &executable);

        if (open_result != 0) {
                return (uint64_t)(int64_t)open_result;
        }
        if (executable.type != VFS_NODE_REGULAR || executable.size == 0U ||
            executable.size > sizeof(executable_buffer)) {
                return (uint64_t)-(int64_t)USER_ERROR_EXEC_FORMAT;
        }

        long executable_length = vfs_read(&executable, 0U, executable_buffer,
                                          (size_t)executable.size);
        if (executable_length < 0) {
                return (uint64_t)(int64_t)executable_length;
        }
        if ((uint64_t)executable_length != executable.size) {
                return (uint64_t)-(int64_t)USER_ERROR_IO;
        }

        bytes_zero(process, sizeof(*process));
        process->pid = next_pid;
        process->parent_pid = parent_pid;
        process->process_group = descriptor_source == NULL
                                     ? process->pid
                                     : descriptor_source->process_group;
        process->state = PROCESS_READY;
        /* SPIE causes sret to enable supervisor interrupts while U-mode runs.
         */
        process->context.sstatus = SSTATUS_SPIE;
        process->exit_status = UINT64_MAX;
        process->current_directory[0] = '/';
        process->current_directory[1] = '\0';
        bool descriptors_ready =
            descriptor_source == NULL
                ? process_descriptors_initialize(process)
                : process_descriptors_clone(process, descriptor_source);
        if (!descriptors_ready) {
                panic("Initial process descriptors are unavailable");
        }
        next_pid++;
        if (next_pid == 0U) {
                next_pid = 1U;
        }
        if (terminal_foreground_process_group == 0U) {
                terminal_foreground_process_group = process->process_group;
        }

        process->address_space = virtual_memory_create_address_space();
        process->stack_page = page_alloc();
        process->kernel_trap_stack = page_alloc();

        if (process->address_space == NULL || process->stack_page == NULL ||
            process->kernel_trap_stack == NULL) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        struct page_table *root = process->address_space;

        if (!process_build_initial_stack(process->stack_page, &process->context,
                                         path, startup)) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return (uint64_t)-(int64_t)USER_ERROR_ARGUMENT_LIST_TOO_LONG;
        }
        if (!page_table_map(root, USER_STACK_ADDRESS,
                            (uintptr_t)process->stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE)) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }
        if (!elf_load_image(root, executable_buffer, (size_t)executable.size,
                            USER_ADDRESS_MIN, USER_STACK_GUARD_ADDRESS,
                            &process->loaded_image)) {
                process_release_resources(process);
                process->state = PROCESS_EXITED;
                process->exit_status = 1U;
                return (uint64_t)-(int64_t)USER_ERROR_EXEC_FORMAT;
        }

        process->heap_start = process_heap_start(&process->loaded_image);
        process->heap_break = process->heap_start;
        process->context.sepc = process->loaded_image.entry;
        process_verify_address_space(root, process->stack_page);

        return 0U;
}

static void page_copy(void *destination, const void *source) {
        uint64_t *destination_words = destination;
        const uint64_t *source_words = source;

        for (size_t index = 0U; index < PAGE_SIZE / sizeof(uint64_t); index++) {
                destination_words[index] = source_words[index];
        }
}

/* Copy the ELF-owned leaves and reproduce their exact permissions. Each page
 * is recorded before mapping so the ordinary image teardown also handles a
 * partially constructed fork child. */
static bool process_fork_loaded_image(struct process *child,
                                      const struct process *parent) {
        child->loaded_image.entry = parent->loaded_image.entry;

        for (size_t index = 0U; index < parent->loaded_image.page_count;
             index++) {
                const struct elf_loaded_page *source_page =
                    &parent->loaded_image.pages[index];
                void *physical_page = page_alloc();

                if (physical_page == NULL) {
                        return false;
                }

                page_copy(physical_page, source_page->physical_page);
                struct elf_loaded_page *destination_page =
                    &child->loaded_image
                         .pages[child->loaded_image.page_count++];
                destination_page->virtual_address =
                    source_page->virtual_address;
                destination_page->physical_page = physical_page;
                destination_page->flags = source_page->flags;

                if (!page_table_map(
                        child->address_space, destination_page->virtual_address,
                        (uintptr_t)physical_page, destination_page->flags)) {
                        return false;
                }
        }

        return true;
}

/* Heap pages are not part of ELF ownership metadata, so advance the candidate
 * break after every mapping. That makes process_release_heap exact on any
 * allocation failure. */
static bool process_fork_heap(struct process *child,
                              const struct process *parent) {
        uintptr_t mapped_end = align_up_to_page(parent->heap_break);

        for (uintptr_t address = parent->heap_start; address < mapped_end;
             address += PAGE_SIZE) {
                uintptr_t source_physical_address;
                uint64_t flags;

                if (!page_table_translate(parent->address_space, address,
                                          &source_physical_address, &flags) ||
                    (source_physical_address & (PAGE_SIZE - 1U)) != 0U ||
                    (flags & (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE)) !=
                        (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE) ||
                    (flags & VM_PAGE_EXECUTE) != 0U) {
                        panic("Fork source heap ownership mismatch");
                }

                void *physical_page = page_alloc();
                if (physical_page == NULL) {
                        return false;
                }
                page_copy(physical_page, (void *)source_physical_address);

                if (!page_table_map(child->address_space, address,
                                    (uintptr_t)physical_page, flags)) {
                        page_free(physical_page);
                        return false;
                }
                child->heap_break = address + PAGE_SIZE;
        }

        child->heap_break = parent->heap_break;
        return true;
}

/* Eagerly clone the caller's complete user image. ROSE does not yet have page
 * faults for copy-on-write, so fork pays the copy cost up front and produces
 * fully private ELF, heap, and stack pages before the child becomes READY. */
static uint64_t syscall_fork(const struct trap_frame *frame) {
        struct process *parent = active_process;
        struct process *child = process_find_available_slot();

        if (child == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
        }

        bytes_zero(child, sizeof(*child));
        child->parent_pid = parent->pid;
        child->process_group = parent->process_group;
        child->exit_status = UINT64_MAX;
        child->heap_start = parent->heap_start;
        child->heap_break = parent->heap_start;
        trap_frame_copy(&child->context, frame);
        child->context.a0 = 0U;
        child->context.sepc += 4U;
        for (size_t signal = 1U; signal <= USER_SIGNAL_MAX; signal++) {
                child->signal_dispositions[signal] =
                    parent->signal_dispositions[signal];
        }
        child->signal_active = parent->signal_active;
        if (parent->signal_active) {
                trap_frame_copy(&child->signal_context,
                                &parent->signal_context);
        }

        size_t directory_length = string_length(parent->current_directory) + 1U;
        for (size_t index = 0U; index < directory_length; index++) {
                child->current_directory[index] =
                    parent->current_directory[index];
        }

        child->address_space = virtual_memory_create_address_space();
        child->stack_page = page_alloc();
        child->kernel_trap_stack = page_alloc();

        if (child->address_space == NULL || child->stack_page == NULL ||
            child->kernel_trap_stack == NULL) {
                goto out_of_memory;
        }

        page_copy(child->stack_page, parent->stack_page);
        if (!page_table_map(child->address_space, USER_STACK_ADDRESS,
                            (uintptr_t)child->stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE) ||
            !process_fork_loaded_image(child, parent) ||
            !process_fork_heap(child, parent)) {
                goto out_of_memory;
        }

        process_descriptors_fork(child, parent);
        process_verify_address_space(child->address_space, child->stack_page);

        child->pid = next_pid;
        next_pid++;
        if (next_pid == 0U) {
                next_pid = 1U;
        }
        child->state = PROCESS_READY;
        return child->pid;

out_of_memory:
        process_release_resources(child);
        bytes_zero(child, sizeof(*child));
        return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
}

/* Validate a complete user range with the requested leaf permissions. */
static bool user_range_is_valid(uintptr_t user_buffer, size_t length,
                                uint64_t required_flags) {
        if (active_process == NULL) {
                return false;
        }
        if (length != 0U && user_buffer > UINTPTR_MAX - (length - 1U)) {
                return false;
        }

        const struct page_table *root = active_process->address_space;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;
                uint64_t flags;

                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, &flags) ||
                    (flags & (VM_PAGE_USER | required_flags)) !=
                        (VM_PAGE_USER | required_flags)) {
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

/* Copy into a range which was validated before any externally visible work. */
static void user_copy_to(uintptr_t user_buffer, const uint8_t *source,
                         size_t length) {
        const struct page_table *root = active_process->address_space;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;

                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, NULL)) {
                        panic("Validated writable user mapping disappeared");
                }

                size_t page_remaining =
                    PAGE_SIZE - ((user_buffer + offset) & (PAGE_SIZE - 1U));
                size_t chunk = length - offset;

                if (chunk > page_remaining) {
                        chunk = page_remaining;
                }

                uint8_t *destination = (uint8_t *)physical_address;

                for (size_t index = 0U; index < chunk; index++) {
                        destination[index] = source[offset + index];
                }
                offset += chunk;
        }
}

/* Copy a previously validated user range into kernel-owned memory. */
static void user_copy_from(uint8_t *destination, uintptr_t user_buffer,
                           size_t length) {
        const struct page_table *root = active_process->address_space;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;
                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, NULL)) {
                        panic("Validated readable user mapping disappeared");
                }
                size_t page_remaining =
                    PAGE_SIZE - ((user_buffer + offset) & (PAGE_SIZE - 1U));
                size_t chunk = length - offset;
                if (chunk > page_remaining) {
                        chunk = page_remaining;
                }
                const uint8_t *source = (const uint8_t *)physical_address;
                for (size_t index = 0U; index < chunk; index++) {
                        destination[offset + index] = source[index];
                }
                offset += chunk;
        }
}

static bool signal_number_is_valid(int64_t signal) {
        return signal > 0 && signal <= USER_SIGNAL_MAX;
}

static uint64_t signal_bit(uint32_t signal) { return UINT64_C(1) << signal; }

static struct process *process_find_pid(uint64_t pid) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state != PROCESS_UNUSED &&
                    process->state != PROCESS_EXITED && process->pid == pid) {
                        return process;
                }
        }

        return NULL;
}

static bool process_group_exists(uint64_t process_group) {
        if (process_group == 0U) {
                return false;
        }

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                const struct process *process = &process_table[index];

                /* A zombie retains its PID and process-group identity until
                 * waitpid reaps it; later pipeline members may still join. */
                if (process->state != PROCESS_UNUSED &&
                    process->process_group == process_group) {
                        return true;
                }
        }

        return false;
}

static bool process_signal_stops_by_default(uint32_t signal) {
        return signal == USER_SIGNAL_STOP ||
               signal == USER_SIGNAL_TERMINAL_STOP ||
               signal == USER_SIGNAL_BACKGROUND_READ;
}

/* Queueing SIGCONT changes scheduler state immediately; its disposition is
 * still delivered later. SIGKILL likewise makes a stopped process runnable so
 * the normal trusted return boundary can terminate it. */
static void process_queue_signal(struct process *target, uint32_t signal) {
        if (signal == USER_SIGNAL_CONTINUE) {
                target->pending_signals &=
                    ~(signal_bit(USER_SIGNAL_STOP) |
                      signal_bit(USER_SIGNAL_TERMINAL_STOP) |
                      signal_bit(USER_SIGNAL_BACKGROUND_READ));
                if (target->state == PROCESS_STOPPED) {
                        target->state = PROCESS_READY;
                        target->continued_event = true;
                        (void)scheduler_wake_all(SCHEDULER_WAIT_CHILD);
                }
        } else if (process_signal_stops_by_default(signal)) {
                target->pending_signals &= ~signal_bit(USER_SIGNAL_CONTINUE);
        }

        target->pending_signals |= signal_bit(signal);
        if (target->state == PROCESS_BLOCKED ||
            (target->state == PROCESS_STOPPED && signal == USER_SIGNAL_KILL)) {
                target->state = PROCESS_READY;
                target->wait_channel = SCHEDULER_WAIT_NONE;
        }
}

static bool process_has_deliverable_signal(const struct process *process) {
        if (process->pending_signals == 0U) {
                return false;
        }
        if (!process->signal_active) {
                return true;
        }

        return (process->pending_signals & (signal_bit(USER_SIGNAL_KILL) |
                                            signal_bit(USER_SIGNAL_STOP))) !=
               0U;
}

static uint64_t syscall_signal_action(int64_t signal, uintptr_t user_action,
                                      uintptr_t user_old_action,
                                      uintptr_t user_restorer) {
        if (!signal_number_is_valid(signal)) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        struct user_signal_action new_action;
        if (user_action != 0U) {
                if (!user_range_is_valid(user_action, sizeof(new_action),
                                         VM_PAGE_READ)) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }
                user_copy_from((uint8_t *)&new_action, user_action,
                               sizeof(new_action));

                if (signal == USER_SIGNAL_KILL || signal == USER_SIGNAL_STOP ||
                    new_action.flags != 0U) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                if (new_action.handler != USER_SIGNAL_DEFAULT &&
                    new_action.handler != USER_SIGNAL_IGNORE &&
                    (!user_range_is_valid(new_action.handler, 1U,
                                          VM_PAGE_EXECUTE) ||
                     !user_range_is_valid(user_restorer, 1U,
                                          VM_PAGE_EXECUTE))) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }
        }
        if (user_old_action != 0U &&
            !user_range_is_valid(user_old_action,
                                 sizeof(struct user_signal_action),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        struct process_signal_disposition *disposition =
            &active_process->signal_dispositions[(size_t)signal];

        if (user_old_action != 0U) {
                struct user_signal_action old_action;
                bytes_zero(&old_action, sizeof(old_action));
                old_action.handler = disposition->handler;
                user_copy_to(user_old_action, (const uint8_t *)&old_action,
                             sizeof(old_action));
        }
        if (user_action != 0U) {
                disposition->handler = new_action.handler;
                disposition->restorer = new_action.handler > USER_SIGNAL_IGNORE
                                            ? user_restorer
                                            : 0U;
                if (new_action.handler == USER_SIGNAL_IGNORE) {
                        active_process->pending_signals &=
                            ~signal_bit((uint32_t)signal);
                }
        }

        return 0U;
}

static uint64_t syscall_kill(int64_t pid, int64_t signal) {
        if (signal < 0 || signal > USER_SIGNAL_MAX || pid == INT64_MIN) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        bool matched = false;
        uint64_t selected_group = pid == 0   ? active_process->process_group
                                  : pid < -1 ? (uint64_t)-pid
                                             : 0U;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *target = &process_table[index];
                bool selected = false;

                if (target->state == PROCESS_UNUSED ||
                    target->state == PROCESS_EXITED) {
                        continue;
                }
                if (pid > 0) {
                        selected = target->pid == (uint64_t)pid;
                } else if (pid == -1) {
                        selected = true;
                } else {
                        selected = target->process_group == selected_group;
                }
                if (!selected) {
                        continue;
                }

                matched = true;
                if (signal != 0) {
                        process_queue_signal(target, (uint32_t)signal);
                }
        }

        return matched ? 0U : (uint64_t)-(int64_t)USER_ERROR_NO_PROCESS;
}

static uint64_t syscall_set_process_group(int64_t pid, int64_t process_group) {
        if (pid < 0 || process_group < 0) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        struct process *target =
            pid == 0 ? active_process : process_find_pid((uint64_t)pid);
        if (target == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_PROCESS;
        }
        if (target != active_process &&
            target->parent_pid != active_process->pid) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }

        uint64_t requested_group =
            process_group == 0 ? target->pid : (uint64_t)process_group;
        if (requested_group != target->pid &&
            !process_group_exists(requested_group)) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }

        target->process_group = requested_group;
        return 0U;
}

static bool process_descriptor_is_console(uint64_t descriptor) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return false;
        }

        const struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        return open_file != NULL &&
               open_file->file.type == VFS_NODE_CHARACTER_DEVICE &&
               open_file->file.device == VFS_DEVICE_CONSOLE;
}

static uint64_t syscall_terminal_set_foreground_group(uint64_t descriptor,
                                                      int64_t process_group) {
        if (!process_descriptor_is_console(descriptor)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (process_group <= 0 ||
            !process_group_exists((uint64_t)process_group)) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_PROCESS;
        }
        terminal_foreground_process_group = (uint64_t)process_group;
        return 0U;
}

static uint64_t syscall_terminal_get_foreground_group(uint64_t descriptor) {
        if (!process_descriptor_is_console(descriptor)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        return terminal_foreground_process_group;
}

bool user_process_handle_console_control(char character) {
        uint32_t signal;

        if (character == '\x03') {
                signal = USER_SIGNAL_INTERRUPT;
        } else if (character == '\x1a') {
                signal = USER_SIGNAL_TERMINAL_STOP;
        } else {
                return false;
        }

        bool matched = false;
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *target = &process_table[index];

                if (target->state == PROCESS_UNUSED ||
                    target->state == PROCESS_EXITED ||
                    target->process_group !=
                        terminal_foreground_process_group) {
                        continue;
                }
                process_queue_signal(target, signal);
                matched = true;
        }

        return matched;
}

/* The interrupted frame never enters userspace, so sigreturn cannot forge
 * sstatus, kernel mappings, or a supervisor return address. */
static uint64_t syscall_signal_return(struct trap_frame *frame) {
        if (!active_process->signal_active) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        trap_frame_copy(frame, &active_process->signal_context);
        trap_frame_copy(&active_process->context,
                        &active_process->signal_context);
        bytes_zero(&active_process->signal_context,
                   sizeof(active_process->signal_context));
        active_process->signal_active = false;
        return 0U;
}

/* Copy a bounded null-terminated path without dereferencing a user pointer. */
static uint64_t user_copy_raw_path(uintptr_t user_path,
                                   char path[USER_PATH_MAX]) {
        for (size_t index = 0U; index < USER_PATH_MAX; index++) {
                uintptr_t address;

                if (user_path > UINTPTR_MAX - index ||
                    !user_range_is_valid(user_path + index, 1U, VM_PAGE_READ) ||
                    !page_table_translate(active_process->address_space,
                                          user_path + index, &address, NULL)) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }

                path[index] = *(const char *)address;
                if (path[index] == '\0') {
                        return 0U;
                }
        }

        return (uint64_t)-(int64_t)USER_ERROR_NAME_TOO_LONG;
}

/* Resolve '.', '..', and repeated separators against the process working
 * directory. VFS implementations continue to receive one canonical absolute
 * path, keeping mount routing independent of process state. */
static uint64_t process_resolve_path(const char *path,
                                     char resolved[USER_PATH_MAX]) {
        if (path[0] == '\0') {
                return (uint64_t)-(int64_t)USER_ERROR_NO_ENTRY;
        }

        size_t length = 1U;
        const char *cursor = path;
        bytes_zero(resolved, USER_PATH_MAX);
        resolved[0] = '/';

        if (path[0] == '/') {
                cursor++;
        } else {
                length = string_length(active_process->current_directory);
                for (size_t index = 0U; index < length; index++) {
                        resolved[index] =
                            active_process->current_directory[index];
                }
        }

        while (*cursor != '\0') {
                while (*cursor == '/') {
                        cursor++;
                }
                if (*cursor == '\0') {
                        break;
                }

                const char *segment = cursor;
                size_t segment_length = 0U;
                while (cursor[segment_length] != '\0' &&
                       cursor[segment_length] != '/') {
                        segment_length++;
                }
                cursor += segment_length;

                if (segment_length == 1U && segment[0] == '.') {
                        continue;
                }
                if (segment_length == 2U && segment[0] == '.' &&
                    segment[1] == '.') {
                        while (length > 1U && resolved[length - 1U] != '/') {
                                length--;
                        }
                        if (length > 1U) {
                                length--;
                        }
                        continue;
                }

                size_t separator_length = length == 1U ? 0U : 1U;
                if (length + separator_length >= USER_PATH_MAX ||
                    segment_length >
                        USER_PATH_MAX - length - separator_length - 1U) {
                        return (uint64_t)-(int64_t)USER_ERROR_NAME_TOO_LONG;
                }
                if (separator_length != 0U) {
                        resolved[length] = '/';
                        length++;
                }
                for (size_t index = 0U; index < segment_length; index++) {
                        resolved[length + index] = segment[index];
                }
                length += segment_length;
        }

        resolved[length] = '\0';
        return 0U;
}

static uint64_t user_copy_path(uintptr_t user_path, char path[USER_PATH_MAX]) {
        char raw_path[USER_PATH_MAX];
        uint64_t result = user_copy_raw_path(user_path, raw_path);

        return result == 0U ? process_resolve_path(raw_path, path) : result;
}

/* Copy one startup string into the shared transaction buffer used by execve
 * and spawn. Its page-sized bound matches the new stack's available storage. */
static uint64_t user_copy_startup_string(uintptr_t user_string,
                                         size_t *buffer_offset) {
        size_t source_offset = 0U;

        while (*buffer_offset < sizeof(startup_string_buffer)) {
                uintptr_t address;

                if (user_string > UINTPTR_MAX - source_offset ||
                    !user_range_is_valid(user_string + source_offset, 1U,
                                         VM_PAGE_READ) ||
                    !page_table_translate(active_process->address_space,
                                          user_string + source_offset, &address,
                                          NULL)) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }

                char character = *(const char *)address;
                startup_string_buffer[*buffer_offset] = character;
                (*buffer_offset)++;
                source_offset++;

                if (character == '\0') {
                        return 0U;
                }
        }

        return (uint64_t)-(int64_t)USER_ERROR_ARGUMENT_LIST_TOO_LONG;
}

/* Copy a null-terminated argv or envp array without directly dereferencing
 * either its user pointers or strings. The slot after the fixed limit is read
 * only to distinguish a valid terminator from E2BIG. */
static uint64_t
user_copy_startup_vector(uintptr_t user_vector, const char **destination,
                         size_t destination_limit, bool require_entry,
                         size_t *entry_count, size_t *buffer_offset) {
        for (size_t index = 0U; index <= destination_limit; index++) {
                size_t pointer_offset = index * sizeof(uintptr_t);
                uintptr_t user_string;

                if (user_vector > UINTPTR_MAX - pointer_offset ||
                    !user_range_is_valid(user_vector + pointer_offset,
                                         sizeof(user_string), VM_PAGE_READ)) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }

                user_copy_from((uint8_t *)&user_string,
                               user_vector + pointer_offset,
                               sizeof(user_string));

                if (user_string == 0U) {
                        if (require_entry && index == 0U) {
                                return (uint64_t)-(
                                    int64_t)USER_ERROR_INVALID_ARGUMENT;
                        }
                        *entry_count = index;
                        return 0U;
                }
                if (index == destination_limit) {
                        return (uint64_t)-(
                            int64_t)USER_ERROR_ARGUMENT_LIST_TOO_LONG;
                }

                destination[index] = &startup_string_buffer[*buffer_offset];
                uint64_t result =
                    user_copy_startup_string(user_string, buffer_offset);

                if (result != 0U) {
                        return result;
                }
        }

        return (uint64_t)-(int64_t)USER_ERROR_ARGUMENT_LIST_TOO_LONG;
}

static void process_release_replacement(void) {
        process_release_image(
            &exec_replacement.address_space, &exec_replacement.loaded_image,
            &exec_replacement.stack_page, &exec_replacement.heap_start,
            &exec_replacement.heap_break);
        bytes_zero(&exec_replacement.context, sizeof(exec_replacement.context));
}

/* Read and fully construct a replacement without modifying the running image.
 * Every failure releases only the candidate, leaving execve able to return to
 * its caller with registers, mappings, and descriptors unchanged. */
static uint64_t
process_prepare_replacement(const char *path,
                            const struct user_process_startup *startup) {
        struct vfs_file executable;
        int open_result = vfs_open(path, VFS_OPEN_READ, &executable);

        if (open_result != 0) {
                return (uint64_t)(int64_t)open_result;
        }
        if (executable.type != VFS_NODE_REGULAR || executable.size == 0U ||
            executable.size > sizeof(executable_buffer)) {
                return (uint64_t)-(int64_t)USER_ERROR_EXEC_FORMAT;
        }

        long executable_length = vfs_read(&executable, 0U, executable_buffer,
                                          (size_t)executable.size);
        if (executable_length < 0) {
                return (uint64_t)(int64_t)executable_length;
        }
        if ((uint64_t)executable_length != executable.size) {
                return (uint64_t)-(int64_t)USER_ERROR_IO;
        }

        if (exec_replacement.address_space != NULL ||
            exec_replacement.stack_page != NULL ||
            exec_replacement.loaded_image.page_count != 0U) {
                panic("Stale execve replacement image");
        }

        bytes_zero(&exec_replacement, sizeof(exec_replacement));
        exec_replacement.context.sstatus = SSTATUS_SPIE;
        exec_replacement.address_space = virtual_memory_create_address_space();
        exec_replacement.stack_page = page_alloc();

        if (exec_replacement.address_space == NULL ||
            exec_replacement.stack_page == NULL) {
                process_release_replacement();
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        struct page_table *root = exec_replacement.address_space;

        if (!process_build_initial_stack(exec_replacement.stack_page,
                                         &exec_replacement.context, path,
                                         startup)) {
                process_release_replacement();
                return (uint64_t)-(int64_t)USER_ERROR_ARGUMENT_LIST_TOO_LONG;
        }
        if (!page_table_map(root, USER_STACK_ADDRESS,
                            (uintptr_t)exec_replacement.stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE)) {
                process_release_replacement();
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }
        if (!elf_load_image(root, executable_buffer, (size_t)executable.size,
                            USER_ADDRESS_MIN, USER_STACK_GUARD_ADDRESS,
                            &exec_replacement.loaded_image)) {
                process_release_replacement();
                return (uint64_t)-(int64_t)USER_ERROR_EXEC_FORMAT;
        }

        exec_replacement.heap_start =
            process_heap_start(&exec_replacement.loaded_image);
        exec_replacement.heap_break = exec_replacement.heap_start;
        exec_replacement.context.sepc = exec_replacement.loaded_image.entry;
        process_verify_address_space(root, exec_replacement.stack_page);
        return 0U;
}

static void loaded_image_move(struct elf_loaded_image *destination,
                              struct elf_loaded_image *source) {
        if (destination->page_count != 0U) {
                panic("Replacing a live ELF ownership record");
        }

        destination->entry = source->entry;
        destination->page_count = source->page_count;
        for (size_t index = 0U; index < source->page_count; index++) {
                destination->pages[index].virtual_address =
                    source->pages[index].virtual_address;
                destination->pages[index].physical_page =
                    source->pages[index].physical_page;
                destination->pages[index].flags = source->pages[index].flags;
        }
        bytes_zero(source, sizeof(*source));
}

/* Switch to the validated root before destroying the old page table. Open
 * descriptors, PID, scheduling state, and the current supervisor trap stack
 * intentionally remain properties of the same process. */
static void process_commit_replacement(struct trap_frame *frame) {
        struct page_table *new_address_space = exec_replacement.address_space;
        void *new_stack_page = exec_replacement.stack_page;
        struct page_table *old_address_space = active_process->address_space;
        void *old_stack_page = active_process->stack_page;
        uintptr_t old_heap_start = active_process->heap_start;
        uintptr_t old_heap_break = active_process->heap_break;

        graphics_release_if_owned(active_process->pid);
        page_table_activate(new_address_space);
        process_release_image(&old_address_space, &active_process->loaded_image,
                              &old_stack_page, &old_heap_start,
                              &old_heap_break);

        active_process->address_space = new_address_space;
        active_process->stack_page = new_stack_page;
        active_process->heap_start = exec_replacement.heap_start;
        active_process->heap_break = exec_replacement.heap_break;
        exec_replacement.address_space = NULL;
        exec_replacement.stack_page = NULL;
        exec_replacement.heap_start = 0U;
        exec_replacement.heap_break = 0U;
        loaded_image_move(&active_process->loaded_image,
                          &exec_replacement.loaded_image);

        trap_frame_copy(&active_process->context, &exec_replacement.context);
        trap_frame_copy(frame, &exec_replacement.context);
        bytes_zero(&exec_replacement.context, sizeof(exec_replacement.context));
        process_signals_reset_on_exec(active_process);
}

static uint64_t syscall_execve(struct trap_frame *frame, uintptr_t user_path,
                               uintptr_t user_arguments,
                               uintptr_t user_environment) {
        char path[USER_PATH_MAX];
        uint64_t result = user_copy_path(user_path, path);

        if (result != 0U) {
                return result;
        }

        size_t string_buffer_offset = 0U;
        size_t argument_count;
        size_t environment_count;

        result = user_copy_startup_vector(
            user_arguments, startup_arguments, PROCESS_ARGUMENT_LIMIT, true,
            &argument_count, &string_buffer_offset);
        if (result != 0U) {
                return result;
        }
        result = user_copy_startup_vector(
            user_environment, startup_environment, PROCESS_ENVIRONMENT_LIMIT,
            false, &environment_count, &string_buffer_offset);
        if (result != 0U) {
                return result;
        }

        const struct user_process_startup startup = {
            .arguments = startup_arguments,
            .argument_count = argument_count,
            .environment = startup_environment,
            .environment_count = environment_count,
        };

        result = process_prepare_replacement(path, &startup);
        if (result != 0U) {
                return result;
        }

        process_descriptors_close_on_exec(active_process);
        process_commit_replacement(frame);
        return 0U;
}

/* Spawn constructs a separate image and records the caller as its parent. The
 * child starts READY and is scheduled normally after this syscall returns. */
static uint64_t syscall_spawn(uintptr_t user_path, uintptr_t user_arguments,
                              uintptr_t user_environment) {
        char path[USER_PATH_MAX];
        uint64_t result = user_copy_path(user_path, path);

        if (result != 0U) {
                return result;
        }

        size_t string_buffer_offset = 0U;
        size_t argument_count;
        size_t environment_count;

        result = user_copy_startup_vector(
            user_arguments, startup_arguments, PROCESS_ARGUMENT_LIMIT, true,
            &argument_count, &string_buffer_offset);
        if (result != 0U) {
                return result;
        }
        result = user_copy_startup_vector(
            user_environment, startup_environment, PROCESS_ENVIRONMENT_LIMIT,
            false, &environment_count, &string_buffer_offset);
        if (result != 0U) {
                return result;
        }

        struct process *child = process_find_available_slot();
        if (child == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
        }

        const struct user_process_startup startup = {
            .arguments = startup_arguments,
            .argument_count = argument_count,
            .environment = startup_environment,
            .environment_count = environment_count,
        };
        uint64_t parent_pid = active_process->pid;

        result =
            process_create(child, path, &startup, parent_pid, active_process);
        if (result != 0U) {
                bytes_zero(child, sizeof(*child));
                return result;
        }

        size_t directory_length =
            string_length(active_process->current_directory) + 1U;
        for (size_t index = 0U; index < directory_length; index++) {
                child->current_directory[index] =
                    active_process->current_directory[index];
        }

        return child->pid;
}

static bool process_is_waitable_child(const struct process *process,
                                      int64_t requested_pid) {
        if (process->state == PROCESS_UNUSED || active_process == NULL ||
            process->parent_pid != active_process->pid) {
                return false;
        }

        if (requested_pid > 0) {
                return process->pid == (uint64_t)requested_pid;
        }
        if (requested_pid == -1) {
                return true;
        }
        if (requested_pid == 0) {
                return process->process_group == active_process->process_group;
        }

        return requested_pid != INT64_MIN &&
               process->process_group == (uint64_t)-requested_pid;
}

/* Leave sepc at ECALL while blocking. A child exit wakes the parent, which
 * re-enters this function and atomically consumes the retained zombie status.
 */
static void syscall_waitpid(struct trap_frame *frame, int64_t requested_pid,
                            uintptr_t user_status, uint32_t options) {
        const uint32_t supported_options = (uint32_t)USER_WAIT_NO_HANG |
                                           (uint32_t)USER_WAIT_UNTRACED |
                                           (uint32_t)USER_WAIT_CONTINUED;
        if ((options & ~supported_options) != 0U ||
            requested_pid == INT64_MIN) {
                frame->a0 = (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                frame->sepc += 4U;
                return;
        }
        bool found_child = false;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *child = &process_table[index];

                if (!process_is_waitable_child(child, requested_pid)) {
                        continue;
                }
                found_child = true;

                bool exited = child->state == PROCESS_EXITED;
                bool stopped = child->stop_event &&
                               (options & (uint32_t)USER_WAIT_UNTRACED) != 0U;
                bool continued =
                    child->continued_event &&
                    (options & (uint32_t)USER_WAIT_CONTINUED) != 0U;
                if (!exited && !stopped && !continued) {
                        continue;
                }
                if (user_status != 0U &&
                    !user_range_is_valid(user_status, sizeof(int),
                                         VM_PAGE_WRITE)) {
                        frame->a0 = (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                        frame->sepc += 4U;
                        return;
                }

                uint64_t child_pid = child->pid;
                int wait_status;
                if (exited) {
                        wait_status =
                            child->termination_signal != 0U
                                ? (int)(child->termination_signal & 0x7fU)
                                : (int)((child->exit_status & UINT64_C(0xff))
                                        << 8U);
                } else if (stopped) {
                        wait_status =
                            (int)((child->stop_signal & 0xffU) << 8U) | 0x7f;
                } else {
                        wait_status = 0xffff;
                }

                if (user_status != 0U) {
                        user_copy_to(user_status, (const uint8_t *)&wait_status,
                                     sizeof(wait_status));
                }
                if (exited) {
                        process_release_resources(child);
                        bytes_zero(child, sizeof(*child));
                } else if (stopped) {
                        child->stop_event = false;
                        if (child->state != PROCESS_STOPPED) {
                                child->stop_signal = 0U;
                        }
                } else {
                        child->continued_event = false;
                }
                frame->a0 = child_pid;
                frame->sepc += 4U;
                return;
        }

        if (!found_child) {
                frame->a0 = (uint64_t)-(int64_t)USER_ERROR_NO_CHILD;
                frame->sepc += 4U;
                return;
        }
        if (user_status != 0U &&
            !user_range_is_valid(user_status, sizeof(int), VM_PAGE_WRITE)) {
                frame->a0 = (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                frame->sepc += 4U;
                return;
        }
        if ((options & (uint32_t)USER_WAIT_NO_HANG) != 0U) {
                frame->a0 = 0U;
                frame->sepc += 4U;
                return;
        }

        scheduler_block_current(frame, SCHEDULER_WAIT_CHILD);
}

/*
 * Copy a validated user write into process-owned memory. This is essential for
 * blocking: the driver and scheduler never retain a pointer into the transient
 * trap frame, and another address space may run before this syscall completes.
 */
static uint64_t syscall_write_begin(uint64_t descriptor, uintptr_t user_buffer,
                                    size_t length) {
        if (active_process == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file == NULL || (open_file->access & DESCRIPTOR_WRITE) == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (length > USER_IO_MAX) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (!user_range_is_valid(user_buffer, length, VM_PAGE_READ)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        if (open_file->pipe != NULL) {
                user_copy_from((uint8_t *)active_process->write_buffer,
                               user_buffer, length);
                active_process->pending_write = length != 0U;
                active_process->write_descriptor = (size_t)descriptor;
                active_process->write_result_length = length;
                active_process->write_length = length;
                active_process->write_offset = 0U;
                return length;
        }

        if (open_file->file.type == VFS_NODE_REGULAR) {
                user_copy_from((uint8_t *)active_process->write_buffer,
                               user_buffer, length);
                long result = vfs_write(&open_file->file, open_file->offset,
                                        active_process->write_buffer, length);
                if (result > 0) {
                        open_file->offset += (uint64_t)result;
                }
                return (uint64_t)(int64_t)result;
        }
        if (open_file->file.type != VFS_NODE_CHARACTER_DEVICE ||
            open_file->file.operations == NULL ||
            open_file->file.operations->write_byte == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
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
        process->write_descriptor = (size_t)descriptor;
        process->write_result_length = length;
        process->write_length = transmit_length;
        process->write_offset = 0U;

        return length;
}

static uint64_t syscall_open(uintptr_t user_path, uint32_t flags) {
        char path[USER_PATH_MAX];
        uint64_t copy_result = user_copy_path(user_path, path);

        if (copy_result != 0U) {
                return copy_result;
        }

        size_t descriptor = PROCESS_FIRST_OPEN_DESCRIPTOR;

        while (descriptor < PROCESS_DESCRIPTOR_LIMIT &&
               active_process->descriptors[descriptor].open_file != NULL) {
                descriptor++;
        }
        if (descriptor == PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_TOO_MANY_FILES;
        }

        struct vfs_file file;
        int open_result = vfs_open(path, flags, &file);
        if (open_result != 0) {
                return (uint64_t)(int64_t)open_result;
        }

        uint8_t access = 0U;
        if ((flags & VFS_OPEN_READ) != 0U) {
                access |= DESCRIPTOR_READ;
        }
        if ((flags & VFS_OPEN_WRITE) != 0U) {
                access |= DESCRIPTOR_WRITE;
        }

        struct process_open_file *open_file = process_open_file_allocate();
        if (open_file == NULL) {
                panic("Descriptor exists without open-file capacity");
        }
        open_file->access = access;
        open_file->file = file;
        process_descriptor_install(&active_process->descriptors[descriptor],
                                   open_file);
        return descriptor;
}

static uint64_t syscall_close(uint64_t descriptor) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            active_process->descriptors[(size_t)descriptor].open_file == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        process_descriptor_close(
            &active_process->descriptors[(size_t)descriptor]);
        return 0U;
}

static uint64_t syscall_read_begin(uint64_t descriptor, uintptr_t user_buffer,
                                   size_t length) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file == NULL || (open_file->access & DESCRIPTOR_READ) == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (length > USER_IO_MAX) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (!user_range_is_valid(user_buffer, length, VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        if (length == 0U) {
                return 0U;
        }

        if (open_file->pipe != NULL) {
                active_process->pending_read = true;
                active_process->read_descriptor = (size_t)descriptor;
                active_process->read_buffer = user_buffer;
                active_process->read_length = length;
                return 0U;
        }

        if (open_file->file.type == VFS_NODE_REGULAR) {
                long result = vfs_read(&open_file->file, open_file->offset,
                                       active_process->write_buffer, length);
                if (result > 0) {
                        user_copy_to(
                            user_buffer,
                            (const uint8_t *)active_process->write_buffer,
                            (size_t)result);
                        open_file->offset += (uint64_t)result;
                }
                return (uint64_t)(int64_t)result;
        }
        if (open_file->file.type != VFS_NODE_CHARACTER_DEVICE ||
            open_file->file.operations == NULL ||
            open_file->file.operations->read_byte == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        active_process->pending_read = true;
        active_process->read_descriptor = (size_t)descriptor;
        active_process->read_buffer = user_buffer;
        active_process->read_length = length;
        if (open_file->file.device == VFS_DEVICE_CONSOLE &&
            active_process->process_group !=
                terminal_foreground_process_group) {
                (void)syscall_kill(0, USER_SIGNAL_BACKGROUND_READ);
        }
        return 0U;
}

static uint64_t user_copy_file_status(const struct vfs_stat *status,
                                      uintptr_t user_status) {
        if (!user_range_is_valid(user_status, sizeof(struct user_file_status),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        struct user_file_status user;
        bytes_zero(&user, sizeof(user));
        user.size = status->size;
        user.inode = status->inode;
        user.mode = status->mode;
        user.type = (uint32_t)status->type;
        user_copy_to(user_status, (const uint8_t *)&user, sizeof(user));
        return 0U;
}

static uint64_t syscall_stat(uintptr_t user_path, uintptr_t user_status) {
        char path[USER_PATH_MAX];
        uint64_t copy_result = user_copy_path(user_path, path);
        if (copy_result != 0U) {
                return copy_result;
        }
        if (!user_range_is_valid(user_status, sizeof(struct user_file_status),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct vfs_stat status;
        int result = vfs_stat_path(path, &status);
        return result == 0 ? user_copy_file_status(&status, user_status)
                           : (uint64_t)(int64_t)result;
}

static uint64_t syscall_fstat(uint64_t descriptor, uintptr_t user_status) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_status, sizeof(struct user_file_status),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        if (open_file->pipe != NULL) {
                struct user_file_status pipe_status;

                bytes_zero(&pipe_status, sizeof(pipe_status));
                pipe_status.size = open_file->pipe->count;
                pipe_status.type = USER_FILE_PIPE;
                user_copy_to(user_status, (const uint8_t *)&pipe_status,
                             sizeof(pipe_status));
                return 0U;
        }

        struct vfs_stat status;
        int result = vfs_stat_file(&open_file->file, &status);
        return result == 0 ? user_copy_file_status(&status, user_status)
                           : (uint64_t)(int64_t)result;
}

static uint64_t syscall_lseek(uint64_t descriptor, int64_t adjustment,
                              uint32_t whence) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            active_process->descriptors[(size_t)descriptor].open_file == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file->pipe != NULL ||
            open_file->file.type != VFS_NODE_REGULAR) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        uint64_t base;
        if (whence == USER_SEEK_SET) {
                base = 0U;
        } else if (whence == USER_SEEK_CURRENT) {
                base = open_file->offset;
        } else if (whence == USER_SEEK_END) {
                struct vfs_stat status;
                int result = vfs_stat_file(&open_file->file, &status);
                if (result != 0) {
                        return (uint64_t)(int64_t)result;
                }
                base = status.size;
        } else {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        uint64_t position;
        if (adjustment < 0) {
                uint64_t magnitude = (uint64_t)(-(adjustment + 1)) + 1U;
                if (magnitude > base) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                position = base - magnitude;
        } else {
                if ((uint64_t)adjustment > UINT64_MAX - base ||
                    base + (uint64_t)adjustment > INT64_MAX) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                position = base + (uint64_t)adjustment;
        }
        open_file->offset = position;
        return position;
}

static uint64_t syscall_read_directory(uint64_t descriptor,
                                       uintptr_t user_entry) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *directory =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (directory == NULL || (directory->access & DESCRIPTOR_READ) == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (directory->pipe != NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_NOT_DIRECTORY;
        }
        if (!user_range_is_valid(user_entry,
                                 sizeof(struct user_directory_entry),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct vfs_directory_entry entry;
        bytes_zero(&entry, sizeof(entry));
        long next =
            vfs_read_directory(&directory->file, directory->offset, &entry);
        if (next <= 0) {
                return (uint64_t)(int64_t)next;
        }
        struct user_directory_entry user;
        bytes_zero(&user, sizeof(user));
        user.inode = entry.inode;
        user.type = (uint32_t)entry.type;
        for (size_t index = 0U; index < sizeof(user.name); index++) {
                user.name[index] = entry.name[index];
        }
        directory->offset = (uint64_t)next;
        user_copy_to(user_entry, (const uint8_t *)&user, sizeof(user));
        return 1U;
}

static uint64_t syscall_path_operation(uintptr_t user_path,
                                       bool make_directory) {
        char path[USER_PATH_MAX];
        uint64_t copy_result = user_copy_path(user_path, path);
        if (copy_result != 0U) {
                return copy_result;
        }
        int result =
            make_directory ? vfs_make_directory(path) : vfs_unlink(path);
        return (uint64_t)(int64_t)result;
}

static uint64_t syscall_chdir(uintptr_t user_path) {
        char path[USER_PATH_MAX];
        uint64_t copy_result = user_copy_path(user_path, path);
        if (copy_result != 0U) {
                return copy_result;
        }

        struct vfs_stat status;
        int result = vfs_stat_path(path, &status);
        if (result != 0) {
                return (uint64_t)(int64_t)result;
        }
        if (status.type != VFS_NODE_DIRECTORY) {
                return (uint64_t)-(int64_t)USER_ERROR_NOT_DIRECTORY;
        }

        size_t length = string_length(path) + 1U;
        for (size_t index = 0U; index < length; index++) {
                active_process->current_directory[index] = path[index];
        }
        return 0U;
}

/* Like Linux's raw getcwd syscall, success returns the byte count including
 * the terminating null character. */
static uint64_t syscall_getcwd(uintptr_t user_buffer, size_t size) {
        size_t length = string_length(active_process->current_directory) + 1U;
        if (size < length) {
                return (uint64_t)-(int64_t)USER_ERROR_RANGE;
        }
        if (!user_range_is_valid(user_buffer, length, VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        user_copy_to(user_buffer,
                     (const uint8_t *)active_process->current_directory,
                     length);
        return length;
}

static uint64_t syscall_dup(uint64_t old_descriptor) {
        if (old_descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_open_file *open_file =
            active_process->descriptors[(size_t)old_descriptor].open_file;
        if (open_file == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        size_t new_descriptor = 0U;
        while (new_descriptor < PROCESS_DESCRIPTOR_LIMIT &&
               active_process->descriptors[new_descriptor].open_file != NULL) {
                new_descriptor++;
        }
        if (new_descriptor == PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_TOO_MANY_FILES;
        }

        process_descriptor_install(&active_process->descriptors[new_descriptor],
                                   open_file);
        return new_descriptor;
}

static uint64_t syscall_dup2(uint64_t old_descriptor, uint64_t new_descriptor) {
        if (old_descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            active_process->descriptors[(size_t)old_descriptor].open_file ==
                NULL ||
            new_descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (old_descriptor == new_descriptor) {
                return new_descriptor;
        }

        struct process_descriptor *target =
            &active_process->descriptors[(size_t)new_descriptor];
        if (target->open_file != NULL) {
                process_descriptor_close(target);
        }
        process_descriptor_install(
            target,
            active_process->descriptors[(size_t)old_descriptor].open_file);
        return new_descriptor;
}

static uint64_t syscall_pipe(uintptr_t user_descriptors) {
        if (!user_range_is_valid(user_descriptors, 2U * sizeof(int),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        size_t descriptor_pair[2];
        size_t descriptor_count = 0U;

        for (size_t index = 0U; index < PROCESS_DESCRIPTOR_LIMIT; index++) {
                if (active_process->descriptors[index].open_file == NULL &&
                    descriptor_count < 2U) {
                        descriptor_pair[descriptor_count++] = index;
                }
        }
        if (descriptor_count != 2U) {
                return (uint64_t)-(int64_t)USER_ERROR_TOO_MANY_FILES;
        }

        size_t free_open_files = 0U;
        for (size_t index = 0U; index < PROCESS_OPEN_FILE_LIMIT; index++) {
                if (!open_file_table[index].used) {
                        free_open_files++;
                }
        }
        if (free_open_files < 2U) {
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }

        struct process_pipe *pipe = process_pipe_allocate();
        if (pipe == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }

        struct process_open_file *read_end = process_open_file_allocate();
        struct process_open_file *write_end = process_open_file_allocate();
        if (read_end == NULL || write_end == NULL) {
                panic("Reserved pipe open-file records disappeared");
        }

        read_end->access = DESCRIPTOR_READ;
        read_end->pipe = pipe;
        process_pipe_retain(pipe, read_end->access);
        write_end->access = DESCRIPTOR_WRITE;
        write_end->pipe = pipe;
        process_pipe_retain(pipe, write_end->access);
        process_descriptor_install(
            &active_process->descriptors[descriptor_pair[0]], read_end);
        process_descriptor_install(
            &active_process->descriptors[descriptor_pair[1]], write_end);

        int result[2] = {(int)descriptor_pair[0], (int)descriptor_pair[1]};
        user_copy_to(user_descriptors, (const uint8_t *)result, sizeof(result));
        return 0U;
}

static uint64_t syscall_set_descriptor_flags(uint64_t descriptor,
                                             uint32_t flags) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            active_process->descriptors[(size_t)descriptor].open_file == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if ((flags & ~(uint32_t)USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        active_process->descriptors[(size_t)descriptor].close_on_exec =
            (flags & USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0U;
        return 0U;
}

/* Move the end of the process data segment. Heap pages are materialized
 * eagerly so a successful return guarantees that every byte below the new
 * break is writable. Growth is transactional; shrink releases complete pages
 * while retaining the partial page which still contains live heap bytes. */
static uint64_t syscall_brk(uintptr_t requested_break) {
        if (requested_break == 0U) {
                return active_process->heap_break;
        }
        if (requested_break < active_process->heap_start) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (requested_break > USER_STACK_GUARD_ADDRESS) {
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        uintptr_t old_mapped_end = align_up_to_page(active_process->heap_break);
        uintptr_t new_mapped_end = align_up_to_page(requested_break);

        if (new_mapped_end < old_mapped_end) {
                process_free_heap_pages(active_process->address_space,
                                        new_mapped_end, old_mapped_end);
        } else if (new_mapped_end > old_mapped_end) {
                uintptr_t mapped_end = old_mapped_end;

                while (mapped_end < new_mapped_end) {
                        void *page = page_alloc();

                        if (page == NULL) {
                                process_free_heap_pages(
                                    active_process->address_space,
                                    old_mapped_end, mapped_end);
                                return (uint64_t)-(
                                    int64_t)USER_ERROR_OUT_OF_MEMORY;
                        }
                        if (!page_table_map(active_process->address_space,
                                            mapped_end, (uintptr_t)page,
                                            VM_PAGE_USER | VM_PAGE_READ |
                                                VM_PAGE_WRITE)) {
                                page_free(page);
                                process_free_heap_pages(
                                    active_process->address_space,
                                    old_mapped_end, mapped_end);
                                return (uint64_t)-(
                                    int64_t)USER_ERROR_OUT_OF_MEMORY;
                        }

                        mapped_end += PAGE_SIZE;
                }
        }

        active_process->heap_break = requested_break;
        return requested_break;
}

static uint64_t syscall_graphics_map(uintptr_t user_information) {
        if (!virtio_gpu_available()) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_ENTRY;
        }
        if (!user_range_is_valid(user_information,
                                 sizeof(struct user_graphics_info),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        if (graphics_owner_pid != 0U &&
            graphics_owner_pid != active_process->pid) {
                return (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
        }

        uintptr_t framebuffer = (uintptr_t)virtio_gpu_framebuffer();
        uintptr_t existing;
        if (page_table_translate(active_process->address_space,
                                 USER_GRAPHICS_ADDRESS, &existing, NULL)) {
                if (existing != framebuffer) {
                        return (uint64_t)-(int64_t)USER_ERROR_EXISTS;
                }
        } else if (!page_table_map_range(
                       active_process->address_space, USER_GRAPHICS_ADDRESS,
                       framebuffer, virtio_gpu_framebuffer_size(),
                       VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        struct user_graphics_info information;
        bytes_zero(&information, sizeof(information));
        information.framebuffer = USER_GRAPHICS_ADDRESS;
        information.framebuffer_size =
            (uint64_t)virtio_gpu_stride() * virtio_gpu_height();
        information.width = virtio_gpu_width();
        information.height = virtio_gpu_height();
        information.stride = virtio_gpu_stride();
        information.pixel_format = USER_GRAPHICS_PIXEL_XRGB8888;
        graphics_owner_pid = active_process->pid;
        graphics_console_suspend();
        input_event_clear();
        input_set_console_captured(true);
        user_copy_to(user_information, (const uint8_t *)&information,
                     sizeof(information));
        return 0U;
}

static uint64_t syscall_graphics_flush(uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height) {
        if (graphics_owner_pid != active_process->pid) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        if (!virtio_gpu_flush(x, y, width, height)) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        return 0U;
}

static uint64_t syscall_input_read(uintptr_t user_event) {
        if (graphics_owner_pid != active_process->pid) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        if (!user_range_is_valid(user_event, sizeof(struct user_input_event),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        struct user_input_event event;
        if (!input_event_pop(&event)) {
                return 0U;
        }
        user_copy_to(user_event, (const uint8_t *)&event, sizeof(event));
        return 1U;
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

bool scheduler_wake_one(enum scheduler_wait_channel channel) {
        if (channel == SCHEDULER_WAIT_NONE) {
                return false;
        }

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_BLOCKED &&
                    process->wait_channel == channel) {
                        process->state = PROCESS_READY;
                        process->wait_channel = SCHEDULER_WAIT_NONE;
                        return true;
                }
        }

        return false;
}

/* Advance one resumable write. While it is blocked, sepc continues to point at
 * ECALL; waking the process safely re-enters this continuation in a fresh trap.
 */
static void process_continue_write(struct trap_frame *frame) {
        struct process *process = active_process;

        if (process == NULL || !process->pending_write ||
            process->write_descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                panic_trap("Invalid pending write", frame);
        }

        const struct process_open_file *open_file =
            process->descriptors[process->write_descriptor].open_file;
        if (open_file == NULL) {
                panic_trap("Pending write descriptor was closed", frame);
        }

        if (open_file->pipe != NULL) {
                struct process_pipe *pipe = open_file->pipe;

                if (pipe->readers == 0U) {
                        process->pending_write = false;
                        frame->a0 = (uint64_t)-(int64_t)USER_ERROR_BROKEN_PIPE;
                        frame->sepc += 4U;
                        return;
                }
                if (process->write_length > PIPE_BUFFER_SIZE - pipe->count) {
                        scheduler_block_current(frame,
                                                SCHEDULER_WAIT_PIPE_WRITE);
                        return;
                }

                for (size_t offset = 0U; offset < process->write_length;
                     offset++) {
                        pipe->buffer[pipe->write_offset] =
                            (uint8_t)process->write_buffer[offset];
                        pipe->write_offset =
                            (pipe->write_offset + 1U) % PIPE_BUFFER_SIZE;
                }
                pipe->count += process->write_length;
                process->pending_write = false;
                frame->a0 = process->write_result_length;
                frame->sepc += 4U;
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_READ);
                return;
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

        const struct vfs_file *file = &open_file->file;

        if (file->operations->write_byte(
                process->write_buffer[process->write_offset])) {
                process->write_offset++;
        }

        scheduler_block_current(frame, SCHEDULER_WAIT_UART_TX);
}

static void process_continue_read(struct trap_frame *frame) {
        struct process *process = active_process;

        if (process == NULL || !process->pending_read ||
            process->read_descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                panic_trap("Invalid pending read", frame);
        }

        size_t count = 0U;
        char character;

        const struct process_open_file *open_file =
            process->descriptors[process->read_descriptor].open_file;
        if (open_file == NULL) {
                panic_trap("Pending read descriptor was closed", frame);
        }

        if (open_file->pipe != NULL) {
                struct process_pipe *pipe = open_file->pipe;

                if (pipe->count == 0U) {
                        if (pipe->writers == 0U) {
                                process->pending_read = false;
                                frame->a0 = 0U;
                                frame->sepc += 4U;
                                return;
                        }

                        scheduler_block_current(frame,
                                                SCHEDULER_WAIT_PIPE_READ);
                        return;
                }

                size_t pipe_count = process->read_length;
                if (pipe_count > pipe->count) {
                        pipe_count = pipe->count;
                }
                for (size_t offset = 0U; offset < pipe_count; offset++) {
                        process->write_buffer[offset] =
                            (char)pipe->buffer[pipe->read_offset];
                        pipe->read_offset =
                            (pipe->read_offset + 1U) % PIPE_BUFFER_SIZE;
                }
                pipe->count -= pipe_count;
                user_copy_to(process->read_buffer,
                             (const uint8_t *)process->write_buffer,
                             pipe_count);
                process->pending_read = false;
                frame->a0 = pipe_count;
                frame->sepc += 4U;
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_WRITE);
                return;
        }
        const struct vfs_file *file = &open_file->file;

        while (count < process->read_length &&
               file->operations->read_byte(&character)) {
                uint8_t byte = (uint8_t)character;

                user_copy_to(process->read_buffer + count, &byte, 1U);
                count++;
        }

        if (count == 0U) {
                scheduler_block_current(frame, SCHEDULER_WAIT_UART_RX);
                return;
        }

        process->pending_read = false;
        frame->a0 = count;
        frame->sepc += 4U;
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
 * Convert the active process into an EXITED zombie. If no READY process
 * remains, redirect trap return to user_mode_resume in supervisor mode. That
 * assembly continuation restores the kernel call context saved by
 * user_mode_enter.
 */
static void user_process_finish(struct trap_frame *frame, uint64_t status) {
        if (active_process == NULL ||
            active_process->state != PROCESS_RUNNING ||
            user_saved_kernel_context_sp == 0U) {
                panic_trap("Invalid user process return state", frame);
        }

        graphics_release_if_owned(active_process->pid);
        if (uart_write_owner == active_process) {
                uart_write_owner = NULL;
                (void)scheduler_wake_all(SCHEDULER_WAIT_UART_TX);
        }
        active_process->pending_write = false;
        active_process->pending_read = false;
        active_process->state = PROCESS_EXITED;
        active_process->exit_status = status;
        process_orphan_children(active_process->pid);
        (void)scheduler_wake_all(SCHEDULER_WAIT_CHILD);

        struct process *next = scheduler_find_next_ready();

        if (next != NULL) {
                scheduler_context_switches++;
                scheduler_switch_to(next, frame);
                return;
        }

        scheduler_return_to_kernel(frame);
}

/* Preserve the complete interrupted frame without releasing resources. A
 * parent waiting with WUNTRACED observes the retained stop event, and SIGCONT
 * later moves this same process back to READY. */
static void process_stop_current(struct trap_frame *frame, uint32_t signal) {
        if (active_process == NULL ||
            active_process->state != PROCESS_RUNNING ||
            user_saved_kernel_context_sp == 0U) {
                panic_trap("Invalid user process stop state", frame);
        }

        struct process *stopped = active_process;
        if (uart_write_owner == stopped) {
                uart_write_owner = NULL;
                (void)scheduler_wake_all(SCHEDULER_WAIT_UART_TX);
        }
        trap_frame_copy(&stopped->context, frame);
        stopped->state = PROCESS_STOPPED;
        stopped->wait_channel = SCHEDULER_WAIT_NONE;
        stopped->stop_signal = signal;
        stopped->stop_event = true;
        stopped->continued_event = false;
        (void)scheduler_wake_all(SCHEDULER_WAIT_CHILD);

        struct process *next = scheduler_find_next_ready();
        if (next != NULL) {
                scheduler_context_switches++;
                scheduler_switch_to(next, frame);
                return;
        }

        scheduler_return_to_kernel(frame);
}

static uint32_t process_next_pending_signal(const struct process *process) {
        if ((process->pending_signals & signal_bit(USER_SIGNAL_KILL)) != 0U) {
                return USER_SIGNAL_KILL;
        }

        for (uint32_t signal = 1U; signal <= USER_SIGNAL_MAX; signal++) {
                if ((process->pending_signals & signal_bit(signal)) != 0U) {
                        return signal;
                }
        }

        return 0U;
}

/* Deliver at the final trap-return boundary, after any scheduling decision has
 * selected the address space represented by frame. Ignored signals are drained
 * immediately; default termination may select another process and continue. */
static void process_deliver_pending_signals(struct trap_frame *frame) {
        while (user_process_is_active()) {
                struct process *process = active_process;
                uint32_t signal = process_next_pending_signal(process);

                if (signal == 0U ||
                    (process->signal_active && signal != USER_SIGNAL_KILL &&
                     signal != USER_SIGNAL_STOP)) {
                        return;
                }

                process->pending_signals &= ~signal_bit(signal);
                const struct process_signal_disposition *disposition =
                    &process->signal_dispositions[signal];

                if (disposition->handler == USER_SIGNAL_IGNORE &&
                    signal != USER_SIGNAL_KILL && signal != USER_SIGNAL_STOP) {
                        continue;
                }
                if (signal == USER_SIGNAL_CONTINUE &&
                    disposition->handler == USER_SIGNAL_DEFAULT) {
                        continue;
                }
                if (process_signal_stops_by_default(signal) &&
                    (disposition->handler == USER_SIGNAL_DEFAULT ||
                     signal == USER_SIGNAL_STOP)) {
                        process_stop_current(frame, signal);
                        continue;
                }
                if (disposition->handler == USER_SIGNAL_DEFAULT ||
                    signal == USER_SIGNAL_KILL) {
                        process->termination_signal = signal;
                        user_process_finish(frame, 128U + signal);
                        continue;
                }

                trap_frame_copy(&process->signal_context, frame);
                process->signal_active = true;
                frame->sepc = disposition->handler;
                frame->ra = disposition->restorer;
                frame->a0 = signal;
                frame->a1 = 0U;
                frame->a2 = 0U;
                trap_frame_copy(&process->context, frame);
                return;
        }
}

/* Print a zombie's retained status exactly once. */
static void print_process_exit(const struct process *process) {
        uart_puts("Process ");
        uart_put_uint64(process->pid);
        if (process->termination_signal != 0U) {
                uart_puts(" terminated by signal ");
                uart_put_uint64(process->termination_signal);
        } else {
                uart_puts(" exited with status ");
                uart_put_uint64(process->exit_status);
        }
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

static bool scheduler_has_waiting_processes(void) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                if (process_table[index].state == PROCESS_BLOCKED ||
                    process_table[index].state == PROCESS_STOPPED) {
                        return true;
                }
        }

        return false;
}

static uint64_t scheduler_interrupts_disable(void) {
        uint64_t previous_status;
        uint64_t interrupt_enable = SSTATUS_SIE;

        __asm__ volatile("csrrc %[previous], sstatus, %[mask]"
                         : [previous] "=r"(previous_status)
                         : [mask] "r"(interrupt_enable)
                         : "memory");
        return previous_status;
}

static void scheduler_interrupts_restore(uint64_t previous_status) {
        if ((previous_status & SSTATUS_SIE) != 0U) {
                uint64_t interrupt_enable = SSTATUS_SIE;

                __asm__ volatile("csrs sstatus, %[mask]"
                                 :
                                 : [mask] "r"(interrupt_enable)
                                 : "memory");
        }
}

/*
 * Enter READY processes until no live work remains. user_mode_enter saves the
 * supervisor ABI context and sret enters U-mode; exits and all-blocked states
 * return through user_mode_resume so this loop can finish or wait for an IRQ.
 */
static bool scheduler_run_ready(void) {
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
        while (true) {
                if (first == NULL) {
                        /* Mask trap delivery while deciding between running,
                         * sleeping, and finishing. WFI still resumes for an
                         * individually enabled pending interrupt when SIE is
                         * clear, so an IRQ cannot be consumed between the
                         * final state scan and sleep. */
                        uint64_t previous_status =
                            scheduler_interrupts_disable();
                        first = scheduler_find_next_ready();
                        if (first != NULL) {
                                scheduler_interrupts_restore(previous_status);
                                continue;
                        }
                        if (!scheduler_has_waiting_processes()) {
                                scheduler_interrupts_restore(previous_status);
                                break;
                        }

                        __asm__ volatile("wfi");
                        scheduler_interrupts_restore(previous_status);
                        continue;
                }

                active_process = first;
                active_process->state = PROCESS_RUNNING;
                user_saved_kernel_context_sp = 0U;

                /* Start a fresh quantum rather than inheriting the idle loop's
                 * timer deadline. */
                timer_schedule_next();
                page_table_activate(active_process->address_space);
                user_mode_enter(&active_process->context,
                                (uintptr_t)active_process->kernel_trap_stack +
                                    PAGE_SIZE);

                if (active_process != NULL) {
                        panic("Scheduler returned while a process was running");
                }

                page_table_activate(virtual_memory_kernel_page_table());
                first = NULL;
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

/* Create the initial READY entry before starting the scheduler. */
static bool user_process_spawn(const char *path,
                               const struct user_process_startup *startup,
                               uint64_t *pid) {
        struct process *process = process_find_available_slot();

        if (process == NULL) {
                return false;
        }
        if (process_create(process, path, startup, 0U, NULL) != 0U) {
                bytes_zero(process, sizeof(*process));
                return false;
        }

        if (pid != NULL) {
                *pid = process->pid;
        }

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

/* Spawn the boot executable and run userspace until it exits. */
void user_process_run_path(const char *path,
                           const struct user_process_startup *startup) {
        uint64_t pid;

        if (!user_process_spawn(path, startup, &pid)) {
                uart_puts("Unable to load program: ");
                uart_puts(path);
                uart_putc('\n');
                return;
        }

        (void)pid;
        (void)scheduler_run_ready();
}

bool user_process_is_active(void) {
        if (active_process == NULL) {
                return false;
        }

        return active_process->state == PROCESS_RUNNING;
}

/* Timer interrupts from supervisor mode belong to the kernel and must not
 * be interpreted as user scheduling events. */
void user_process_handle_timer(struct trap_frame *frame) {
        if ((frame->sstatus & SSTATUS_SPP) != 0U || !user_process_is_active()) {
                return;
        }

        scheduler_reschedule(frame, true);
}

/* Dispatch the small user ABI. Blocking write deliberately retains ECALL in
 * sepc until its continuation has completed; other calls advance immediately.
 */
void user_process_handle_syscall(struct trap_frame *frame) {
        if (!user_process_is_active()) {
                panic_trap("U-mode syscall without an active process", frame);
        }
        /* A blocked syscall resumes at its original ECALL. Deliver the signal
         * which woke it before its continuation can put it back to sleep. */
        if (process_has_deliverable_signal(active_process)) {
                return;
        }

        switch (frame->a7) {
        case USER_SYSCALL_WRITE: {
                if (!active_process->pending_write) {
                        uint64_t result = syscall_write_begin(
                            frame->a0, (uintptr_t)frame->a1, (size_t)frame->a2);

                        if (!active_process->pending_write) {
                                frame->a0 = result;
                                frame->sepc += 4U;
                                return;
                        }
                }

                process_continue_write(frame);
                return;
        }

        case USER_SYSCALL_READ: {
                if (!active_process->pending_read) {
                        uint64_t result = syscall_read_begin(
                            frame->a0, (uintptr_t)frame->a1, (size_t)frame->a2);

                        if (!active_process->pending_read) {
                                frame->a0 = result;
                                frame->sepc += 4U;
                                return;
                        }
                        if (process_has_deliverable_signal(active_process)) {
                                return;
                        }
                }

                process_continue_read(frame);
                return;
        }

        case USER_SYSCALL_OPEN:
                frame->a0 =
                    syscall_open((uintptr_t)frame->a0, (uint32_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_CLOSE:
                frame->a0 = syscall_close(frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_STAT:
                frame->a0 =
                    syscall_stat((uintptr_t)frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_FSTAT:
                frame->a0 = syscall_fstat(frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_LSEEK:
                frame->a0 = syscall_lseek(frame->a0, (int64_t)frame->a1,
                                          (uint32_t)frame->a2);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_READ_DIRECTORY:
                frame->a0 =
                    syscall_read_directory(frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_MKDIR:
                frame->a0 = syscall_path_operation((uintptr_t)frame->a0, true);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_UNLINK:
                frame->a0 = syscall_path_operation((uintptr_t)frame->a0, false);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_CHDIR:
                frame->a0 = syscall_chdir((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_GETCWD:
                frame->a0 =
                    syscall_getcwd((uintptr_t)frame->a0, (size_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_DUP:
                frame->a0 = syscall_dup(frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_DUP2:
                frame->a0 = syscall_dup2(frame->a0, frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_PIPE:
                frame->a0 = syscall_pipe((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SET_DESCRIPTOR_FLAGS:
                frame->a0 = syscall_set_descriptor_flags(frame->a0,
                                                         (uint32_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_EXECVE: {
                uint64_t result =
                    syscall_execve(frame, (uintptr_t)frame->a0,
                                   (uintptr_t)frame->a1, (uintptr_t)frame->a2);

                /* Success installed an entirely new frame and begins at its
                 * ELF entry. Only failure resumes after the original ECALL. */
                if (result != 0U) {
                        frame->a0 = result;
                        frame->sepc += 4U;
                }
                return;
        }

        case USER_SYSCALL_GETPID:
                frame->a0 = active_process->pid;
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_WAITPID:
                syscall_waitpid(frame, (int64_t)frame->a0, (uintptr_t)frame->a1,
                                (uint32_t)frame->a2);
                return;

        case USER_SYSCALL_SPAWN:
                frame->a0 =
                    syscall_spawn((uintptr_t)frame->a0, (uintptr_t)frame->a1,
                                  (uintptr_t)frame->a2);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_FORK:
                frame->a0 = syscall_fork(frame);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SIGNAL_ACTION:
                frame->a0 = syscall_signal_action(
                    (int64_t)frame->a0, (uintptr_t)frame->a1,
                    (uintptr_t)frame->a2, (uintptr_t)frame->a3);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_KILL:
                frame->a0 =
                    syscall_kill((int64_t)frame->a0, (int64_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SIGNAL_RETURN: {
                uint64_t result = syscall_signal_return(frame);
                if (result != 0U) {
                        frame->a0 = result;
                        frame->sepc += 4U;
                }
                return;
        }

        case USER_SYSCALL_SET_PROCESS_GROUP:
                frame->a0 = syscall_set_process_group((int64_t)frame->a0,
                                                      (int64_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_GET_PROCESS_GROUP:
                frame->a0 = active_process->process_group;
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_SET_FOREGROUND_GROUP:
                frame->a0 = syscall_terminal_set_foreground_group(
                    frame->a0, (int64_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_GET_FOREGROUND_GROUP:
                frame->a0 = syscall_terminal_get_foreground_group(frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_GRAPHICS_MAP:
                frame->a0 = syscall_graphics_map((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_GRAPHICS_FLUSH:
                frame->a0 = syscall_graphics_flush(
                    (uint32_t)frame->a0, (uint32_t)frame->a1,
                    (uint32_t)frame->a2, (uint32_t)frame->a3);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_INPUT_READ:
                frame->a0 = syscall_input_read((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_BRK:
                frame->a0 = syscall_brk((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

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
                frame->a0 = (uint64_t)-(int64_t)USER_ERROR_NOT_IMPLEMENTED;
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
        process_deliver_pending_signals(frame);

        /* Default termination of the final process redirects this trap back
         * to the suspended supervisor scheduler rather than to U-mode. */
        if (!user_process_is_active()) {
                return;
        }
        if (active_process->kernel_trap_stack == NULL) {
                panic_trap("Invalid scheduled user return", frame);
        }

        frame->kernel_trap_stack_top =
            (uintptr_t)active_process->kernel_trap_stack + PAGE_SIZE;
}
