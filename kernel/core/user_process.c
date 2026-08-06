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
        PROCESS_EXECUTABLE_MAX = 64 * 1024,
        PROCESS_DESCRIPTOR_LIMIT = 16,
        PROCESS_FIRST_OPEN_DESCRIPTOR = 3,
        PROCESS_LIMIT = 48,
        PROCESS_OPEN_FILE_LIMIT = PROCESS_LIMIT * PROCESS_DESCRIPTOR_LIMIT,
        PIPE_LIMIT = 32,
        PIPE_BUFFER_SIZE = USER_IO_MAX,
        PSEUDO_TERMINAL_LIMIT = PIPE_LIMIT / 2,
        PROCESS_ARGUMENT_LIMIT = 16,
        PROCESS_ENVIRONMENT_LIMIT = 16,
        SHARED_MEMORY_OBJECT_LIMIT = 32,
        SHARED_MEMORY_PROCESS_LIMIT = 16,
        SHARED_MEMORY_PAGE_LIMIT = 768,
        PROCESS_ANONYMOUS_MAPPING_LIMIT = 32,
        USER_WAIT_ITEM_LIMIT = PROCESS_LIMIT + 4,
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

enum process_pending_wait {
        PROCESS_WAIT_NONE,
        PROCESS_WAIT_SLEEP,
        PROCESS_WAIT_POLL,
        PROCESS_WAIT_EVENTS,
};

_Static_assert((uint32_t)USER_OPEN_READ == (uint32_t)VFS_OPEN_READ &&
                   (uint32_t)USER_OPEN_WRITE == (uint32_t)VFS_OPEN_WRITE &&
                   (uint32_t)USER_OPEN_CREATE == (uint32_t)VFS_OPEN_CREATE &&
                   (uint32_t)USER_OPEN_TRUNCATE ==
                       (uint32_t)VFS_OPEN_TRUNCATE &&
                   (uint32_t)USER_OPEN_DIRECTORY ==
                       (uint32_t)VFS_OPEN_DIRECTORY &&
                   (uint32_t)USER_OPEN_APPEND == (uint32_t)VFS_OPEN_APPEND,
               "User and VFS open flags must match");

/*
 * User virtual-address layout:
 *
 *   0x00001000 .. 0x007fefff   ELF load range
 *   first page after ELF ..     growable userspace heap
 *   0x007ff000                 unmapped stack guard
 *   0x00800000                 one-page user stack
 *   0x01000000 ..              owned graphical framebuffer
 *   0x02000000 .. 0x04ffffff   sixteen 3 MiB shared-memory mapping slots
 *   0x05000000 .. 0x0fffffff   private anonymous mappings
 *
 * The stack pointer begins just above the mapped stack page and grows down.
 */
#define USER_ADDRESS_MIN UINT64_C(0x00001000)
#define USER_STACK_GUARD_ADDRESS UINT64_C(0x007ff000)
#define USER_STACK_ADDRESS UINT64_C(0x00800000)
#define USER_STACK_TOP (USER_STACK_ADDRESS + PAGE_SIZE)
#define USER_GRAPHICS_ADDRESS UINT64_C(0x01000000)
#define USER_SHARED_MEMORY_BASE UINT64_C(0x02000000)
#define USER_SHARED_MEMORY_STRIDE (SHARED_MEMORY_PAGE_LIMIT * PAGE_SIZE)
#define USER_MMAP_BASE UINT64_C(0x05000000)
#define USER_MMAP_END UINT64_C(0x10000000)

extern char text_start[];

/* Assembly transitions which save and later restore the kernel scheduler. */
extern void user_mode_enter(const struct trap_frame *context,
                            uintptr_t kernel_trap_stack_top);
extern void user_mode_resume(void);
extern uintptr_t user_saved_kernel_context_sp;

