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
};

/* Stable negative error values returned in a0 by failed system calls. */
enum user_syscall_error {
        USER_ERROR_NO_ENTRY = 2,
        USER_ERROR_IO = 5,
        USER_ERROR_BAD_FILE_DESCRIPTOR = 9,
        USER_ERROR_PERMISSION = 13,
        USER_ERROR_BAD_ADDRESS = 14,
        USER_ERROR_EXISTS = 17,
        USER_ERROR_NOT_DIRECTORY = 20,
        USER_ERROR_IS_DIRECTORY = 21,
        USER_ERROR_INVALID_ARGUMENT = 22,
        USER_ERROR_TOO_MANY_FILES = 24,
        USER_ERROR_NO_SPACE = 28,
        USER_ERROR_READ_ONLY = 30,
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

enum user_seek_whence {
        USER_SEEK_SET,
        USER_SEEK_CURRENT,
        USER_SEEK_END,
};

enum user_file_type {
        USER_FILE_DIRECTORY,
        USER_FILE_REGULAR,
        USER_FILE_CHARACTER_DEVICE,
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
};

#endif

#endif
