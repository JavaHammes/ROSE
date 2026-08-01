#ifndef USER_ABI_H
#define USER_ABI_H

#ifdef __ASSEMBLER__

/* Keep assembly-visible values as macros; C receives matching enums below. */
#define USER_SYSCALL_WRITE 1
#define USER_SYSCALL_EXIT 2
#define USER_SYSCALL_YIELD 3
#define USER_SYSCALL_READ 4
#define USER_SYSCALL_OPEN 5
#define USER_SYSCALL_CLOSE 6
#define USER_SYSCALL_STAT 7
#define USER_SYSCALL_LSEEK 8
#define USER_SYSCALL_READ_DIRECTORY 9
#define USER_SYSCALL_MKDIR 10
#define USER_SYSCALL_UNLINK 11
#define USER_SYSCALL_EXECVE 12
#define USER_SYSCALL_GETPID 13
#define USER_SYSCALL_WAITPID 14
#define USER_SYSCALL_SPAWN 15
#define USER_SYSCALL_BRK 16
#define USER_SYSCALL_CHDIR 17
#define USER_SYSCALL_GETCWD 18
#define USER_SYSCALL_FSTAT 19
#define USER_SYSCALL_DUP 20
#define USER_SYSCALL_DUP2 21
#define USER_SYSCALL_PIPE 22
#define USER_SYSCALL_SET_DESCRIPTOR_FLAGS 23
#define USER_SYSCALL_FORK 24
#define USER_SYSCALL_SIGNAL_ACTION 25
#define USER_SYSCALL_KILL 26
#define USER_SYSCALL_SIGNAL_RETURN 27
#define USER_SYSCALL_SET_PROCESS_GROUP 28
#define USER_SYSCALL_GET_PROCESS_GROUP 29
#define USER_SYSCALL_TERMINAL_SET_FOREGROUND_GROUP 30
#define USER_SYSCALL_TERMINAL_GET_FOREGROUND_GROUP 31
#define USER_SYSCALL_GRAPHICS_MAP 32
#define USER_SYSCALL_GRAPHICS_FLUSH 33
#define USER_SYSCALL_INPUT_READ 34
#define USER_SYSCALL_SHARED_MEMORY_CREATE 35
#define USER_SYSCALL_SHARED_MEMORY_MAP 36
#define USER_SYSCALL_SHARED_MEMORY_UNMAP 37
#define USER_SYSCALL_OPEN_PSEUDO_TERMINAL 38
#define USER_SYSCALL_SYSTEM_INFO 39
#define USER_SYSCALL_MMAP 40
#define USER_SYSCALL_MUNMAP 41
#define USER_SYSCALL_MPROTECT 42

#else

/* System-call numbers shared by the kernel and U-mode runtime. */
enum user_syscall_number {
        USER_SYSCALL_WRITE = 1,
        USER_SYSCALL_EXIT = 2,
        USER_SYSCALL_YIELD = 3,
        USER_SYSCALL_READ = 4,
        USER_SYSCALL_OPEN = 5,
        USER_SYSCALL_CLOSE = 6,
        USER_SYSCALL_STAT = 7,
        USER_SYSCALL_LSEEK = 8,
        USER_SYSCALL_READ_DIRECTORY = 9,
        USER_SYSCALL_MKDIR = 10,
        USER_SYSCALL_UNLINK = 11,
        USER_SYSCALL_EXECVE = 12,
        USER_SYSCALL_GETPID = 13,
        USER_SYSCALL_WAITPID = 14,
        USER_SYSCALL_SPAWN = 15,
        USER_SYSCALL_BRK = 16,
        USER_SYSCALL_CHDIR = 17,
        USER_SYSCALL_GETCWD = 18,
        USER_SYSCALL_FSTAT = 19,
        USER_SYSCALL_DUP = 20,
        USER_SYSCALL_DUP2 = 21,
        USER_SYSCALL_PIPE = 22,
        USER_SYSCALL_SET_DESCRIPTOR_FLAGS = 23,
        USER_SYSCALL_FORK = 24,
        USER_SYSCALL_SIGNAL_ACTION = 25,
        USER_SYSCALL_KILL = 26,
        USER_SYSCALL_SIGNAL_RETURN = 27,
        USER_SYSCALL_SET_PROCESS_GROUP = 28,
        USER_SYSCALL_GET_PROCESS_GROUP = 29,
        USER_SYSCALL_TERMINAL_SET_FOREGROUND_GROUP = 30,
        USER_SYSCALL_TERMINAL_GET_FOREGROUND_GROUP = 31,
        USER_SYSCALL_GRAPHICS_MAP = 32,
        USER_SYSCALL_GRAPHICS_FLUSH = 33,
        USER_SYSCALL_INPUT_READ = 34,
        USER_SYSCALL_SHARED_MEMORY_CREATE = 35,
        USER_SYSCALL_SHARED_MEMORY_MAP = 36,
        USER_SYSCALL_SHARED_MEMORY_UNMAP = 37,
        USER_SYSCALL_OPEN_PSEUDO_TERMINAL = 38,
        USER_SYSCALL_SYSTEM_INFO = 39,
        USER_SYSCALL_MMAP = 40,
        USER_SYSCALL_MUNMAP = 41,
        USER_SYSCALL_MPROTECT = 42,
};