struct process_open_file {
        bool used;
        uint8_t access;
        bool append;
        size_t references;
        uint64_t offset;
        struct process_pipe *read_pipe;
        struct process_pipe *write_pipe;
        struct process_terminal *terminal;
        uint8_t terminal_endpoint;
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

enum process_terminal_endpoint {
        PROCESS_TERMINAL_NONE,
        PROCESS_TERMINAL_MASTER,
        PROCESS_TERMINAL_SLAVE,
};

/* One shared line discipline belongs to both ends of a PTY. The physical
 * console uses the same foreground/session metadata but retains its existing
 * UART byte queue. */
struct process_terminal {
        bool used;
        bool console;
        size_t open_references;
        struct process_pipe *input_pipe;
        struct process_pipe *output_pipe;
        struct user_terminal_attributes attributes;
        struct user_terminal_window_size window_size;
        size_t canonical_ready;
        size_t canonical_pending;
        bool canonical_eof;
        uint64_t foreground_process_group;
        uint64_t controlling_session;
};

struct process_descriptor {
        struct process_open_file *open_file;
        bool close_on_exec;
        bool nonblocking;
};

struct process_signal_disposition {
        uintptr_t handler;
        uintptr_t restorer;
};

struct shared_memory_object {
        bool used;
        uint32_t identifier;
        size_t size;
        size_t page_count;
        size_t references;
        void *pages[SHARED_MEMORY_PAGE_LIMIT];
};

struct process_shared_memory_mapping {
        struct shared_memory_object *object;
        uintptr_t address;
};

struct process_anonymous_mapping {
        bool used;
        uintptr_t start;
        uintptr_t end;
        uint32_t protection;
};

struct process {
        /* Scheduler-visible identity and saved execution state. */
        uint64_t pid;
        uint64_t parent_pid;
        uint64_t process_group;
        uint64_t session_id;
        struct process_terminal *controlling_terminal;
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
        char executable[USER_PROCESS_NAME_MAX];
        struct process_descriptor descriptors[PROCESS_DESCRIPTOR_LIMIT];
        struct process_shared_memory_mapping
            shared_memory[SHARED_MEMORY_PROCESS_LIMIT];
        struct process_anonymous_mapping
            anonymous_mappings[PROCESS_ANONYMOUS_MAPPING_LIMIT];

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
        bool scheduler_deadline_active;
        uint64_t scheduler_deadline;
        enum process_pending_wait pending_wait;
        bool pending_wait_has_deadline;
        uint64_t pending_wait_deadline;
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
static struct process_terminal pseudo_terminal_table[PSEUDO_TERMINAL_LIMIT];
static struct process_terminal console_terminal;
static struct process_open_file open_file_table[PROCESS_OPEN_FILE_LIMIT];
static struct shared_memory_object
    shared_memory_objects[SHARED_MEMORY_OBJECT_LIMIT];
static struct process *active_process;
static struct process *uart_write_owner;
static uint64_t next_pid = 1U;
static uint64_t graphics_owner_pid;
static uint32_t next_shared_memory_identifier = 1U;
static uint64_t scheduler_preemptions;
static uint64_t scheduler_context_switches;
static uint64_t scheduler_blocks;
static uint64_t copy_on_write_faults;
static uint64_t copy_on_write_copies;
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

static void shared_memory_object_destroy(struct shared_memory_object *object) {
        if (object == NULL || !object->used || object->references != 0U) {
                panic("Invalid shared-memory destruction");
        }
        for (size_t index = 0U; index < object->page_count; index++) {
                if (object->pages[index] == NULL) {
                        panic("Shared-memory page ownership mismatch");
                }
                page_release(object->pages[index]);
        }
        bytes_zero(object, sizeof(*object));
}

static void process_shared_memory_unmap_slot(struct process *process,
                                             size_t slot) {
        struct process_shared_memory_mapping *mapping =
            &process->shared_memory[slot];
        struct shared_memory_object *object = mapping->object;

        if (object == NULL) {
                return;
        }
        if (!object->used || object->references == 0U ||
            process->address_space == NULL) {
                panic("Invalid shared-memory mapping release");
        }

        for (size_t index = 0U; index < object->page_count; index++) {
                if (!page_table_unmap(process->address_space,
                                      mapping->address + index * PAGE_SIZE)) {
                        panic("Shared-memory unmap ownership mismatch");
                }
        }
        object->references--;
        bytes_zero(mapping, sizeof(*mapping));
        if (object->references == 0U) {
                shared_memory_object_destroy(object);
        }
}

static void process_release_shared_memory(struct process *process) {
        for (size_t slot = 0U; slot < SHARED_MEMORY_PROCESS_LIMIT; slot++) {
                process_shared_memory_unmap_slot(process, slot);
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

static void process_terminal_set_defaults(struct process_terminal *terminal,
                                          bool console) {
        bytes_zero(terminal, sizeof(*terminal));
        terminal->used = true;
        terminal->console = console;
        /* Preserve the serial console's established userspace line editor.
         * PTYs start with the conventional cooked discipline. */
        terminal->attributes.flags = console ? USER_TERMINAL_SIGNALS
                                             : USER_TERMINAL_CANONICAL |
                                                   USER_TERMINAL_ECHO |
                                                   USER_TERMINAL_SIGNALS;
        terminal->attributes.interrupt_character = UINT8_C(0x03);
        terminal->attributes.quit_character = UINT8_C(0x1c);
        terminal->attributes.erase_character = UINT8_C(0x7f);
        terminal->attributes.end_of_file_character = UINT8_C(0x04);
        terminal->attributes.suspend_character = UINT8_C(0x1a);
        terminal->window_size.rows = console ? 25U : 24U;
        terminal->window_size.columns = 80U;
}

static struct process_terminal *process_console_terminal(void) {
        if (!console_terminal.used) {
                process_terminal_set_defaults(&console_terminal, true);
        }
        return &console_terminal;
}

static struct process_terminal *process_terminal_allocate(void) {
        for (size_t index = 0U; index < PSEUDO_TERMINAL_LIMIT; index++) {
                if (!pseudo_terminal_table[index].used) {
                        process_terminal_set_defaults(
                            &pseudo_terminal_table[index], false);
                        return &pseudo_terminal_table[index];
                }
        }
        return NULL;
}

static void process_terminal_maybe_destroy(struct process_terminal *terminal) {
        if (terminal == NULL || terminal->console || !terminal->used) {
                return;
        }
        if (terminal->open_references == 0U &&
            terminal->controlling_session == 0U) {
                bytes_zero(terminal, sizeof(*terminal));
        }
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

static void process_terminal_open_release(struct process_terminal *terminal) {
        if (terminal == NULL || terminal->console) {
                return;
        }
        if (!terminal->used || terminal->open_references == 0U) {
                panic("Invalid terminal endpoint release");
        }
        terminal->open_references--;
        process_terminal_maybe_destroy(terminal);
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
        descriptor->nonblocking = false;
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
        descriptor->nonblocking = false;
        open_file->references--;
        if (open_file->references == 0U) {
                if (open_file->read_pipe != NULL) {
                        process_pipe_release(open_file->read_pipe,
                                             DESCRIPTOR_READ);
                }
                if (open_file->write_pipe != NULL) {
                        process_pipe_release(open_file->write_pipe,
                                             DESCRIPTOR_WRITE);
                }
                process_terminal_open_release(open_file->terminal);
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
                open_file->terminal = process_console_terminal();
                open_file->terminal_endpoint = PROCESS_TERMINAL_SLAVE;
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
                destination->descriptors[descriptor].nonblocking =
                    source->descriptors[descriptor].nonblocking;
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
                destination->descriptors[descriptor].nonblocking =
                    source_descriptor->nonblocking;
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

static bool anonymous_mapping_protection_is_valid(uint32_t protection) {
        const uint32_t valid_protection = USER_MEMORY_PROTECTION_READ |
                                          USER_MEMORY_PROTECTION_WRITE |
                                          USER_MEMORY_PROTECTION_EXECUTE;

        return (protection & ~valid_protection) == 0U &&
               ((protection & USER_MEMORY_PROTECTION_WRITE) == 0U ||
                (protection & USER_MEMORY_PROTECTION_READ) != 0U) &&
               (protection & (USER_MEMORY_PROTECTION_WRITE |
                              USER_MEMORY_PROTECTION_EXECUTE)) !=
                   (USER_MEMORY_PROTECTION_WRITE |
                    USER_MEMORY_PROTECTION_EXECUTE);
}

static uint64_t anonymous_mapping_page_flags(uint32_t protection) {
        /* A valid RISC-V leaf needs at least one R/W/X bit. Keep resident
         * PROT_NONE pages supervisor-readable so their contents and ownership
         * survive while U-mode has no access to them. */
        uint64_t flags = protection == USER_MEMORY_PROTECTION_NONE
                             ? VM_PAGE_READ
                             : VM_PAGE_USER;

        if ((protection & USER_MEMORY_PROTECTION_READ) != 0U) {
                flags |= VM_PAGE_READ;
        }
        if ((protection & USER_MEMORY_PROTECTION_WRITE) != 0U) {
                flags |= VM_PAGE_WRITE;
        }
        if ((protection & USER_MEMORY_PROTECTION_EXECUTE) != 0U) {
                flags |= VM_PAGE_EXECUTE;
        }
        return flags;
}

static struct process_anonymous_mapping *
process_find_anonymous_mapping(struct process *process, uintptr_t address) {
        if (process == NULL) {
                return NULL;
        }

        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                struct process_anonymous_mapping *mapping =
                    &process->anonymous_mappings[index];

                if (mapping->used && address >= mapping->start &&
                    address < mapping->end) {
                        return mapping;
                }
        }
        return NULL;
}

static bool
anonymous_mapping_allows(const struct process_anonymous_mapping *mapping,
                         uint64_t required_flags) {
        if (mapping == NULL || !mapping->used) {
                return false;
        }
        if ((required_flags & VM_PAGE_READ) != 0U &&
            (mapping->protection & USER_MEMORY_PROTECTION_READ) == 0U) {
                return false;
        }
        if ((required_flags & VM_PAGE_WRITE) != 0U &&
            (mapping->protection & USER_MEMORY_PROTECTION_WRITE) == 0U) {
                return false;
        }
        if ((required_flags & VM_PAGE_EXECUTE) != 0U &&
            (mapping->protection & USER_MEMORY_PROTECTION_EXECUTE) == 0U) {
                return false;
        }
        return true;
}

static bool anonymous_mapping_flags_are_valid(
    const struct process_anonymous_mapping *mapping, uint64_t flags);

/* Install a zero-filled page for a reserved anonymous address. page_alloc
 * supplies the zeroing guarantee, while the VMA retains ownership implicitly
 * through its virtual-address interval. */
static bool process_materialize_anonymous_page(struct process *process,
                                               uintptr_t address,
                                               uint64_t required_flags) {
        uintptr_t virtual_address = address & ~(PAGE_SIZE - 1U);
        struct process_anonymous_mapping *mapping =
            process_find_anonymous_mapping(process, virtual_address);

        if (!anonymous_mapping_allows(mapping, required_flags)) {
                return false;
        }

        uintptr_t existing_physical_address;
        uint64_t existing_flags;
        if (page_table_translate(process->address_space, virtual_address,
                                 &existing_physical_address, &existing_flags)) {
                return (existing_flags & (VM_PAGE_USER | required_flags)) ==
                       (VM_PAGE_USER | required_flags);
        }

        uint64_t mapping_flags =
            anonymous_mapping_page_flags(mapping->protection);
        if ((mapping_flags & (VM_PAGE_READ | VM_PAGE_EXECUTE)) == 0U) {
                return false;
        }

        void *page = page_alloc();
        if (page == NULL) {
                return false;
        }
        if (!page_table_map(process->address_space, virtual_address,
                            (uintptr_t)page, mapping_flags)) {
                page_release(page);
                return false;
        }
        return true;
}

static void process_release_anonymous_page(struct process *process,
                                           uintptr_t virtual_address) {
        uintptr_t physical_address;
        uint64_t flags;
        struct process_anonymous_mapping *mapping =
            process_find_anonymous_mapping(process, virtual_address);

        if (!page_table_translate(process->address_space, virtual_address,
                                  &physical_address, &flags)) {
                return;
        }
        if ((physical_address & (PAGE_SIZE - 1U)) != 0U ||
            !anonymous_mapping_flags_are_valid(mapping, flags) ||
            !page_table_unmap(process->address_space, virtual_address)) {
                panic("Anonymous mapping ownership mismatch");
        }
        page_release((void *)physical_address);
}

static void process_release_anonymous_mappings(struct process *process) {
        if (process == NULL) {
                return;
        }

        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                struct process_anonymous_mapping *mapping =
                    &process->anonymous_mappings[index];

                if (!mapping->used) {
                        continue;
                }
                if (process->address_space == NULL ||
                    mapping->start >= mapping->end) {
                        panic("Invalid anonymous mapping release");
                }
                for (uintptr_t address = mapping->start; address < mapping->end;
                     address += PAGE_SIZE) {
                        process_release_anonymous_page(process, address);
                }
                bytes_zero(mapping, sizeof(*mapping));
        }
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
                    (flags & (VM_PAGE_USER | VM_PAGE_READ)) !=
                        (VM_PAGE_USER | VM_PAGE_READ) ||
                    ((flags & VM_PAGE_WRITE) != 0U) ==
                        ((flags & VM_PAGE_COPY_ON_WRITE) != 0U) ||
                    (flags & VM_PAGE_EXECUTE) != 0U ||
                    !page_table_unmap(root, address)) {
                        panic("User heap ownership mismatch");
                }

                page_release((void *)physical_address);
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
                page_release(*stack_page);
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
        process_release_shared_memory(process);
        process_release_anonymous_mappings(process);
        process_release_image(&process->address_space, &process->loaded_image,
                              &process->stack_page, &process->heap_start,
                              &process->heap_break);

        if (process->kernel_trap_stack != NULL) {
                page_release(process->kernel_trap_stack);
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

static void process_set_executable(struct process *process, const char *path) {
        size_t index = 0U;
        while (path[index] != '\0' &&
               index + 1U < sizeof(process->executable)) {
                process->executable[index] = path[index];
                index++;
        }
        process->executable[index] = '\0';
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
                            VM_PAGE_USER | VM_PAGE_READ, VM_PAGE_EXECUTE);
        uintptr_t stack_physical;
        uint64_t stack_flags;
        if (!page_table_translate(root, USER_STACK_ADDRESS, &stack_physical,
                                  &stack_flags) ||
            ((stack_flags & VM_PAGE_WRITE) != 0U) ==
                ((stack_flags & VM_PAGE_COPY_ON_WRITE) != 0U)) {
                panic("User stack is neither writable nor copy-on-write");
        }

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
        process->session_id = descriptor_source == NULL
                                  ? process->pid
                                  : descriptor_source->session_id;
        process->controlling_terminal =
            descriptor_source == NULL ? process_console_terminal()
                                      : descriptor_source->controlling_terminal;
        process->state = PROCESS_READY;
        /* SPIE causes sret to enable supervisor interrupts while U-mode runs.
         */
        process->context.sstatus = SSTATUS_SPIE;
        process->exit_status = UINT64_MAX;
        process->current_directory[0] = '/';
        process->current_directory[1] = '\0';
        process_set_executable(process, path);
        bool descriptors_ready =
            descriptor_source == NULL
                ? process_descriptors_initialize(process)
                : process_descriptors_clone(process, descriptor_source);
        if (!descriptors_ready) {
                process_release_resources(process);
                bytes_zero(process, sizeof(*process));
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }
        next_pid++;
        if (next_pid == 0U) {
                next_pid = 1U;
        }
        if (process->controlling_terminal != NULL &&
            process->controlling_terminal->controlling_session == 0U) {
                process->controlling_terminal->controlling_session =
                    process->session_id;
                process->controlling_terminal->foreground_process_group =
                    process->process_group;
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

/* Update the one ownership record which names a privately replaced page. Heap
 * ownership is implicit in its virtual-address interval; ELF and stack pages
 * additionally retain their physical addresses for exact teardown checks. */
static void process_replace_owned_page(struct process *process,
                                       uintptr_t virtual_address,
                                       void *old_page, void *new_page) {
        if (virtual_address == USER_STACK_ADDRESS) {
                if (process->stack_page != old_page) {
                        panic("Copy-on-write stack ownership mismatch");
                }
                process->stack_page = new_page;
                return;
        }

        for (size_t index = 0U; index < process->loaded_image.page_count;
             index++) {
                struct elf_loaded_page *page =
                    &process->loaded_image.pages[index];
                if (page->virtual_address != virtual_address) {
                        continue;
                }
                if (page->physical_page != old_page ||
                    (page->flags & VM_PAGE_WRITE) == 0U) {
                        panic("Copy-on-write ELF ownership mismatch");
                }
                page->physical_page = new_page;
                return;
        }

        if (virtual_address >= process->heap_start &&
            virtual_address < align_up_to_page(process->heap_break)) {
                return;
        }

        struct process_anonymous_mapping *mapping =
            process_find_anonymous_mapping(process, virtual_address);
        if (mapping != NULL &&
            (mapping->protection & USER_MEMORY_PROTECTION_WRITE) != 0U) {
                return;
        }

        panic("Copy-on-write page has no private owner");
}

/* Resolve one intentional COW mapping for either a U-mode store fault or a
 * kernel copy into a userspace output buffer. A sole owner only needs its write
 * permission restored; multiple owners require one new physical page. */
static bool process_resolve_copy_on_write(struct process *process,
                                          uintptr_t address) {
        uintptr_t virtual_address = address & ~(PAGE_SIZE - 1U);
        uintptr_t physical_address;
        uint64_t flags;

        if (process == NULL ||
            !page_table_translate(process->address_space, virtual_address,
                                  &physical_address, &flags) ||
            (physical_address & (PAGE_SIZE - 1U)) != 0U ||
            (flags & (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_COPY_ON_WRITE)) !=
                (VM_PAGE_USER | VM_PAGE_READ | VM_PAGE_COPY_ON_WRITE) ||
            (flags & (VM_PAGE_WRITE | VM_PAGE_EXECUTE)) != 0U) {
                return false;
        }

        void *old_page = (void *)physical_address;
        uint64_t writable_flags =
            (flags & ~VM_PAGE_COPY_ON_WRITE) | VM_PAGE_WRITE;

        if (page_reference_count(old_page) == 1U) {
                return page_table_protect(process->address_space,
                                          virtual_address, writable_flags);
        }

        void *new_page = page_alloc();
        if (new_page == NULL) {
                return false;
        }
        page_copy(new_page, old_page);
        if (!page_table_replace(process->address_space, virtual_address,
                                (uintptr_t)new_page, writable_flags)) {
                page_release(new_page);
                return false;
        }

        process_replace_owned_page(process, virtual_address, old_page,
                                   new_page);
        page_release(old_page);
        copy_on_write_copies++;
        return true;
}

static uint64_t copy_on_write_flags(uint64_t writable_flags) {
        return (writable_flags & ~VM_PAGE_WRITE) | VM_PAGE_COPY_ON_WRITE;
}

/* Share ELF leaves with the child. Immutable pages keep their exact mapping;
 * writable pages become read-only COW in both address spaces. Each child
 * record is installed before mapping so ordinary teardown handles a partial
 * fork without special rollback ownership. */
static bool process_fork_loaded_image(struct process *child,
                                      struct process *parent) {
        child->loaded_image.entry = parent->loaded_image.entry;

        for (size_t index = 0U; index < parent->loaded_image.page_count;
             index++) {
                const struct elf_loaded_page *source_page =
                    &parent->loaded_image.pages[index];
                uint64_t mapping_flags = source_page->flags;
                if ((mapping_flags & VM_PAGE_WRITE) != 0U) {
                        mapping_flags = copy_on_write_flags(mapping_flags);
                }
                page_retain(source_page->physical_page);
                struct elf_loaded_page *destination_page =
                    &child->loaded_image
                         .pages[child->loaded_image.page_count++];
                destination_page->virtual_address =
                    source_page->virtual_address;
                destination_page->physical_page = source_page->physical_page;
                destination_page->flags = source_page->flags;

                if (!page_table_map(child->address_space,
                                    destination_page->virtual_address,
                                    (uintptr_t)destination_page->physical_page,
                                    mapping_flags)) {
                        return false;
                }
                if ((source_page->flags & VM_PAGE_WRITE) != 0U &&
                    !page_table_protect(parent->address_space,
                                        source_page->virtual_address,
                                        mapping_flags)) {
                        return false;
                }
        }

        return true;
}

/* Share every materialized heap page and advance the candidate break after
 * each child mapping so process_release_heap remains exact on failure. */
static bool process_fork_heap(struct process *child, struct process *parent) {
        uintptr_t mapped_end = align_up_to_page(parent->heap_break);

        for (uintptr_t address = parent->heap_start; address < mapped_end;
             address += PAGE_SIZE) {
                uintptr_t source_physical_address;
                uint64_t flags;

                if (!page_table_translate(parent->address_space, address,
                                          &source_physical_address, &flags) ||
                    (source_physical_address & (PAGE_SIZE - 1U)) != 0U ||
                    (flags & (VM_PAGE_USER | VM_PAGE_READ)) !=
                        (VM_PAGE_USER | VM_PAGE_READ) ||
                    ((flags & VM_PAGE_WRITE) != 0U) ==
                        ((flags & VM_PAGE_COPY_ON_WRITE) != 0U) ||
                    (flags & VM_PAGE_EXECUTE) != 0U) {
                        panic("Fork source heap ownership mismatch");
                }

                uint64_t mapping_flags = (flags & VM_PAGE_COPY_ON_WRITE) != 0U
                                             ? flags
                                             : copy_on_write_flags(flags);
                page_retain((void *)source_physical_address);
                if (!page_table_map(child->address_space, address,
                                    source_physical_address, mapping_flags)) {
                        page_release((void *)source_physical_address);
                        return false;
                }
                child->heap_break = address + PAGE_SIZE;
                if (!page_table_protect(parent->address_space, address,
                                        mapping_flags)) {
                        return false;
                }
        }

        child->heap_break = parent->heap_break;
        return true;
}

static bool process_fork_stack(struct process *child, struct process *parent) {
        uintptr_t physical_address;
        uint64_t flags;

        if (!page_table_translate(parent->address_space, USER_STACK_ADDRESS,
                                  &physical_address, &flags) ||
            physical_address != (uintptr_t)parent->stack_page ||
            (flags & (VM_PAGE_USER | VM_PAGE_READ)) !=
                (VM_PAGE_USER | VM_PAGE_READ) ||
            ((flags & VM_PAGE_WRITE) != 0U) ==
                ((flags & VM_PAGE_COPY_ON_WRITE) != 0U) ||
            (flags & VM_PAGE_EXECUTE) != 0U) {
                panic("Fork source stack ownership mismatch");
        }

        uint64_t mapping_flags = (flags & VM_PAGE_COPY_ON_WRITE) != 0U
                                     ? flags
                                     : copy_on_write_flags(flags);
        page_retain(parent->stack_page);
        child->stack_page = parent->stack_page;
        if (!page_table_map(child->address_space, USER_STACK_ADDRESS,
                            (uintptr_t)child->stack_page, mapping_flags)) {
                return false;
        }
        return page_table_protect(parent->address_space, USER_STACK_ADDRESS,
                                  mapping_flags);
}

static bool anonymous_mapping_flags_are_valid(
    const struct process_anonymous_mapping *mapping, uint64_t flags) {
        uint64_t expected = anonymous_mapping_page_flags(mapping->protection);

        if ((mapping->protection & USER_MEMORY_PROTECTION_WRITE) != 0U) {
                return flags == expected ||
                       flags == copy_on_write_flags(expected);
        }
        return flags == expected;
}

/* Copy VMA reservations without materializing untouched pages. Resident pages
 * are retained directly; writable leaves become COW in both processes. */
static bool process_fork_anonymous_mappings(struct process *child,
                                            struct process *parent) {
        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                child->anonymous_mappings[index] =
                    parent->anonymous_mappings[index];
        }

        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                const struct process_anonymous_mapping *mapping =
                    &parent->anonymous_mappings[index];

                if (!mapping->used) {
                        continue;
                }
                for (uintptr_t address = mapping->start; address < mapping->end;
                     address += PAGE_SIZE) {
                        uintptr_t physical_address;
                        uint64_t flags;

                        if (!page_table_translate(parent->address_space,
                                                  address, &physical_address,
                                                  &flags)) {
                                continue;
                        }
                        if ((physical_address & (PAGE_SIZE - 1U)) != 0U ||
                            !anonymous_mapping_flags_are_valid(mapping,
                                                               flags)) {
                                panic("Fork source anonymous mapping mismatch");
                        }

                        uint64_t child_flags = flags;
                        if ((mapping->protection &
                             USER_MEMORY_PROTECTION_WRITE) != 0U) {
                                child_flags = copy_on_write_flags(
                                    anonymous_mapping_page_flags(
                                        mapping->protection));
                        }

                        page_retain((void *)physical_address);
                        if (!page_table_map(child->address_space, address,
                                            physical_address, child_flags)) {
                                page_release((void *)physical_address);
                                return false;
                        }
                        if (flags != child_flags &&
                            !page_table_protect(parent->address_space, address,
                                                child_flags)) {
                                return false;
                        }
                }
        }
        return true;
}

/* Clone page tables and ownership records while sharing user leaves. Only the
 * child's page-table hierarchy and trusted trap stack are allocated eagerly;
 * private writable data is materialized on its first write. */
static uint64_t syscall_fork(const struct trap_frame *frame) {
        struct process *parent = active_process;
        struct process *child = process_find_available_slot();

        if (child == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
        }

        bytes_zero(child, sizeof(*child));
        child->parent_pid = parent->pid;
        child->process_group = parent->process_group;
        child->session_id = parent->session_id;
        child->controlling_terminal = parent->controlling_terminal;
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
        process_set_executable(child, parent->executable);

        child->address_space = virtual_memory_create_address_space();
        child->kernel_trap_stack = page_alloc();

        if (child->address_space == NULL || child->kernel_trap_stack == NULL) {
                goto out_of_memory;
        }

        if (!process_fork_stack(child, parent) ||
            !process_fork_loaded_image(child, parent) ||
            !process_fork_heap(child, parent) ||
            !process_fork_anonymous_mappings(child, parent)) {
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

        struct page_table *root = active_process->address_space;

        for (size_t offset = 0U; offset < length;) {
                uintptr_t physical_address;
                uint64_t flags;

                if (!page_table_translate(root, user_buffer + offset,
                                          &physical_address, &flags)) {
                        if (!process_materialize_anonymous_page(
                                active_process, user_buffer + offset,
                                required_flags) ||
                            !page_table_translate(root, user_buffer + offset,
                                                  &physical_address, &flags)) {
                                return false;
                        }
                }
                if ((required_flags & VM_PAGE_WRITE) != 0U &&
                    (flags & VM_PAGE_WRITE) == 0U &&
                    (flags & VM_PAGE_COPY_ON_WRITE) != 0U) {
                        if (!process_resolve_copy_on_write(
                                active_process, user_buffer + offset) ||
                            !page_table_translate(root, user_buffer + offset,
                                                  &physical_address, &flags)) {
                                return false;
                        }
                }
                if ((flags & (VM_PAGE_USER | required_flags)) !=
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

/* Job-control setup may race a very short-lived child. Its zombie still owns
 * a PID and group identity until waitpid, so the parent may finish assigning
 * that identity even after the child has released its execution resources. */
static struct process *process_find_pid(uint64_t pid) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];
                if (process->state != PROCESS_UNUSED && process->pid == pid) {
                        return process;
                }
        }
        return NULL;
}

static bool process_signal_stops_by_default(uint32_t signal) {
        return signal == USER_SIGNAL_STOP ||
               signal == USER_SIGNAL_TERMINAL_STOP ||
               signal == USER_SIGNAL_BACKGROUND_READ;
}

static bool process_signal_ignored_by_default(uint32_t signal) {
        return signal == USER_SIGNAL_WINDOW_CHANGED;
}

/* Queueing SIGCONT changes scheduler state immediately; its disposition is
 * still delivered later. SIGKILL likewise makes a stopped process runnable so
 * the normal trusted return boundary can terminate it. */
static void process_queue_signal(struct process *target, uint32_t signal) {
        if (process_signal_ignored_by_default(signal) &&
            target->signal_dispositions[signal].handler <= USER_SIGNAL_IGNORE) {
                return;
        }
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
                target->scheduler_deadline_active = false;
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
        if (target->session_id != active_process->session_id ||
            target->pid == target->session_id) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }

        uint64_t requested_group =
            process_group == 0 ? target->pid : (uint64_t)process_group;
        if (requested_group != target->pid) {
                bool matching_session = false;
                for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                        const struct process *member = &process_table[index];
                        if (member->state != PROCESS_UNUSED &&
                            member->process_group == requested_group &&
                            member->session_id == target->session_id) {
                                matching_session = true;
                                break;
                        }
                }
                if (!matching_session) {
                        return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
                }
        }

        target->process_group = requested_group;
        return 0U;
}

static struct process_terminal *process_descriptor_terminal(uint64_t descriptor,
                                                            uint8_t *endpoint) {
        if (descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return NULL;
        }

        const struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file == NULL || open_file->terminal == NULL) {
                return NULL;
        }
        if (endpoint != NULL) {
                *endpoint = open_file->terminal_endpoint;
        }
        return open_file->terminal;
}

static bool process_group_belongs_to_session(uint64_t process_group,
                                             uint64_t session_id) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                const struct process *process = &process_table[index];
                if (process->state != PROCESS_UNUSED &&
                    process->process_group == process_group &&
                    process->session_id == session_id) {
                        return true;
                }
        }
        return false;
}

static uint64_t syscall_terminal_set_foreground_group(uint64_t descriptor,
                                                      int64_t process_group) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (active_process->controlling_terminal != terminal ||
            terminal->controlling_session != active_process->session_id) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        if (process_group <= 0 ||
            !process_group_belongs_to_session((uint64_t)process_group,
                                              active_process->session_id)) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_PROCESS;
        }
        terminal->foreground_process_group = (uint64_t)process_group;
        return 0U;
}

