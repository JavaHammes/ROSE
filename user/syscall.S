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

.globl rose_fstat
.type rose_fstat, @function
rose_fstat:
    li a7, USER_SYSCALL_FSTAT
    ecall
    ret
.size rose_fstat, . - rose_fstat

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

.globl rose_chdir
.type rose_chdir, @function
rose_chdir:
    li a7, USER_SYSCALL_CHDIR
    ecall
    ret
.size rose_chdir, . - rose_chdir

.globl rose_getcwd
.type rose_getcwd, @function
rose_getcwd:
    li a7, USER_SYSCALL_GETCWD
    ecall
    ret
.size rose_getcwd, . - rose_getcwd

.globl rose_dup
.type rose_dup, @function
rose_dup:
    li a7, USER_SYSCALL_DUP
    ecall
    ret
.size rose_dup, . - rose_dup

.globl rose_dup2
.type rose_dup2, @function
rose_dup2:
    li a7, USER_SYSCALL_DUP2
    ecall
    ret
.size rose_dup2, . - rose_dup2

.globl rose_pipe
.type rose_pipe, @function
rose_pipe:
    /* a0 points at storage for the read and write descriptor pair. */
    li a7, USER_SYSCALL_PIPE
    ecall
    ret
.size rose_pipe, . - rose_pipe

.globl rose_execve
.type rose_execve, @function
rose_execve:
    /* a0-a2 contain path, argv, and envp. Success starts the new image. */
    li a7, USER_SYSCALL_EXECVE
    ecall
    ret
.size rose_execve, . - rose_execve

.globl rose_spawn
.type rose_spawn, @function
rose_spawn:
    /* a0-a2 contain path, argv, and envp for the new child image. */
    li a7, USER_SYSCALL_SPAWN
    ecall
    ret
.size rose_spawn, . - rose_spawn

.globl rose_getpid
.type rose_getpid, @function
rose_getpid:
    li a7, USER_SYSCALL_GETPID
    ecall
    ret
.size rose_getpid, . - rose_getpid

.globl rose_waitpid
.type rose_waitpid, @function
rose_waitpid:
    /* a0-a2 contain the child selector, status pointer, and options. */
    li a7, USER_SYSCALL_WAITPID
    ecall
    ret
.size rose_waitpid, . - rose_waitpid

.globl rose_brk
.type rose_brk, @function
rose_brk:
    /* a0 is the requested process break, or zero to query it. */
    li a7, USER_SYSCALL_BRK
    ecall
    ret
.size rose_brk, . - rose_brk

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
