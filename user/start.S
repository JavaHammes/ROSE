/* Minimal freestanding C runtime entry. The kernel supplies a valid stack. */
.section .text.entry,"ax",@progbits
.align 2

.globl _start
.type _start, @function
_start:
    /* Decode argc/argv/envp from the conventional initial process stack. */
    ld a0, 0(sp)
    addi a1, sp, 8
    addi t0, a0, 1
    slli t0, t0, 3
    add a2, a1, t0
    call user_main

    /* Returning from main terminates the process with its return value. */
    tail rose_exit

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
