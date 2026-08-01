/*
 * User-process lifecycle and the foreground round-robin scheduler.
 *
 * A process owns a private Sv39 root, ELF pages, one user stack page, one
 * supervisor-only trap stack, and a small descriptor table. The complete user
 * register set lives in a trap_frame, allowing the trap handler to switch
 * processes by replacing the frame that assembly will restore.
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
        USER_IO_MAX = 1024,
        USER_WRITE_TRANSMIT_MAX = USER_IO_MAX * 2,
        USER_PATH_MAX = 64,
        PROCESS_EXECUTABLE_MAX = 16 * 1024,
        PROCESS_DESCRIPTOR_LIMIT = 8,
        PROCESS_FIRST_OPEN_DESCRIPTOR = 3,
        PROCESS_LIMIT = 8,
        PROCESS_ARGUMENT_LIMIT = 16,
        PROCESS_ENVIRONMENT_LIMIT = 16,
};

enum descriptor_access {
        DESCRIPTOR_READ = (1U << 0),
        DESCRIPTOR_WRITE = (1U << 1),
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

struct process_descriptor {
        bool open;
        uint8_t access;
        uint64_t offset;
        struct vfs_file file;
};

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
        struct process_descriptor descriptors[PROCESS_DESCRIPTOR_LIMIT];

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

/* Fixed slots keep early process management independent of a kernel heap. */
static struct process process_table[PROCESS_LIMIT];
static struct process *active_process;
static struct process *uart_write_owner;
static uint64_t next_pid = 1U;
static uint64_t scheduler_preemptions;
static uint64_t scheduler_context_switches;
static uint64_t scheduler_blocks;
static uint8_t executable_buffer[PROCESS_EXECUTABLE_MAX];

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

static bool process_descriptors_initialize(struct process *process) {
        struct vfs_file console;

        if (vfs_open("/dev/console", VFS_OPEN_READ | VFS_OPEN_WRITE,
                     &console) != 0 ||
            console.type != VFS_NODE_CHARACTER_DEVICE ||
            console.device != VFS_DEVICE_CONSOLE) {
                return false;
        }

        process->descriptors[USER_STDIN_FILENO] =
            (struct process_descriptor){
                .open = true,
                .access = DESCRIPTOR_READ,
                .file = console,
            };
        process->descriptors[USER_STDOUT_FILENO] =
            (struct process_descriptor){
                .open = true,
                .access = DESCRIPTOR_WRITE,
                .file = console,
            };
        process->descriptors[USER_STDERR_FILENO] =
            (struct process_descriptor){
                .open = true,
                .access = DESCRIPTOR_WRITE,
                .file = console,
            };
        return true;
}

