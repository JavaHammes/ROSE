/*
 * Thin RISC-V syscall stubs for the freestanding user runtime.
 *
 * a0-a5 carry arguments, a7 selects the syscall, and the kernel returns its
 * result in a0. The kernel advances sepc past ecall before returning here.
 */
#include "user_abi.h"

.section .text,"ax",@progbits
.align 2

.globl rose_write
.type rose_write, @function
rose_write:
    /* Arguments already arrive in a0 (buffer) and a1 (length). */
    li a7, USER_SYSCALL_WRITE
    ecall
    ret
.size rose_write, . - rose_write

.globl rose_exit
.type rose_exit, @function
rose_exit:
    /* a0 contains the status. A successful exit never resumes this stub. */
    li a7, USER_SYSCALL_EXIT
    ecall
1:
    j 1b
.size rose_exit, . - rose_exit

.globl rose_yield
.type rose_yield, @function
rose_yield:
    /* No arguments or result are required. */
    li a7, USER_SYSCALL_YIELD
    ecall
    ret
.size rose_yield, . - rose_yield

.globl rose_syscall
.type rose_syscall, @function
rose_syscall:
    /* C passes (number, argument0, argument1) in a0-a2. Move them into the
     * regular syscall register layout before entering the kernel. */
    mv a7, a0
    mv a0, a1
    mv a1, a2
    ecall
    ret
.size rose_syscall, . - rose_syscall

.section .note.GNU-stack,"",@progbits
