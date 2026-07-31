#ifndef USER_ABI_H
#define USER_ABI_H

#ifdef __ASSEMBLER__

/* Keep assembly-visible values as macros; C receives matching enums below. */
#define USER_SYSCALL_WRITE 1
#define USER_SYSCALL_EXIT 2
#define USER_SYSCALL_YIELD 3

#else

/* System-call numbers shared by the kernel and U-mode runtime. */
enum user_syscall_number {
        USER_SYSCALL_WRITE = 1,
        USER_SYSCALL_EXIT = 2,
        USER_SYSCALL_YIELD = 3,
};

/* Stable negative error values returned in a0 by failed system calls. */
enum user_syscall_error {
        USER_ERROR_BAD_ADDRESS = 14,
        USER_ERROR_INVALID_ARGUMENT = 22,
        USER_ERROR_NOT_IMPLEMENTED = 38,
};

/*
 * Initial a0 values understood by the single embedded demonstration ELF. Each
 * value selects one behavior inside user_main.
 */
enum user_program {
        USER_PROGRAM_HELLO,
        USER_PROGRAM_FAULT,
        USER_PROGRAM_MULTI_A,
        USER_PROGRAM_MULTI_B,
        USER_PROGRAM_SYSCALL_TEST,
};

#endif

#endif