/* Stable negative error values returned in a0 by failed system calls. */
enum user_syscall_error {
        USER_ERROR_NO_ENTRY = 2,
        USER_ERROR_NO_PROCESS = 3,
        USER_ERROR_INTERRUPTED = 4,
        USER_ERROR_IO = 5,
        USER_ERROR_ARGUMENT_LIST_TOO_LONG = 7,
        USER_ERROR_EXEC_FORMAT = 8,
        USER_ERROR_BAD_FILE_DESCRIPTOR = 9,
        USER_ERROR_NO_CHILD = 10,
        USER_ERROR_TRY_AGAIN = 11,
        USER_ERROR_OUT_OF_MEMORY = 12,
        USER_ERROR_PERMISSION = 13,
        USER_ERROR_BAD_ADDRESS = 14,
        USER_ERROR_EXISTS = 17,
        USER_ERROR_NOT_DIRECTORY = 20,
        USER_ERROR_IS_DIRECTORY = 21,
        USER_ERROR_INVALID_ARGUMENT = 22,
        USER_ERROR_FILE_TABLE_OVERFLOW = 23,
        USER_ERROR_TOO_MANY_FILES = 24,
        USER_ERROR_NO_SPACE = 28,
        USER_ERROR_READ_ONLY = 30,
        USER_ERROR_BROKEN_PIPE = 32,
        USER_ERROR_RANGE = 34,
        USER_ERROR_NAME_TOO_LONG = 36,
        USER_ERROR_NOT_IMPLEMENTED = 38,
        USER_ERROR_NOT_EMPTY = 39,
};

enum user_open_flags {
        USER_OPEN_READ = (1U << 0),
        USER_OPEN_WRITE = (1U << 1),
        USER_OPEN_CREATE = (1U << 2),
        USER_OPEN_TRUNCATE = (1U << 3),
        USER_OPEN_DIRECTORY = (1U << 4),
};

enum user_descriptor_flags {
        USER_DESCRIPTOR_CLOSE_ON_EXEC = (1U << 0),
        USER_DESCRIPTOR_NONBLOCK = (1U << 1),
};

enum user_seek_whence {
        USER_SEEK_SET,
        USER_SEEK_CURRENT,
        USER_SEEK_END,
};

/* Mappings are currently anonymous and private. Writable mappings must also be
 * readable on RISC-V, and writable/executable mappings are rejected to
 * preserve W^X. */
enum user_memory_protection {
        USER_MEMORY_PROTECTION_NONE = 0,
        USER_MEMORY_PROTECTION_READ = (1U << 0),
        USER_MEMORY_PROTECTION_WRITE = (1U << 1),
        USER_MEMORY_PROTECTION_EXECUTE = (1U << 2),
};

enum user_wait_options {
        USER_WAIT_NO_HANG = (1U << 0),
        USER_WAIT_UNTRACED = (1U << 1),
        USER_WAIT_CONTINUED = (1U << 2),
};

#define USER_WAIT_STATUS_EXITED(status) (((uint32_t)(status) & 0x7fU) == 0U)
#define USER_WAIT_STATUS_EXIT_CODE(status) (((uint32_t)(status) >> 8U) & 0xffU)
#define USER_WAIT_STATUS_SIGNALED(status)                                      \
        ((((uint32_t)(status) & 0x7fU) != 0U) &&                               \
         (((uint32_t)(status) & 0x7fU) != 0x7fU))
#define USER_WAIT_STATUS_TERMINATION_SIGNAL(status) ((uint32_t)(status) & 0x7fU)
#define USER_WAIT_STATUS_STOPPED(status) (((uint32_t)(status) & 0xffU) == 0x7fU)
#define USER_WAIT_STATUS_STOP_SIGNAL(status)                                   \
        (((uint32_t)(status) >> 8U) & 0xffU)
