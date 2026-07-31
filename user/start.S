/* Minimal freestanding C runtime entry. The kernel supplies a valid stack. */
.section .text.entry,"ax",@progbits
.align 2

.globl _start
.type _start, @function
_start:
    /* a0 contains the program selector and is user_main's first argument. */
    call user_main

    /* Returning from main terminates the process with its return value. */
    tail rose_exit

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
