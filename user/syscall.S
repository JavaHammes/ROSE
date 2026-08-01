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
    /* Arguments arrive in a0-a2 as descriptor, buffer, and length. */
    li a7, USER_SYSCALL_WRITE
    ecall
    ret
.size rose_write, . - rose_write

.globl rose_read
.type rose_read, @function
rose_read:
    /* Arguments arrive in a0-a2 as descriptor, buffer, and length. */
    li a7, USER_SYSCALL_READ
    ecall
    ret
.size rose_read, . - rose_read

.globl rose_open
.type rose_open, @function
rose_open:
    /* a0 points at a null-terminated absolute path. */
    li a7, USER_SYSCALL_OPEN
    ecall
    ret
.size rose_open, . - rose_open

.globl rose_close
.type rose_close, @function
rose_close:
    /* a0 contains the descriptor to release. */
    li a7, USER_SYSCALL_CLOSE
    ecall
    ret
.size rose_close, . - rose_close

.globl rose_stat
.type rose_stat, @function
rose_stat:
    li a7, USER_SYSCALL_STAT
    ecall
    ret
.size rose_stat, . - rose_stat

.globl rose_lseek
.type rose_lseek, @function
rose_lseek:
    li a7, USER_SYSCALL_LSEEK
    ecall
    ret
.size rose_lseek, . - rose_lseek

.globl rose_read_directory
.type rose_read_directory, @function
rose_read_directory:
    li a7, USER_SYSCALL_READ_DIRECTORY
    ecall
    ret
.size rose_read_directory, . - rose_read_directory

.globl rose_mkdir
.type rose_mkdir, @function
rose_mkdir:
    li a7, USER_SYSCALL_MKDIR
    ecall
    ret
.size rose_mkdir, . - rose_mkdir

.globl rose_unlink
.type rose_unlink, @function
rose_unlink:
    li a7, USER_SYSCALL_UNLINK
    ecall
    ret
.size rose_unlink, . - rose_unlink

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
    /* C passes (number, argument0, argument1, argument2) in a0-a3. Move into the
     * regular syscall register layout before entering the kernel. */
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    ecall
    ret
.size rose_syscall, . - rose_syscall

.section .note.GNU-stack,"",@progbits