#define USER_WAIT_STATUS_CONTINUED(status) ((uint32_t)(status) == 0xffffU)

/* Initial process-directed signals. Values follow the conventional Unix ABI
 * so wait statuses and programs can use familiar numbers. */
enum user_signal_number {
        USER_SIGNAL_INTERRUPT = 2,
        USER_SIGNAL_KILL = 9,
        USER_SIGNAL_USER_1 = 10,
        USER_SIGNAL_TERMINATE = 15,
        USER_SIGNAL_CONTINUE = 18,
        USER_SIGNAL_STOP = 19,
        USER_SIGNAL_TERMINAL_STOP = 20,
        USER_SIGNAL_BACKGROUND_READ = 21,
        USER_SIGNAL_MAX = 31,
};

#define USER_SIGNAL_DEFAULT ((uintptr_t)0U)
#define USER_SIGNAL_IGNORE ((uintptr_t)1U)

struct user_signal_action {
        uintptr_t handler;
        uint64_t flags;
};

enum user_file_type {
        USER_FILE_DIRECTORY,
        USER_FILE_REGULAR,
        USER_FILE_CHARACTER_DEVICE,
        USER_FILE_PIPE,
};

enum { USER_DIRECTORY_NAME_MAX = 56 };

struct user_file_status {
        uint64_t size;
        uint32_t inode;
        uint32_t mode;
        uint32_t type;
        uint32_t reserved;
};

struct user_directory_entry {
        uint32_t inode;
        uint32_t type;
        char name[USER_DIRECTORY_NAME_MAX];
};

enum user_graphics_pixel_format {
        USER_GRAPHICS_PIXEL_XRGB8888 = 1,
};

struct user_graphics_info {
        uintptr_t framebuffer;
        uint64_t framebuffer_size;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t pixel_format;
};

enum user_input_event_type {
        USER_INPUT_EVENT_KEY = 1,
        USER_INPUT_EVENT_POINTER = 2,
};

enum user_pointer_button {
        USER_POINTER_BUTTON_LEFT = (1U << 0),
        USER_POINTER_BUTTON_RIGHT = (1U << 1),
        USER_POINTER_BUTTON_MIDDLE = (1U << 2),
};

struct user_input_event {
        uint32_t type;
        uint32_t code;
        int32_t value;
        int32_t x;
        int32_t y;
        uint32_t buttons;
};

struct user_shared_memory_info {
        uint32_t identifier;
        uint32_t reserved;
        uintptr_t address;
        uint64_t size;
};

struct user_system_info {
        uint64_t total_pages;
        uint64_t free_pages;
        uint64_t used_pages;
        uint64_t context_switches;
        uint64_t scheduler_preemptions;
        uint64_t scheduler_blocks;
        uint64_t copy_on_write_faults;
        uint64_t copy_on_write_copies;
        uint32_t process_count;
        uint32_t reserved;
};

enum user_standard_file_descriptor {
        USER_STDIN_FILENO = 0,
        USER_STDOUT_FILENO = 1,
        USER_STDERR_FILENO = 2,
};

/* Build-time selectors used to produce the distinct demonstration ELFs. */
enum user_program {
        USER_PROGRAM_HELLO,
        USER_PROGRAM_FAULT,
        USER_PROGRAM_MULTI_A,
        USER_PROGRAM_MULTI_B,
        USER_PROGRAM_SYSCALL_TEST,
        USER_PROGRAM_CAT,
        USER_PROGRAM_CONSOLE_READ,
        USER_PROGRAM_INIT,
        USER_PROGRAM_FS_TEST,
        USER_PROGRAM_ARGUMENTS_ENVIRONMENT,
        USER_PROGRAM_EXECVE,
        USER_PROGRAM_EXECVE_TARGET,
        USER_PROGRAM_PIPE_TEST,
        USER_PROGRAM_PIPE_WRITER,
        USER_PROGRAM_SH,
        USER_PROGRAM_LS,
        USER_PROGRAM_ECHO,
        USER_PROGRAM_PWD,
        USER_PROGRAM_ENV,
        USER_PROGRAM_MKDIR,
        USER_PROGRAM_RM,
        USER_PROGRAM_DESCRIPTOR_TEST,
        USER_PROGRAM_SIGNAL_EXEC_TEST,
        USER_PROGRAM_DESKTOP,
        USER_PROGRAM_GUI_TERMINAL,
        USER_PROGRAM_GUI_FILES,
        USER_PROGRAM_GUI_SYSTEM_MONITOR,
};

#endif

#endif