static void process_descriptors_close_all(struct process *process) {
        bytes_zero(process->descriptors, sizeof(process->descriptors));
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
 * and exit status for ps. Descriptors close first, the user stack is verified
 * and unmapped explicitly, and ELF teardown checks executable/data ownership.
 */
static void process_release_resources(struct process *process) {
        process_descriptors_close_all(process);

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
        char *destination =
            (char *)stack_page + (*cursor - USER_STACK_ADDRESS);

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
static bool process_build_initial_stack(
    struct process *process, const char *path,
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
                    !stack_copy_string(process->stack_page, &cursor,
                                       arguments[index - 1U],
                                       &argument_addresses[index - 1U])) {
                        return false;
                }
        }

        for (size_t index = environment_count; index != 0U; index--) {
                if (environment[index - 1U] == NULL ||
                    !stack_copy_string(process->stack_page, &cursor,
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

        uint64_t *words = (uint64_t *)((uint8_t *)process->stack_page +
                                       stack_pointer - USER_STACK_ADDRESS);
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

        process->context.sp = stack_pointer;
        process->context.a0 = argument_count;
        process->context.a1 = stack_pointer + sizeof(uint64_t);
        process->context.a2 = process->context.a1 +
                              (argument_count + 1U) * sizeof(uint64_t);
        return true;
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
static bool process_create(struct process *process, const char *path,
                           const struct user_process_startup *startup) {
        struct vfs_file executable;

        if (vfs_open(path, VFS_OPEN_READ, &executable) != 0 ||
            executable.type != VFS_NODE_REGULAR || executable.size == 0U ||
            executable.size > sizeof(executable_buffer)) {
                return false;
        }

        long executable_length = vfs_read(&executable, 0U, executable_buffer,
                                          (size_t)executable.size);
        if (executable_length < 0 ||
            (uint64_t)executable_length != executable.size) {
                return false;
        }

        bytes_zero(process, sizeof(*process));
        process->pid = next_pid;
        process->state = PROCESS_READY;
        /* SPIE causes sret to enable supervisor interrupts while U-mode runs. */
        process->context.sstatus = SSTATUS_SPIE;
        process->exit_status = UINT64_MAX;
        if (!process_descriptors_initialize(process)) {
                panic("Initial console descriptors are unavailable");
        }
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

        if (!process_build_initial_stack(process, path, startup) ||
            !page_table_map(root, USER_STACK_ADDRESS,
                            (uintptr_t)process->stack_page,
                            VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_WRITE) ||
            !elf_load_image(root, executable_buffer, (size_t)executable.size,
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

/* Copy a bounded null-terminated path without dereferencing a user pointer. */
static uint64_t user_copy_path(uintptr_t user_path,
                               char path[USER_PATH_MAX]) {
        for (size_t index = 0U; index < USER_PATH_MAX; index++) {
                uintptr_t address;

                if (user_path > UINTPTR_MAX - index ||
                    !user_range_is_valid(user_path + index, 1U,
                                         VM_PAGE_READ) ||
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
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            !active_process->descriptors[(size_t)descriptor].open ||
            (active_process->descriptors[(size_t)descriptor].access &
             DESCRIPTOR_WRITE) == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (length > USER_IO_MAX) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (!user_range_is_valid(user_buffer, length, VM_PAGE_READ)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        struct process_descriptor *open_file =
            &active_process->descriptors[(size_t)descriptor];
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
               active_process->descriptors[descriptor].open) {
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

        active_process->descriptors[descriptor] =
            (struct process_descriptor){
                .open = true,
                .access = access,
                .file = file,
            };
        return descriptor;
}

static uint64_t syscall_close(uint64_t descriptor) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            !active_process->descriptors[(size_t)descriptor].open) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        bytes_zero(&active_process->descriptors[(size_t)descriptor],
                   sizeof(active_process->descriptors[(size_t)descriptor]));
        return 0U;
}

static uint64_t syscall_read_begin(uint64_t descriptor,
                                   uintptr_t user_buffer, size_t length) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            !active_process->descriptors[(size_t)descriptor].open ||
            (active_process->descriptors[(size_t)descriptor].access &
             DESCRIPTOR_READ) == 0U) {
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

        struct process_descriptor *open_file =
            &active_process->descriptors[(size_t)descriptor];

        if (open_file->file.type == VFS_NODE_REGULAR) {
                long result = vfs_read(&open_file->file, open_file->offset,
                                       active_process->write_buffer, length);
                if (result > 0) {
                        user_copy_to(user_buffer,
                                     (const uint8_t *)active_process
                                         ->write_buffer,
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
        if (result != 0) {
                return (uint64_t)(int64_t)result;
        }
        struct user_file_status user;
        bytes_zero(&user, sizeof(user));
        user.size = status.size;
        user.inode = status.inode;
        user.mode = status.mode;
        user.type = (uint32_t)status.type;
        user_copy_to(user_status, (const uint8_t *)&user, sizeof(user));
        return 0U;
}

static uint64_t syscall_lseek(uint64_t descriptor, int64_t adjustment,
                              uint32_t whence) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            !active_process->descriptors[(size_t)descriptor].open) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        struct process_descriptor *open_file =
            &active_process->descriptors[(size_t)descriptor];
        if (open_file->file.type != VFS_NODE_REGULAR) {
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
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT ||
            !active_process->descriptors[(size_t)descriptor].open ||
            (active_process->descriptors[(size_t)descriptor].access &
             DESCRIPTOR_READ) == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_entry,
                                 sizeof(struct user_directory_entry),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct process_descriptor *directory =
            &active_process->descriptors[(size_t)descriptor];
        struct vfs_directory_entry entry;
        bytes_zero(&entry, sizeof(entry));
        long next = vfs_read_directory(&directory->file, directory->offset,
                                       &entry);
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

static uint64_t syscall_path_operation(uintptr_t user_path, bool make_directory) {
        char path[USER_PATH_MAX];
        uint64_t copy_result = user_copy_path(user_path, path);
        if (copy_result != 0U) {
                return copy_result;
        }
        int result = make_directory ? vfs_make_directory(path)
                                    : vfs_unlink(path);
        return (uint64_t)(int64_t)result;
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
 * ECALL; waking the process safely re-enters this continuation in a fresh trap. */
static void process_continue_write(struct trap_frame *frame) {
        struct process *process = active_process;

        if (process == NULL || !process->pending_write ||
            process->write_descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
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

        const struct vfs_file *file =
            &process->descriptors[process->write_descriptor].file;

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

        const struct vfs_file *file =
            &process->descriptors[process->read_descriptor].file;

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
bool user_process_spawn(const char *path,
                        const struct user_process_startup *startup,
                        uint64_t *pid) {
        struct process *process = process_find_available_slot();

        if (process == NULL) {
                return false;
        }
        if (!process_create(process, path, startup)) {
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

        if (!user_process_spawn("/bin/hello", NULL, &pid)) {
                uart_puts("Unable to create process; run 'reap' and retry\n");
                return;
        }

        (void)pid;
        (void)scheduler_run_ready(false);
}

/* Spawn one executable by absolute VFS path and run it immediately. */
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
                if (!user_process_spawn(programs[index], NULL,
                                        &created_pids[index])) {
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
                            frame->a0, (uintptr_t)frame->a1,
                            (size_t)frame->a2);

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
                            frame->a0, (uintptr_t)frame->a1,
                            (size_t)frame->a2);

                        if (!active_process->pending_read) {
                                frame->a0 = result;
                                frame->sepc += 4U;
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
                frame->a0 = syscall_stat((uintptr_t)frame->a0,
                                         (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_LSEEK:
                frame->a0 = syscall_lseek(frame->a0, (int64_t)frame->a1,
                                          (uint32_t)frame->a2);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_READ_DIRECTORY:
                frame->a0 = syscall_read_directory(frame->a0,
                                                   (uintptr_t)frame->a1);
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