static uint64_t syscall_terminal_get_foreground_group(uint64_t descriptor) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }

        return terminal->foreground_process_group;
}

static bool
process_terminal_signal_foreground(const struct process_terminal *terminal,
                                   uint32_t signal) {
        bool matched = false;
        if (terminal == NULL || terminal->foreground_process_group == 0U) {
                return false;
        }
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *target = &process_table[index];
                if (target->state == PROCESS_UNUSED ||
                    target->state == PROCESS_EXITED ||
                    target->process_group !=
                        terminal->foreground_process_group ||
                    target->session_id != terminal->controlling_session) {
                        continue;
                }
                process_queue_signal(target, signal);
                matched = true;
        }
        return matched;
}

static uint32_t
process_terminal_control_signal(const struct process_terminal *terminal,
                                uint8_t character) {
        if ((terminal->attributes.flags & USER_TERMINAL_SIGNALS) == 0U) {
                return 0U;
        }
        if (terminal->attributes.interrupt_character != 0U &&
            character == terminal->attributes.interrupt_character) {
                return USER_SIGNAL_INTERRUPT;
        }
        if (terminal->attributes.quit_character != 0U &&
            character == terminal->attributes.quit_character) {
                return USER_SIGNAL_QUIT;
        }
        if (terminal->attributes.suspend_character != 0U &&
            character == terminal->attributes.suspend_character) {
                return USER_SIGNAL_TERMINAL_STOP;
        }
        return 0U;
}

