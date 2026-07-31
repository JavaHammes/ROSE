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

#else

/* System-call numbers shared by the kernel and U-mode runtime. */
enum user_syscall_number {
        USER_SYSCALL_WRITE = 1,
        USER_SYSCALL_EXIT = 2,
        USER_SYSCALL_YIELD = 3,
        USER_SYSCALL_READ = 4,
        USER_SYSCALL_OPEN = 5,
        USER_SYSCALL_CLOSE = 6,
};

/* Stable negative error values returned in a0 by failed system calls. */
enum user_syscall_error {
        USER_ERROR_NO_ENTRY = 2,
        USER_ERROR_BAD_FILE_DESCRIPTOR = 9,
        USER_ERROR_BAD_ADDRESS = 14,
        USER_ERROR_INVALID_ARGUMENT = 22,
        USER_ERROR_TOO_MANY_FILES = 24,
        USER_ERROR_NAME_TOO_LONG = 36,
        USER_ERROR_NOT_IMPLEMENTED = 38,
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
};

#endif

#endif