static void process_pipe_push_byte(struct process_pipe *pipe, uint8_t byte) {
        if (pipe == NULL || !pipe->used || pipe->count == PIPE_BUFFER_SIZE) {
                panic("Terminal pipe overflow");
        }
        pipe->buffer[pipe->write_offset] = byte;
        pipe->write_offset = (pipe->write_offset + 1U) % PIPE_BUFFER_SIZE;
        pipe->count++;
}

static void process_pipe_discard_last(struct process_pipe *pipe) {
        if (pipe == NULL || !pipe->used || pipe->count == 0U) {
                panic("Terminal pipe underflow");
        }
        pipe->write_offset =
            (pipe->write_offset + PIPE_BUFFER_SIZE - 1U) % PIPE_BUFFER_SIZE;
        pipe->count--;
}

/* Process one byte written to a PTY master. False means that accepting this
 * byte would overrun the input or echo ring and the write must wait. */
static bool
process_terminal_master_write_character(struct process_terminal *terminal,
                                        uint8_t character) {
        struct process_pipe *input = terminal->input_pipe;
        struct process_pipe *output = terminal->output_pipe;
        bool canonical =
            (terminal->attributes.flags & USER_TERMINAL_CANONICAL) != 0U;
        bool echo = (terminal->attributes.flags & USER_TERMINAL_ECHO) != 0U;
        uint32_t signal = process_terminal_control_signal(terminal, character);
        bool erase = canonical && terminal->attributes.erase_character != 0U &&
                     character == terminal->attributes.erase_character;
        bool eof = canonical &&
                   terminal->attributes.end_of_file_character != 0U &&
                   character == terminal->attributes.end_of_file_character;
        bool newline = canonical && (character == '\r' || character == '\n');
        size_t echo_length = 0U;

        if (echo && signal == 0U && !eof) {
                echo_length =
                    erase ? (terminal->canonical_pending != 0U ? 3U : 0U)
                          : (newline ? 2U : 1U);
        }
        if (echo_length > PIPE_BUFFER_SIZE - output->count) {
                return false;
        }
        if (signal == 0U && !erase && !eof &&
            input->count == PIPE_BUFFER_SIZE) {
                return false;
        }

        if (signal != 0U) {
                (void)process_terminal_signal_foreground(terminal, signal);
        } else if (erase) {
                if (terminal->canonical_pending != 0U) {
                        process_pipe_discard_last(input);
                        terminal->canonical_pending--;
                }
        } else if (eof) {
                if (terminal->canonical_pending != 0U) {
                        terminal->canonical_ready +=
                            terminal->canonical_pending;
                        terminal->canonical_pending = 0U;
                } else {
                        terminal->canonical_eof = true;
                }
        } else {
                uint8_t stored = newline ? (uint8_t)'\n' : character;
                process_pipe_push_byte(input, stored);
                if (canonical) {
                        terminal->canonical_pending++;
                        if (newline) {
                                terminal->canonical_ready +=
                                    terminal->canonical_pending;
                                terminal->canonical_pending = 0U;
                        }
                }
        }

        if (echo_length == 3U) {
                process_pipe_push_byte(output, (uint8_t)'\b');
                process_pipe_push_byte(output, (uint8_t)' ');
                process_pipe_push_byte(output, (uint8_t)'\b');
        } else if (echo_length == 2U) {
                process_pipe_push_byte(output, (uint8_t)'\r');
                process_pipe_push_byte(output, (uint8_t)'\n');
        } else if (echo_length == 1U) {
                process_pipe_push_byte(output, character);
        }
        if (signal == 0U) {
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_READ);
        }
        return true;
}

bool user_process_handle_console_control(char character) {
        struct process_terminal *terminal = process_console_terminal();
        uint32_t signal =
            process_terminal_control_signal(terminal, (uint8_t)character);

        if (signal == 0U) {
                return false;
        }
        (void)process_terminal_signal_foreground(terminal, signal);
        /* Signal-generating control bytes are consumed even when the current
         * foreground group has already exited. */
        return true;
}

static uint64_t syscall_terminal_get_attributes(uint64_t descriptor,
                                                uintptr_t user_attributes) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_attributes,
                                 sizeof(struct user_terminal_attributes),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        user_copy_to(user_attributes, (const uint8_t *)&terminal->attributes,
                     sizeof(terminal->attributes));
        return 0U;
}

static uint64_t syscall_terminal_set_attributes(uint64_t descriptor,
                                                uintptr_t user_attributes) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_attributes,
                                 sizeof(struct user_terminal_attributes),
                                 VM_PAGE_READ)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct user_terminal_attributes attributes;
        user_copy_from((uint8_t *)&attributes, user_attributes,
                       sizeof(attributes));
        if ((attributes.flags &
             ~(uint32_t)(USER_TERMINAL_CANONICAL | USER_TERMINAL_ECHO |
                         USER_TERMINAL_SIGNALS)) != 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        for (size_t index = 0U; index < sizeof(attributes.reserved); index++) {
                if (attributes.reserved[index] != 0U) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
        }

        bool was_canonical =
            (terminal->attributes.flags & USER_TERMINAL_CANONICAL) != 0U;
        bool canonical = (attributes.flags & USER_TERMINAL_CANONICAL) != 0U;
        terminal->attributes = attributes;
        if (was_canonical != canonical && terminal->input_pipe != NULL) {
                /* Never strand bytes across a mode transition. Existing input
                 * is immediately readable; only subsequent bytes begin a new
                 * canonical record. */
                terminal->canonical_ready = terminal->input_pipe->count;
                terminal->canonical_pending = 0U;
                terminal->canonical_eof = false;
                (void)scheduler_wake_all(SCHEDULER_WAIT_PIPE_READ);
        }
        return 0U;
}

static uint64_t syscall_terminal_get_window_size(uint64_t descriptor,
                                                 uintptr_t user_window_size) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_window_size,
                                 sizeof(struct user_terminal_window_size),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        user_copy_to(user_window_size, (const uint8_t *)&terminal->window_size,
                     sizeof(terminal->window_size));
        return 0U;
}

static bool process_terminal_window_sizes_equal(
    const struct user_terminal_window_size *left,
    const struct user_terminal_window_size *right) {
        return left->rows == right->rows && left->columns == right->columns &&
               left->pixel_width == right->pixel_width &&
               left->pixel_height == right->pixel_height;
}

static uint64_t syscall_terminal_set_window_size(uint64_t descriptor,
                                                 uintptr_t user_window_size) {
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, NULL);
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (!user_range_is_valid(user_window_size,
                                 sizeof(struct user_terminal_window_size),
                                 VM_PAGE_READ)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct user_terminal_window_size window_size;
        user_copy_from((uint8_t *)&window_size, user_window_size,
                       sizeof(window_size));
        if (window_size.rows == 0U || window_size.columns == 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (!process_terminal_window_sizes_equal(&terminal->window_size,
                                                 &window_size)) {
                terminal->window_size = window_size;
                (void)process_terminal_signal_foreground(
                    terminal, USER_SIGNAL_WINDOW_CHANGED);
        }
        return 0U;
}

static bool process_session_has_live_member(uint64_t session_id) {
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                const struct process *process = &process_table[index];
                if (process->state != PROCESS_UNUSED &&
                    process->state != PROCESS_EXITED &&
                    process->session_id == session_id) {
                        return true;
                }
        }
        return false;
}

static void
process_terminal_release_empty_session(struct process_terminal *terminal,
                                       uint64_t session_id) {
        if (terminal != NULL && terminal->controlling_session == session_id &&
            !process_session_has_live_member(session_id)) {
                terminal->controlling_session = 0U;
                terminal->foreground_process_group = 0U;
                process_terminal_maybe_destroy(terminal);
        }
}

static uint64_t syscall_create_session(void) {
        if (active_process->process_group == active_process->pid) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        uint64_t old_session = active_process->session_id;
        struct process_terminal *old_terminal =
            active_process->controlling_terminal;
        active_process->session_id = active_process->pid;
        active_process->process_group = active_process->pid;
        active_process->controlling_terminal = NULL;
        process_terminal_release_empty_session(old_terminal, old_session);
        return active_process->session_id;
}

static uint64_t syscall_get_session(int64_t pid) {
        if (pid < 0) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        struct process *target =
            pid == 0 ? active_process : process_find_pid((uint64_t)pid);
        return target == NULL ? (uint64_t)-(int64_t)USER_ERROR_NO_PROCESS
                              : target->session_id;
}

static uint64_t syscall_terminal_set_controlling(uint64_t descriptor) {
        uint8_t endpoint;
        struct process_terminal *terminal =
            process_descriptor_terminal(descriptor, &endpoint);
        if (terminal == NULL || endpoint != PROCESS_TERMINAL_SLAVE) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_FILE_DESCRIPTOR;
        }
        if (active_process->session_id != active_process->pid ||
            active_process->controlling_terminal != NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        if (terminal->controlling_session != 0U &&
            terminal->controlling_session != active_process->session_id) {
                return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
        }
        terminal->controlling_session = active_process->session_id;
        terminal->foreground_process_group = active_process->process_group;
        active_process->controlling_terminal = terminal;
        return 0U;
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
        process_release_shared_memory(active_process);
        page_table_activate(new_address_space);
        process_release_anonymous_mappings(active_process);
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
        process_set_executable(active_process, path);
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

        if (open_file->write_pipe != NULL) {
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
                uint64_t write_offset = open_file->offset;
                if (open_file->append) {
                        struct vfs_stat status;
                        int stat_result =
                            vfs_stat_file(&open_file->file, &status);
                        if (stat_result != 0) {
                                return (uint64_t)(int64_t)stat_result;
                        }
                        write_offset = status.size;
                }
                user_copy_from((uint8_t *)active_process->write_buffer,
                               user_buffer, length);
                long result = vfs_write(&open_file->file, write_offset,
                                        active_process->write_buffer, length);
                if (result > 0) {
                        open_file->offset = write_offset + (uint64_t)result;
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
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }
        open_file->access = access;
        open_file->append = (flags & VFS_OPEN_APPEND) != 0U;
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

        if (open_file->terminal != NULL &&
            open_file->terminal_endpoint == PROCESS_TERMINAL_SLAVE &&
            active_process->controlling_terminal == open_file->terminal &&
            open_file->terminal->foreground_process_group != 0U &&
            active_process->process_group !=
                open_file->terminal->foreground_process_group) {
                (void)syscall_kill(0, USER_SIGNAL_BACKGROUND_READ);
        }

        if (open_file->read_pipe != NULL) {
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

        if (open_file->read_pipe != NULL || open_file->write_pipe != NULL) {
                struct user_file_status pipe_status;

                bytes_zero(&pipe_status, sizeof(pipe_status));
                pipe_status.size = open_file->read_pipe == NULL
                                       ? open_file->write_pipe->count
                                       : open_file->read_pipe->count;
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
        if (open_file->read_pipe != NULL || open_file->write_pipe != NULL ||
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
        if (directory->read_pipe != NULL || directory->write_pipe != NULL) {
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

static uint64_t syscall_rename(uintptr_t user_old_path,
                               uintptr_t user_new_path) {
        char old_path[USER_PATH_MAX];
        char new_path[USER_PATH_MAX];
        uint64_t result = user_copy_path(user_old_path, old_path);
        if (result != 0U) return result;
        result = user_copy_path(user_new_path, new_path);
        if (result != 0U) return result;
        return (uint64_t)(int64_t)vfs_rename(old_path, new_path);
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
        read_end->read_pipe = pipe;
        process_pipe_retain(pipe, read_end->access);
        write_end->access = DESCRIPTOR_WRITE;
        write_end->write_pipe = pipe;
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
        if ((flags & ~((uint32_t)USER_DESCRIPTOR_CLOSE_ON_EXEC |
                       (uint32_t)USER_DESCRIPTOR_NONBLOCK)) != 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        active_process->descriptors[(size_t)descriptor].close_on_exec =
            (flags & USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0U;
        active_process->descriptors[(size_t)descriptor].nonblocking =
            (flags & USER_DESCRIPTOR_NONBLOCK) != 0U;
        return 0U;
}

static struct process_anonymous_mapping *
process_find_free_anonymous_mapping(struct process *process) {
        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                if (!process->anonymous_mappings[index].used) {
                        return &process->anonymous_mappings[index];
                }
        }
        return NULL;
}

static bool address_ranges_overlap(uintptr_t first_start, uintptr_t first_end,
                                   uintptr_t second_start,
                                   uintptr_t second_end) {
        return first_start < second_end && second_start < first_end;
}

/* Reserve the first fitting address interval without allocating page-table or
 * physical pages. Fault handling materializes each page independently. */
static uint64_t syscall_mmap(size_t length, uint32_t protection) {
        if (length == 0U || length > UINTPTR_MAX - (PAGE_SIZE - 1U) ||
            !anonymous_mapping_protection_is_valid(protection)) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        size_t rounded_length = (size_t)align_up_to_page(length);
        if (rounded_length > USER_MMAP_END - USER_MMAP_BASE) {
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        struct process_anonymous_mapping *slot =
            process_find_free_anonymous_mapping(active_process);
        if (slot == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }

        uintptr_t candidate = USER_MMAP_BASE;
        while (candidate <= USER_MMAP_END - rounded_length) {
                bool collision = false;
                uintptr_t candidate_end = candidate + rounded_length;

                for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
                     index++) {
                        const struct process_anonymous_mapping *mapping =
                            &active_process->anonymous_mappings[index];

                        if (!mapping->used ||
                            !address_ranges_overlap(candidate, candidate_end,
                                                    mapping->start,
                                                    mapping->end)) {
                                continue;
                        }
                        candidate = mapping->end;
                        collision = true;
                        break;
                }
                if (!collision) {
                        *slot = (struct process_anonymous_mapping){
                            .used = true,
                            .start = candidate,
                            .end = candidate_end,
                            .protection = protection,
                        };
                        return candidate;
                }
        }

        return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
}

static void anonymous_mapping_sort(struct process_anonymous_mapping *mappings,
                                   size_t count) {
        for (size_t index = 1U; index < count; index++) {
                struct process_anonymous_mapping value = mappings[index];
                size_t destination = index;

                while (destination != 0U &&
                       mappings[destination - 1U].start > value.start) {
                        mappings[destination] = mappings[destination - 1U];
                        destination--;
                }
                mappings[destination] = value;
        }
}

static bool anonymous_mapping_append(struct process_anonymous_mapping *mappings,
                                     size_t capacity, size_t *count,
                                     uintptr_t start, uintptr_t end,
                                     uint32_t protection) {
        if (start == end) {
                return true;
        }
        if (*count != 0U) {
                struct process_anonymous_mapping *previous =
                    &mappings[*count - 1U];
                if (previous->end == start &&
                    previous->protection == protection) {
                        previous->end = end;
                        return true;
                }
        }
        if (*count == capacity) {
                return false;
        }
        mappings[*count] = (struct process_anonymous_mapping){
            .used = true,
            .start = start,
            .end = end,
            .protection = protection,
        };
        (*count)++;
        return true;
}

/* Change permissions only across completely reserved anonymous ranges. A
 * candidate VMA set is assembled before any live state changes, so exhausting
 * the fixed metadata table leaves both records and PTEs untouched. */
static uint64_t syscall_mprotect(uintptr_t address, size_t length,
                                 uint32_t protection) {
        if ((address & (PAGE_SIZE - 1U)) != 0U || length == 0U ||
            length > UINTPTR_MAX - (PAGE_SIZE - 1U) ||
            !anonymous_mapping_protection_is_valid(protection)) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        size_t rounded_length = (size_t)align_up_to_page(length);
        if (address < USER_MMAP_BASE || address > USER_MMAP_END ||
            rounded_length > USER_MMAP_END - address) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        uintptr_t end = address + rounded_length;

        struct process_anonymous_mapping
            ordered[PROCESS_ANONYMOUS_MAPPING_LIMIT];
        size_t ordered_count = 0U;
        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                if (active_process->anonymous_mappings[index].used) {
                        ordered[ordered_count++] =
                            active_process->anonymous_mappings[index];
                }
        }
        anonymous_mapping_sort(ordered, ordered_count);

        uintptr_t covered = address;
        for (size_t index = 0U; index < ordered_count && covered < end;
             index++) {
                const struct process_anonymous_mapping *mapping =
                    &ordered[index];
                if (mapping->end <= covered) {
                        continue;
                }
                if (mapping->start > covered) {
                        break;
                }
                if (mapping->end > covered) {
                        covered = mapping->end < end ? mapping->end : end;
                }
        }
        if (covered != end) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        struct process_anonymous_mapping
            candidate[PROCESS_ANONYMOUS_MAPPING_LIMIT];
        size_t candidate_count = 0U;
        for (size_t index = 0U; index < ordered_count; index++) {
                const struct process_anonymous_mapping *mapping =
                    &ordered[index];
                uintptr_t overlap_start =
                    address > mapping->start ? address : mapping->start;
                uintptr_t overlap_end = end < mapping->end ? end : mapping->end;

                if (!address_ranges_overlap(address, end, mapping->start,
                                            mapping->end)) {
                        if (!anonymous_mapping_append(
                                candidate, PROCESS_ANONYMOUS_MAPPING_LIMIT,
                                &candidate_count, mapping->start, mapping->end,
                                mapping->protection)) {
                                return (uint64_t)-(
                                    int64_t)USER_ERROR_OUT_OF_MEMORY;
                        }
                        continue;
                }

                if (!anonymous_mapping_append(
                        candidate, PROCESS_ANONYMOUS_MAPPING_LIMIT,
                        &candidate_count, mapping->start, overlap_start,
                        mapping->protection) ||
                    !anonymous_mapping_append(candidate,
                                              PROCESS_ANONYMOUS_MAPPING_LIMIT,
                                              &candidate_count, overlap_start,
                                              overlap_end, protection) ||
                    !anonymous_mapping_append(
                        candidate, PROCESS_ANONYMOUS_MAPPING_LIMIT,
                        &candidate_count, overlap_end, mapping->end,
                        mapping->protection)) {
                        return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
                }
        }

        for (uintptr_t page = address; page < end; page += PAGE_SIZE) {
                uintptr_t physical_address;
                uint64_t flags;
                struct process_anonymous_mapping *mapping =
                    process_find_anonymous_mapping(active_process, page);

                if (!page_table_translate(active_process->address_space, page,
                                          &physical_address, &flags)) {
                        continue;
                }
                if ((physical_address & (PAGE_SIZE - 1U)) != 0U ||
                    !anonymous_mapping_flags_are_valid(mapping, flags)) {
                        panic("mprotect source anonymous mapping mismatch");
                }

                uint64_t new_flags = anonymous_mapping_page_flags(protection);
                if ((protection & USER_MEMORY_PROTECTION_WRITE) != 0U &&
                    page_reference_count((void *)physical_address) > 1U) {
                        new_flags = copy_on_write_flags(new_flags);
                }
                if (!page_table_protect(active_process->address_space, page,
                                        new_flags)) {
                        panic("mprotect page-table update failed");
                }
        }

        bytes_zero(active_process->anonymous_mappings,
                   sizeof(active_process->anonymous_mappings));
        for (size_t index = 0U; index < candidate_count; index++) {
                active_process->anonymous_mappings[index] = candidate[index];
        }
        return 0U;
}

/* Linux-compatible hole handling keeps munmap idempotent. Cutting the middle
 * of one VMA consumes a second fixed metadata slot; prefix/suffix removals do
 * not. Only resident pages have PTEs and physical ownership to release. */
static uint64_t syscall_munmap(uintptr_t address, size_t length) {
        if ((address & (PAGE_SIZE - 1U)) != 0U || length == 0U ||
            length > UINTPTR_MAX - (PAGE_SIZE - 1U)) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        size_t rounded_length = (size_t)align_up_to_page(length);
        if (address < USER_MMAP_BASE || address > USER_MMAP_END ||
            rounded_length > USER_MMAP_END - address) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        uintptr_t end = address + rounded_length;

        struct process_anonymous_mapping *split_mapping = NULL;
        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                struct process_anonymous_mapping *mapping =
                    &active_process->anonymous_mappings[index];

                if (mapping->used && address > mapping->start &&
                    end < mapping->end) {
                        split_mapping = mapping;
                        break;
                }
        }

        struct process_anonymous_mapping *split_slot = NULL;
        if (split_mapping != NULL) {
                split_slot =
                    process_find_free_anonymous_mapping(active_process);
                if (split_slot == NULL) {
                        return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
                }
        }

        uintptr_t split_start = 0U;
        uintptr_t split_end = 0U;
        uint32_t split_protection = 0U;

        for (size_t index = 0U; index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
             index++) {
                struct process_anonymous_mapping *mapping =
                    &active_process->anonymous_mappings[index];

                if (!mapping->used ||
                    !address_ranges_overlap(address, end, mapping->start,
                                            mapping->end)) {
                        continue;
                }

                uintptr_t overlap_start =
                    address > mapping->start ? address : mapping->start;
                uintptr_t overlap_end = end < mapping->end ? end : mapping->end;
                for (uintptr_t page = overlap_start; page < overlap_end;
                     page += PAGE_SIZE) {
                        process_release_anonymous_page(active_process, page);
                }

                if (overlap_start == mapping->start &&
                    overlap_end == mapping->end) {
                        bytes_zero(mapping, sizeof(*mapping));
                } else if (overlap_start == mapping->start) {
                        mapping->start = overlap_end;
                } else if (overlap_end == mapping->end) {
                        mapping->end = overlap_start;
                } else {
                        split_start = overlap_end;
                        split_end = mapping->end;
                        split_protection = mapping->protection;
                        mapping->end = overlap_start;
                }
        }

        if (split_mapping != NULL) {
                *split_slot = (struct process_anonymous_mapping){
                    .used = true,
                    .start = split_start,
                    .end = split_end,
                    .protection = split_protection,
                };
        }
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
                                page_release(page);
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

static struct shared_memory_object *shared_memory_find(uint32_t identifier) {
        if (identifier == 0U) {
                return NULL;
        }
        for (size_t index = 0U; index < SHARED_MEMORY_OBJECT_LIMIT; index++) {
                if (shared_memory_objects[index].used &&
                    shared_memory_objects[index].identifier == identifier) {
                        return &shared_memory_objects[index];
                }
        }
        return NULL;
}

static uint32_t shared_memory_allocate_identifier(void) {
        for (size_t attempt = 0U; attempt <= SHARED_MEMORY_OBJECT_LIMIT;
             attempt++) {
                uint32_t candidate = next_shared_memory_identifier++;
                if (next_shared_memory_identifier == 0U) {
                        next_shared_memory_identifier = 1U;
                }
                if (candidate != 0U && shared_memory_find(candidate) == NULL) {
                        return candidate;
                }
        }
        return 0U;
}

static size_t process_shared_memory_free_slot(void) {
        for (size_t slot = 0U; slot < SHARED_MEMORY_PROCESS_LIMIT; slot++) {
                if (active_process->shared_memory[slot].object == NULL) {
                        return slot;
                }
        }
        return SHARED_MEMORY_PROCESS_LIMIT;
}

static bool
process_shared_memory_map_object(struct shared_memory_object *object,
                                 size_t slot) {
        uintptr_t address =
            USER_SHARED_MEMORY_BASE + slot * USER_SHARED_MEMORY_STRIDE;
        size_t mapped = 0U;

        while (mapped < object->page_count) {
                if (!page_table_map(active_process->address_space,
                                    address + mapped * PAGE_SIZE,
                                    (uintptr_t)object->pages[mapped],
                                    VM_PAGE_USER | VM_PAGE_READ |
                                        VM_PAGE_WRITE)) {
                        while (mapped != 0U) {
                                mapped--;
                                if (!page_table_unmap(
                                        active_process->address_space,
                                        address + mapped * PAGE_SIZE)) {
                                        panic("Shared-memory map rollback "
                                              "failed");
                                }
                        }
                        return false;
                }
                mapped++;
        }

        active_process->shared_memory[slot].object = object;
        active_process->shared_memory[slot].address = address;
        object->references++;
        return true;
}

static uint64_t shared_memory_copy_information(
    const struct process_shared_memory_mapping *mapping,
    uintptr_t user_information) {
        struct user_shared_memory_info information;

        bytes_zero(&information, sizeof(information));
        information.identifier = mapping->object->identifier;
        information.address = mapping->address;
        information.size = mapping->object->size;
        user_copy_to(user_information, (const uint8_t *)&information,
                     sizeof(information));
        return 0U;
}

static uint64_t syscall_shared_memory_create(size_t size,
                                             uintptr_t user_information) {
        if (!user_range_is_valid(user_information,
                                 sizeof(struct user_shared_memory_info),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        if (size == 0U || size > SHARED_MEMORY_PAGE_LIMIT * PAGE_SIZE) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        size_t slot = process_shared_memory_free_slot();
        if (slot == SHARED_MEMORY_PROCESS_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_TOO_MANY_FILES;
        }
        struct shared_memory_object *object = NULL;
        for (size_t index = 0U; index < SHARED_MEMORY_OBJECT_LIMIT; index++) {
                if (!shared_memory_objects[index].used) {
                        object = &shared_memory_objects[index];
                        break;
                }
        }
        if (object == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_SPACE;
        }

        bytes_zero(object, sizeof(*object));
        object->used = true;
        object->identifier = shared_memory_allocate_identifier();
        if (object->identifier == 0U) {
                bytes_zero(object, sizeof(*object));
                return (uint64_t)-(int64_t)USER_ERROR_NO_SPACE;
        }
        object->size = size;
        size_t page_count = (size + PAGE_SIZE - 1U) / PAGE_SIZE;
        while (object->page_count < page_count) {
                void *page = page_alloc();
                if (page == NULL) {
                        while (object->page_count != 0U) {
                                object->page_count--;
                                page_release(object->pages[object->page_count]);
                        }
                        bytes_zero(object, sizeof(*object));
                        return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
                }
                object->pages[object->page_count++] = page;
        }

        if (!process_shared_memory_map_object(object, slot)) {
                while (object->page_count != 0U) {
                        object->page_count--;
                        page_release(object->pages[object->page_count]);
                }
                bytes_zero(object, sizeof(*object));
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }
        return shared_memory_copy_information(
            &active_process->shared_memory[slot], user_information);
}

static uint64_t syscall_shared_memory_map(uint32_t identifier,
                                          uintptr_t user_information) {
        if (!user_range_is_valid(user_information,
                                 sizeof(struct user_shared_memory_info),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct shared_memory_object *object = shared_memory_find(identifier);
        if (object == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_NO_ENTRY;
        }
        for (size_t slot = 0U; slot < SHARED_MEMORY_PROCESS_LIMIT; slot++) {
                if (active_process->shared_memory[slot].object == object) {
                        return shared_memory_copy_information(
                            &active_process->shared_memory[slot],
                            user_information);
                }
        }
        size_t slot = process_shared_memory_free_slot();
        if (slot == SHARED_MEMORY_PROCESS_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_TOO_MANY_FILES;
        }
        if (!process_shared_memory_map_object(object, slot)) {
                return (uint64_t)-(int64_t)USER_ERROR_OUT_OF_MEMORY;
        }
        return shared_memory_copy_information(
            &active_process->shared_memory[slot], user_information);
}

static uint64_t syscall_shared_memory_unmap(uint32_t identifier) {
        for (size_t slot = 0U; slot < SHARED_MEMORY_PROCESS_LIMIT; slot++) {
                struct shared_memory_object *object =
                    active_process->shared_memory[slot].object;
                if (object != NULL && object->identifier == identifier) {
                        process_shared_memory_unmap_slot(active_process, slot);
                        return 0U;
                }
        }
        return (uint64_t)-(int64_t)USER_ERROR_NO_ENTRY;
}

static uint64_t syscall_open_pseudo_terminal(uintptr_t user_descriptors) {
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

        struct process_terminal *terminal = process_terminal_allocate();
        if (terminal == NULL) {
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }

        struct process_pipe *to_slave = process_pipe_allocate();
        struct process_pipe *to_master = process_pipe_allocate();
        if (to_slave == NULL || to_master == NULL) {
                if (to_slave != NULL) {
                        bytes_zero(to_slave, sizeof(*to_slave));
                }
                if (to_master != NULL) {
                        bytes_zero(to_master, sizeof(*to_master));
                }
                bytes_zero(terminal, sizeof(*terminal));
                return (uint64_t)-(int64_t)USER_ERROR_FILE_TABLE_OVERFLOW;
        }

        terminal->input_pipe = to_slave;
        terminal->output_pipe = to_master;
        terminal->open_references = 2U;

        struct process_open_file *master = process_open_file_allocate();
        struct process_open_file *slave = process_open_file_allocate();
        if (master == NULL || slave == NULL) {
                panic("Reserved pseudo-terminal records disappeared");
        }
        master->access = DESCRIPTOR_READ | DESCRIPTOR_WRITE;
        master->read_pipe = to_master;
        master->write_pipe = to_slave;
        master->terminal = terminal;
        master->terminal_endpoint = PROCESS_TERMINAL_MASTER;
        slave->access = DESCRIPTOR_READ | DESCRIPTOR_WRITE;
        slave->read_pipe = to_slave;
        slave->write_pipe = to_master;
        slave->terminal = terminal;
        slave->terminal_endpoint = PROCESS_TERMINAL_SLAVE;
        process_pipe_retain(to_master, DESCRIPTOR_READ);
        process_pipe_retain(to_master, DESCRIPTOR_WRITE);
        process_pipe_retain(to_slave, DESCRIPTOR_READ);
        process_pipe_retain(to_slave, DESCRIPTOR_WRITE);
        process_descriptor_install(
            &active_process->descriptors[descriptor_pair[0]], master);
        process_descriptor_install(
            &active_process->descriptors[descriptor_pair[1]], slave);

        int result[2] = {(int)descriptor_pair[0], (int)descriptor_pair[1]};
        user_copy_to(user_descriptors, (const uint8_t *)result, sizeof(result));
        return 0U;
}

static uint64_t syscall_system_info(uintptr_t user_information) {
        if (!user_range_is_valid(user_information,
                                 sizeof(struct user_system_info),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        struct user_system_info information;
        bytes_zero(&information, sizeof(information));
        information.total_pages = page_total_count();
        information.free_pages = page_free_count();
        information.used_pages = page_used_count();
        information.context_switches = scheduler_context_switches;
        information.scheduler_preemptions = scheduler_preemptions;
        information.scheduler_blocks = scheduler_blocks;
        information.copy_on_write_faults = copy_on_write_faults;
        information.copy_on_write_copies = copy_on_write_copies;
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                if (process_table[index].state != PROCESS_UNUSED &&
                    process_table[index].state != PROCESS_EXITED) {
                        information.process_count++;
                }
        }
        user_copy_to(user_information, (const uint8_t *)&information,
                     sizeof(information));
        return 0U;
}

static uint64_t syscall_process_list(uintptr_t user_processes,
                                     size_t capacity) {
        if (capacity > USER_PROCESS_INFO_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (capacity != 0U &&
            !user_range_is_valid(user_processes,
                                 capacity * sizeof(struct user_process_info),
                                 VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }

        size_t count = 0U;
        for (size_t index = 0U; index < PROCESS_LIMIT && count < capacity;
             index++) {
                const struct process *process = &process_table[index];
                if (process->state == PROCESS_UNUSED ||
                    process->state == PROCESS_EXITED) {
                        continue;
                }
                struct user_process_info information;
                bytes_zero(&information, sizeof(information));
                information.pid = process->pid;
                information.parent_pid = process->parent_pid;
                information.process_group = process->process_group;
                information.session_id = process->session_id;
                information.resident_pages =
                    process->loaded_image.page_count +
                    (process->stack_page != NULL ? 1U : 0U);
                if (process->heap_break > process->heap_start) {
                        information.resident_pages +=
                            (align_up_to_page(process->heap_break) -
                             process->heap_start) /
                            PAGE_SIZE;
                }
                for (size_t mapping_index = 0U;
                     mapping_index < PROCESS_ANONYMOUS_MAPPING_LIMIT;
                     mapping_index++) {
                        const struct process_anonymous_mapping *mapping =
                            &process->anonymous_mappings[mapping_index];
                        if (!mapping->used) continue;
                        for (uintptr_t address = mapping->start;
                             address < mapping->end; address += PAGE_SIZE) {
                                uintptr_t physical_address;
                                if (page_table_translate(process->address_space,
                                                         address,
                                                         &physical_address,
                                                         NULL)) {
                                        information.resident_pages++;
                                }
                        }
                }
                for (size_t descriptor = 0U;
                     descriptor < PROCESS_DESCRIPTOR_LIMIT; descriptor++) {
                        if (process->descriptors[descriptor].open_file != NULL)
                                information.open_descriptors++;
                }
                information.pending_signals = process->pending_signals;
                switch (process->state) {
                case PROCESS_READY:
                        information.state = USER_PROCESS_READY;
                        break;
                case PROCESS_RUNNING:
                        information.state = USER_PROCESS_RUNNING;
                        break;
                case PROCESS_BLOCKED:
                        information.state = USER_PROCESS_BLOCKED;
                        break;
                case PROCESS_STOPPED:
                        information.state = USER_PROCESS_STOPPED;
                        break;
                case PROCESS_UNUSED:
                case PROCESS_EXITED:
                default:
                        panic("Invalid process state in snapshot");
                }
                for (size_t character = 0U;
                     character < sizeof(information.name); character++) {
                        information.name[character] =
                            process->executable[character];
                }
                user_copy_to(
                    user_processes + count * sizeof(struct user_process_info),
                    (const uint8_t *)&information, sizeof(information));
                count++;
        }
        return count;
}

static uint64_t saturating_add(uint64_t left, uint64_t right) {
        return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

static uint64_t milliseconds_to_nanoseconds(int64_t milliseconds) {
        const uint64_t nanoseconds_per_millisecond = UINT64_C(1000000);
        if (milliseconds <= 0) {
                return 0U;
        }
        if ((uint64_t)milliseconds > UINT64_MAX / nanoseconds_per_millisecond) {
                return UINT64_MAX;
        }
        return (uint64_t)milliseconds * nanoseconds_per_millisecond;
}

static void process_wait_begin(enum process_pending_wait wait,
                               bool has_deadline,
                               uint64_t duration_nanoseconds) {
        if (active_process->pending_wait == PROCESS_WAIT_NONE) {
                active_process->pending_wait = wait;
                active_process->pending_wait_has_deadline = has_deadline;
                active_process->pending_wait_deadline =
                    has_deadline ? saturating_add(timer_monotonic_nanoseconds(),
                                                  duration_nanoseconds)
                                 : 0U;
                return;
        }
        if (active_process->pending_wait != wait) {
                panic("Mismatched blocking wait continuation");
        }
}

static bool process_wait_deadline_reached(void) {
        return active_process->pending_wait_has_deadline &&
               timer_monotonic_nanoseconds() >=
                   active_process->pending_wait_deadline;
}

static void process_wait_complete(void) {
        active_process->pending_wait = PROCESS_WAIT_NONE;
        active_process->pending_wait_has_deadline = false;
        active_process->pending_wait_deadline = 0U;
}

static void process_wait_block(struct trap_frame *frame,
                               enum scheduler_wait_channel channel) {
        if (active_process->pending_wait_has_deadline) {
                scheduler_block_current_until(
                    frame, channel, active_process->pending_wait_deadline);
        } else {
                scheduler_block_current(frame, channel);
        }
}

static uint32_t descriptor_ready_events(int32_t descriptor,
                                        uint32_t requested) {
        if (descriptor < 0) {
                return 0U;
        }
        if ((uint32_t)descriptor >= PROCESS_DESCRIPTOR_LIMIT) {
                return USER_POLL_INVALID;
        }

        const struct process_open_file *open_file =
            active_process->descriptors[(size_t)descriptor].open_file;
        if (open_file == NULL) {
                return USER_POLL_INVALID;
        }

        uint32_t returned = 0U;
        if (open_file->read_pipe != NULL) {
                const struct process_pipe *pipe = open_file->read_pipe;
                bool canonical_slave =
                    open_file->terminal != NULL &&
                    open_file->terminal_endpoint == PROCESS_TERMINAL_SLAVE &&
                    (open_file->terminal->attributes.flags &
                     USER_TERMINAL_CANONICAL) != 0U;
                bool input_ready = !canonical_slave ||
                                   open_file->terminal->canonical_ready != 0U ||
                                   open_file->terminal->canonical_eof;
                if ((requested & USER_POLL_READABLE) != 0U &&
                    ((pipe->count != 0U && input_ready) ||
                     pipe->writers == 0U ||
                     (canonical_slave && open_file->terminal->canonical_eof))) {
                        returned |= USER_POLL_READABLE;
                }
                if (pipe->writers == 0U) {
                        returned |= USER_POLL_HANGUP;
                }
        }
        if (open_file->write_pipe != NULL) {
                const struct process_pipe *pipe = open_file->write_pipe;
                if (pipe->readers == 0U) {
                        returned |= USER_POLL_ERROR | USER_POLL_HANGUP;
                } else if ((requested & USER_POLL_WRITABLE) != 0U &&
                           pipe->count < PIPE_BUFFER_SIZE) {
                        returned |= USER_POLL_WRITABLE;
                }
        }
        if (open_file->read_pipe != NULL || open_file->write_pipe != NULL) {
                return returned;
        }

        if (open_file->file.type == VFS_NODE_CHARACTER_DEVICE) {
                if ((requested & USER_POLL_READABLE) != 0U &&
                    (open_file->access & DESCRIPTOR_READ) != 0U &&
                    uart_read_ready()) {
                        returned |= USER_POLL_READABLE;
                }
                if ((requested & USER_POLL_WRITABLE) != 0U &&
                    (open_file->access & DESCRIPTOR_WRITE) != 0U &&
                    (uart_write_owner == NULL ||
                     uart_write_owner == active_process) &&
                    uart_write_ready()) {
                        returned |= USER_POLL_WRITABLE;
                }
                return returned;
        }

        if ((requested & USER_POLL_READABLE) != 0U &&
            (open_file->access & DESCRIPTOR_READ) != 0U) {
                returned |= USER_POLL_READABLE;
        }
        if ((requested & USER_POLL_WRITABLE) != 0U &&
            (open_file->access & DESCRIPTOR_WRITE) != 0U) {
                returned |= USER_POLL_WRITABLE;
        }
        return returned;
}

static uint64_t poll_validate(uintptr_t user_descriptors, size_t count) {
        if (count > PROCESS_DESCRIPTOR_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (count != 0U &&
            !user_range_is_valid(user_descriptors,
                                 count * sizeof(struct user_poll_descriptor),
                                 VM_PAGE_READ | VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        return 0U;
}

static size_t poll_scan(uintptr_t user_descriptors, size_t count) {
        size_t ready = 0U;
        for (size_t index = 0U; index < count; index++) {
                uintptr_t address = user_descriptors +
                                    index * sizeof(struct user_poll_descriptor);
                struct user_poll_descriptor descriptor;
                user_copy_from((uint8_t *)&descriptor, address,
                               sizeof(descriptor));
                descriptor.returned_events = descriptor_ready_events(
                    descriptor.descriptor, descriptor.events);
                if (descriptor.returned_events != 0U) {
                        ready++;
                }
                user_copy_to(address, (const uint8_t *)&descriptor,
                             sizeof(descriptor));
        }
        return ready;
}

static void syscall_sleep(struct trap_frame *frame,
                          uint64_t duration_nanoseconds) {
        process_wait_begin(PROCESS_WAIT_SLEEP, true, duration_nanoseconds);
        if (process_wait_deadline_reached()) {
                process_wait_complete();
                frame->a0 = 0U;
                frame->sepc += 4U;
                return;
        }
        process_wait_block(frame, SCHEDULER_WAIT_TIMER);
}

static void syscall_poll(struct trap_frame *frame, uintptr_t user_descriptors,
                         size_t count, int64_t timeout_milliseconds) {
        uint64_t validation = poll_validate(user_descriptors, count);
        if (validation != 0U) {
                process_wait_complete();
                frame->a0 = validation;
                frame->sepc += 4U;
                return;
        }

        bool has_deadline = timeout_milliseconds >= 0;
        process_wait_begin(PROCESS_WAIT_POLL, has_deadline,
                           milliseconds_to_nanoseconds(timeout_milliseconds));
        size_t ready = poll_scan(user_descriptors, count);
        if (ready != 0U || process_wait_deadline_reached()) {
                process_wait_complete();
                frame->a0 = ready;
                frame->sepc += 4U;
                return;
        }
        process_wait_block(frame, SCHEDULER_WAIT_EVENT);
}

static uint64_t child_ready_events(int64_t requested_pid, uint32_t requested,
                                   bool *found_child) {
        uint32_t returned = 0U;
        *found_child = false;
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                const struct process *child = &process_table[index];
                if (!process_is_waitable_child(child, requested_pid)) {
                        continue;
                }
                *found_child = true;
                if (child->state == PROCESS_EXITED &&
                    (requested & USER_WAIT_EVENT_CHILD_EXITED) != 0U) {
                        returned |= USER_WAIT_EVENT_CHILD_EXITED;
                }
                if (child->stop_event &&
                    (requested & USER_WAIT_EVENT_CHILD_STOPPED) != 0U) {
                        returned |= USER_WAIT_EVENT_CHILD_STOPPED;
                }
                if (child->continued_event &&
                    (requested & USER_WAIT_EVENT_CHILD_CONTINUED) != 0U) {
                        returned |= USER_WAIT_EVENT_CHILD_CONTINUED;
                }
        }
        return returned;
}

static uint64_t wait_item_scan(struct user_wait_item *item) {
        item->returned_events = 0U;
        if (item->reserved != 0U) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }

        switch (item->type) {
        case USER_WAIT_OBJECT_DESCRIPTOR:
                if ((item->events & ~(uint32_t)(USER_WAIT_EVENT_READABLE |
                                                USER_WAIT_EVENT_WRITABLE)) !=
                        0U ||
                    item->identifier < INT32_MIN ||
                    item->identifier > INT32_MAX) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                item->returned_events = descriptor_ready_events(
                    (int32_t)item->identifier, item->events);
                return 0U;

        case USER_WAIT_OBJECT_INPUT:
                if (item->identifier != 0 || item->value != 0U ||
                    item->events != USER_WAIT_EVENT_READABLE) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                if (graphics_owner_pid != active_process->pid) {
                        return (uint64_t)-(int64_t)USER_ERROR_PERMISSION;
                }
                if (input_event_available()) {
                        item->returned_events = USER_WAIT_EVENT_READABLE;
                }
                return 0U;

        case USER_WAIT_OBJECT_CHILD: {
                const uint32_t child_events = USER_WAIT_EVENT_CHILD_EXITED |
                                              USER_WAIT_EVENT_CHILD_STOPPED |
                                              USER_WAIT_EVENT_CHILD_CONTINUED;
                if ((item->events & ~child_events) != 0U ||
                    item->events == 0U || item->value != 0U ||
                    item->identifier == INT64_MIN) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                bool found_child;
                item->returned_events = (uint32_t)child_ready_events(
                    item->identifier, item->events, &found_child);
                return found_child ? 0U
                                   : (uint64_t)-(int64_t)USER_ERROR_NO_CHILD;
        }

        case USER_WAIT_OBJECT_SHARED_WORD: {
                if (item->events != USER_WAIT_EVENT_CHANGED) {
                        return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
                }
                if (item->identifier <= 0 ||
                    !user_range_is_valid((uintptr_t)item->identifier,
                                         sizeof(uint32_t), VM_PAGE_READ)) {
                        return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
                }
                uint32_t current;
                user_copy_from((uint8_t *)&current, (uintptr_t)item->identifier,
                               sizeof(current));
                if (current != (uint32_t)item->value) {
                        item->returned_events = USER_WAIT_EVENT_CHANGED;
                        item->value = current;
                }
                return 0U;
        }

        default:
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
}

static uint64_t wait_events_validate(uintptr_t user_items, size_t count) {
        if (count > USER_WAIT_ITEM_LIMIT) {
                return (uint64_t)-(int64_t)USER_ERROR_INVALID_ARGUMENT;
        }
        if (count != 0U &&
            !user_range_is_valid(user_items,
                                 count * sizeof(struct user_wait_item),
                                 VM_PAGE_READ | VM_PAGE_WRITE)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        return 0U;
}

static uint64_t wait_events_scan(uintptr_t user_items, size_t count,
                                 size_t *ready) {
        *ready = 0U;
        for (size_t index = 0U; index < count; index++) {
                uintptr_t address =
                    user_items + index * sizeof(struct user_wait_item);
                struct user_wait_item item;
                user_copy_from((uint8_t *)&item, address, sizeof(item));
                uint64_t result = wait_item_scan(&item);
                if (result != 0U) {
                        return result;
                }
                if (item.returned_events != 0U) {
                        (*ready)++;
                }
                user_copy_to(address, (const uint8_t *)&item, sizeof(item));
        }
        return 0U;
}

static void syscall_wait_events(struct trap_frame *frame, uintptr_t user_items,
                                size_t count, int64_t timeout_nanoseconds) {
        uint64_t validation = wait_events_validate(user_items, count);
        if (validation != 0U) {
                process_wait_complete();
                frame->a0 = validation;
                frame->sepc += 4U;
                return;
        }

        bool has_deadline = timeout_nanoseconds >= 0;
        process_wait_begin(PROCESS_WAIT_EVENTS, has_deadline,
                           has_deadline ? (uint64_t)timeout_nanoseconds : 0U);
        size_t ready;
        uint64_t result = wait_events_scan(user_items, count, &ready);
        if (result != 0U || ready != 0U || process_wait_deadline_reached()) {
                process_wait_complete();
                frame->a0 = result != 0U ? result : ready;
                frame->sepc += 4U;
                return;
        }
        process_wait_block(frame, SCHEDULER_WAIT_EVENT);
}

static uint64_t syscall_event_notify(uintptr_t user_word) {
        if (!user_range_is_valid(user_word, sizeof(uint32_t), VM_PAGE_READ)) {
                return (uint64_t)-(int64_t)USER_ERROR_BAD_ADDRESS;
        }
        (void)scheduler_wake_all(SCHEDULER_WAIT_EVENT);
        return 0U;
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

static void scheduler_block_current_internal(
    struct trap_frame *frame, enum scheduler_wait_channel channel,
    bool deadline_active, uint64_t deadline_nanoseconds) {
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
        previous->scheduler_deadline_active = deadline_active;
        previous->scheduler_deadline = deadline_nanoseconds;
        scheduler_blocks++;

        struct process *next = scheduler_find_next_ready();

        if (next != NULL) {
                scheduler_context_switches++;
                scheduler_switch_to(next, frame);
                return;
        }

        scheduler_return_to_kernel(frame);
}

void scheduler_block_current(struct trap_frame *frame,
                             enum scheduler_wait_channel channel) {
        scheduler_block_current_internal(frame, channel, false, 0U);
}

void scheduler_block_current_until(struct trap_frame *frame,
                                   enum scheduler_wait_channel channel,
                                   uint64_t deadline_nanoseconds) {
        scheduler_block_current_internal(frame, channel, true,
                                         deadline_nanoseconds);
}

static void scheduler_make_ready(struct process *process) {
        process->state = PROCESS_READY;
        process->wait_channel = SCHEDULER_WAIT_NONE;
        process->scheduler_deadline_active = false;
}

size_t scheduler_wake_all(enum scheduler_wait_channel channel) {
        if (channel == SCHEDULER_WAIT_NONE) {
                return 0U;
        }

        size_t woken = 0U;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_BLOCKED &&
                    (process->wait_channel == channel ||
                     (channel != SCHEDULER_WAIT_EVENT &&
                      process->wait_channel == SCHEDULER_WAIT_EVENT))) {
                        scheduler_make_ready(process);
                        woken++;
                }
        }

        return woken;
}

bool scheduler_wake_one(enum scheduler_wait_channel channel) {
        if (channel == SCHEDULER_WAIT_NONE) {
                return false;
        }

        bool woken = false;
        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];

                if (process->state == PROCESS_BLOCKED &&
                    process->wait_channel == channel) {
                        scheduler_make_ready(process);
                        woken = true;
                        break;
                }
        }

        /* A readiness wait may be interested in the same transition as a
         * traditional blocking read. It is safe to wake all wait sets: each
         * one rescans its objects atomically before sleeping again. */
        if (channel != SCHEDULER_WAIT_EVENT) {
                for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                        struct process *process = &process_table[index];
                        if (process->state == PROCESS_BLOCKED &&
                            process->wait_channel == SCHEDULER_WAIT_EVENT) {
                                scheduler_make_ready(process);
                                woken = true;
                        }
                }
        }

        return woken;
}

size_t scheduler_wake_expired(uint64_t now_nanoseconds) {
        size_t woken = 0U;

        for (size_t index = 0U; index < PROCESS_LIMIT; index++) {
                struct process *process = &process_table[index];
                if (process->state == PROCESS_BLOCKED &&
                    process->scheduler_deadline_active &&
                    now_nanoseconds >= process->scheduler_deadline) {
                        scheduler_make_ready(process);
                        woken++;
                }
        }
        return woken;
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

        if (open_file->write_pipe != NULL) {
                struct process_pipe *pipe = open_file->write_pipe;

                if (pipe->readers == 0U) {
                        process->pending_write = false;
                        frame->a0 = (uint64_t)-(int64_t)USER_ERROR_BROKEN_PIPE;
                        frame->sepc += 4U;
                        return;
                }
                if (open_file->terminal != NULL &&
                    open_file->terminal_endpoint == PROCESS_TERMINAL_MASTER) {
                        while (process->write_offset < process->write_length &&
                               process_terminal_master_write_character(
                                   open_file->terminal,
                                   (uint8_t)process
                                       ->write_buffer[process->write_offset])) {
                                process->write_offset++;
                        }
                        if (process->write_offset != process->write_length) {
                                if (process
                                        ->descriptors[process->write_descriptor]
                                        .nonblocking) {
                                        size_t completed =
                                            process->write_offset;
                                        process->pending_write = false;
                                        frame->a0 =
                                            completed == 0U
                                                ? (uint64_t)-(int64_t)
                                                      USER_ERROR_TRY_AGAIN
                                                : completed;
                                        frame->sepc += 4U;
                                        return;
                                }
                                scheduler_block_current(
                                    frame, SCHEDULER_WAIT_PIPE_WRITE);
                                return;
                        }
                        process->pending_write = false;
                        frame->a0 = process->write_result_length;
                        frame->sepc += 4U;
                        return;
                }
                if (process->write_length > PIPE_BUFFER_SIZE - pipe->count) {
                        if (process->descriptors[process->write_descriptor]
                                .nonblocking) {
                                process->pending_write = false;
                                frame->a0 =
                                    (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
                                frame->sepc += 4U;
                                return;
                        }
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
        if (open_file->terminal != NULL &&
            open_file->terminal_endpoint == PROCESS_TERMINAL_SLAVE &&
            process->controlling_terminal == open_file->terminal &&
            open_file->terminal->foreground_process_group != 0U &&
            process->process_group !=
                open_file->terminal->foreground_process_group) {
                (void)syscall_kill(0, USER_SIGNAL_BACKGROUND_READ);
                return;
        }

        if (open_file->read_pipe != NULL) {
                struct process_pipe *pipe = open_file->read_pipe;
                struct process_terminal *terminal = open_file->terminal;
                bool canonical =
                    terminal != NULL &&
                    open_file->terminal_endpoint == PROCESS_TERMINAL_SLAVE &&
                    (terminal->attributes.flags & USER_TERMINAL_CANONICAL) !=
                        0U;

                if (canonical && pipe->writers == 0U &&
                    terminal->canonical_ready == 0U &&
                    terminal->canonical_pending != 0U) {
                        terminal->canonical_ready = terminal->canonical_pending;
                        terminal->canonical_pending = 0U;
                }
                if (canonical && terminal->canonical_ready == 0U &&
                    terminal->canonical_eof) {
                        terminal->canonical_eof = false;
                        process->pending_read = false;
                        frame->a0 = 0U;
                        frame->sepc += 4U;
                        return;
                }

                if (pipe->count == 0U ||
                    (canonical && terminal->canonical_ready == 0U)) {
                        if (pipe->writers == 0U) {
                                process->pending_read = false;
                                frame->a0 = 0U;
                                frame->sepc += 4U;
                                return;
                        }

                        if (process->descriptors[process->read_descriptor]
                                .nonblocking) {
                                process->pending_read = false;
                                frame->a0 =
                                    (uint64_t)-(int64_t)USER_ERROR_TRY_AGAIN;
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
                if (canonical && pipe_count > terminal->canonical_ready) {
                        pipe_count = terminal->canonical_ready;
                }
                if (canonical) {
                        size_t scan = pipe->read_offset;
                        for (size_t offset = 0U; offset < pipe_count;
                             offset++) {
                                if (pipe->buffer[scan] == (uint8_t)'\n') {
                                        pipe_count = offset + 1U;
                                        break;
                                }
                                scan = (scan + 1U) % PIPE_BUFFER_SIZE;
                        }
                }
                for (size_t offset = 0U; offset < pipe_count; offset++) {
                        process->write_buffer[offset] =
                            (char)pipe->buffer[pipe->read_offset];
                        pipe->read_offset =
                            (pipe->read_offset + 1U) % PIPE_BUFFER_SIZE;
                }
                pipe->count -= pipe_count;
                if (canonical) {
                        terminal->canonical_ready -= pipe_count;
                }
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
        active_process->pending_wait = PROCESS_WAIT_NONE;
        active_process->pending_wait_has_deadline = false;
        active_process->state = PROCESS_EXITED;
        active_process->exit_status = status;
        struct process_terminal *terminal =
            active_process->controlling_terminal;
        uint64_t session_id = active_process->session_id;
        active_process->controlling_terminal = NULL;
        process_terminal_release_empty_session(terminal, session_id);
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
        stopped->scheduler_deadline_active = false;
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
                if (process_signal_ignored_by_default(signal) &&
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
                if (active_process->pending_wait != PROCESS_WAIT_NONE) {
                        process_wait_complete();
                        frame->a0 = (uint64_t)-(int64_t)USER_ERROR_INTERRUPTED;
                        frame->sepc += 4U;
                }
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

        case USER_SYSCALL_RENAME:
                frame->a0 = syscall_rename((uintptr_t)frame->a0,
                                           (uintptr_t)frame->a1);
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

        case USER_SYSCALL_TERMINAL_GET_ATTRIBUTES:
                frame->a0 = syscall_terminal_get_attributes(
                    frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_SET_ATTRIBUTES:
                frame->a0 = syscall_terminal_set_attributes(
                    frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_GET_WINDOW_SIZE:
                frame->a0 = syscall_terminal_get_window_size(
                    frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_SET_WINDOW_SIZE:
                frame->a0 = syscall_terminal_set_window_size(
                    frame->a0, (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_CREATE_SESSION:
                frame->a0 = syscall_create_session();
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_GET_SESSION:
                frame->a0 = syscall_get_session((int64_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_TERMINAL_SET_CONTROLLING:
                frame->a0 = syscall_terminal_set_controlling(frame->a0);
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

        case USER_SYSCALL_SHARED_MEMORY_CREATE:
                frame->a0 = syscall_shared_memory_create((size_t)frame->a0,
                                                         (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SHARED_MEMORY_MAP:
                frame->a0 = syscall_shared_memory_map((uint32_t)frame->a0,
                                                      (uintptr_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SHARED_MEMORY_UNMAP:
                frame->a0 = syscall_shared_memory_unmap((uint32_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_OPEN_PSEUDO_TERMINAL:
                frame->a0 = syscall_open_pseudo_terminal((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SYSTEM_INFO:
                frame->a0 = syscall_system_info((uintptr_t)frame->a0);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_PROCESS_LIST:
                frame->a0 = syscall_process_list((uintptr_t)frame->a0,
                                                 (size_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_MMAP:
                frame->a0 =
                    syscall_mmap((size_t)frame->a0, (uint32_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_MUNMAP:
                frame->a0 =
                    syscall_munmap((uintptr_t)frame->a0, (size_t)frame->a1);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_MPROTECT:
                frame->a0 =
                    syscall_mprotect((uintptr_t)frame->a0, (size_t)frame->a1,
                                     (uint32_t)frame->a2);
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_MONOTONIC_TIME:
                frame->a0 = timer_monotonic_nanoseconds();
                frame->sepc += 4U;
                return;

        case USER_SYSCALL_SLEEP:
                syscall_sleep(frame, frame->a0);
                return;

        case USER_SYSCALL_POLL:
                syscall_poll(frame, (uintptr_t)frame->a0, (size_t)frame->a1,
                             (int64_t)frame->a2);
                return;

        case USER_SYSCALL_WAIT_EVENTS:
                syscall_wait_events(frame, (uintptr_t)frame->a0,
                                    (size_t)frame->a1, (int64_t)frame->a2);
                return;

        case USER_SYSCALL_EVENT_NOTIFY:
                frame->a0 = syscall_event_notify((uintptr_t)frame->a0);
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

/* A store fault on a software-marked COW leaf installs writable private memory
 * and retries the instruction. Other U-mode faults still terminate only that
 * process; supervisor faults remain fatal in the generic trap path. */
void user_process_handle_fault(struct trap_frame *frame, uint64_t cause) {
        if (cause == SCAUSE_STORE_PAGE_FAULT &&
            process_resolve_copy_on_write(active_process,
                                          (uintptr_t)frame->stval)) {
                copy_on_write_faults++;
                return;
        }

        uint64_t required_flags = 0U;
        if (cause == SCAUSE_LOAD_PAGE_FAULT) {
                required_flags = VM_PAGE_READ;
        } else if (cause == SCAUSE_STORE_PAGE_FAULT) {
                required_flags = VM_PAGE_WRITE;
        } else if (cause == SCAUSE_INSTRUCTION_PAGE_FAULT) {
                required_flags = VM_PAGE_EXECUTE;
        }
        if (required_flags != 0U &&
            process_materialize_anonymous_page(
                active_process, (uintptr_t)frame->stval, required_flags)) {
                return;
        }

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
