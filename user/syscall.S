/*
 * Thin RISC-V syscall stubs for the freestanding user runtime.
 *
 * a0-a5 carry arguments, a7 selects the syscall, and the kernel returns its
 * result in a0. The kernel advances sepc past ecall before returning here.
 * Each stub has its own section so --gc-sections retains only the calls used
 * by a given small executable.
 */
#include "user_abi.h"

.section .text.rose_write,"ax",@progbits
.align 2
.globl rose_write
.type rose_write, @function
rose_write:
    /* Arguments arrive in a0-a2 as descriptor, buffer, and length. */
    li a7, USER_SYSCALL_WRITE
    ecall
    ret
.size rose_write, . - rose_write

.section .text.rose_read,"ax",@progbits
.align 2
.globl rose_read
.type rose_read, @function
rose_read:
    /* Arguments arrive in a0-a2 as descriptor, buffer, and length. */
    li a7, USER_SYSCALL_READ
    ecall
    ret
.size rose_read, . - rose_read

.section .text.rose_open,"ax",@progbits
.align 2
.globl rose_open
.type rose_open, @function
rose_open:
    /* a0 points at a null-terminated absolute path. */
    li a7, USER_SYSCALL_OPEN
    ecall
    ret
.size rose_open, . - rose_open

.section .text.rose_close,"ax",@progbits
.align 2
.globl rose_close
.type rose_close, @function
rose_close:
    /* a0 contains the descriptor to release. */
    li a7, USER_SYSCALL_CLOSE
    ecall
    ret
.size rose_close, . - rose_close

.section .text.rose_stat,"ax",@progbits
.align 2
.globl rose_stat
.type rose_stat, @function
rose_stat:
    li a7, USER_SYSCALL_STAT
    ecall
    ret
.size rose_stat, . - rose_stat

.section .text.rose_fstat,"ax",@progbits
.align 2
.globl rose_fstat
.type rose_fstat, @function
rose_fstat:
    li a7, USER_SYSCALL_FSTAT
    ecall
    ret
.size rose_fstat, . - rose_fstat

.section .text.rose_lseek,"ax",@progbits
.align 2
.globl rose_lseek
.type rose_lseek, @function
rose_lseek:
    li a7, USER_SYSCALL_LSEEK
    ecall
    ret
.size rose_lseek, . - rose_lseek

.section .text.rose_read_directory,"ax",@progbits
.align 2
.globl rose_read_directory
.type rose_read_directory, @function
rose_read_directory:
    li a7, USER_SYSCALL_READ_DIRECTORY
    ecall
    ret
.size rose_read_directory, . - rose_read_directory

.section .text.rose_mkdir,"ax",@progbits
.align 2
.globl rose_mkdir
.type rose_mkdir, @function
rose_mkdir:
    li a7, USER_SYSCALL_MKDIR
    ecall
    ret
.size rose_mkdir, . - rose_mkdir

.section .text.rose_unlink,"ax",@progbits
.align 2
.globl rose_unlink
.type rose_unlink, @function
rose_unlink:
    li a7, USER_SYSCALL_UNLINK
    ecall
    ret
.size rose_unlink, . - rose_unlink

.section .text.rose_chdir,"ax",@progbits
.align 2
.globl rose_chdir
.type rose_chdir, @function
rose_chdir:
    li a7, USER_SYSCALL_CHDIR
    ecall
    ret
.size rose_chdir, . - rose_chdir

.section .text.rose_getcwd,"ax",@progbits
.align 2
.globl rose_getcwd
.type rose_getcwd, @function
rose_getcwd:
    li a7, USER_SYSCALL_GETCWD
    ecall
    ret
.size rose_getcwd, . - rose_getcwd

.section .text.rose_dup,"ax",@progbits
.align 2
.globl rose_dup
.type rose_dup, @function
rose_dup:
    li a7, USER_SYSCALL_DUP
    ecall
    ret
.size rose_dup, . - rose_dup

.section .text.rose_dup2,"ax",@progbits
.align 2
.globl rose_dup2
.type rose_dup2, @function
rose_dup2:
    li a7, USER_SYSCALL_DUP2
    ecall
    ret
.size rose_dup2, . - rose_dup2

.section .text.rose_pipe,"ax",@progbits
.align 2
.globl rose_pipe
.type rose_pipe, @function
rose_pipe:
    /* a0 points at storage for the read and write descriptor pair. */
    li a7, USER_SYSCALL_PIPE
    ecall
    ret
.size rose_pipe, . - rose_pipe

.section .text.rose_set_descriptor_flags,"ax",@progbits
.align 2
.globl rose_set_descriptor_flags
.type rose_set_descriptor_flags, @function
rose_set_descriptor_flags:
    /* a0 is a descriptor and a1 is a user_descriptor_flags mask. */
    li a7, USER_SYSCALL_SET_DESCRIPTOR_FLAGS
    ecall
    ret
.size rose_set_descriptor_flags, . - rose_set_descriptor_flags

.section .text.rose_execve,"ax",@progbits
.align 2
.globl rose_execve
.type rose_execve, @function
rose_execve:
    /* a0-a2 contain path, argv, and envp. Success starts the new image. */
    li a7, USER_SYSCALL_EXECVE
    ecall
    ret
.size rose_execve, . - rose_execve

.section .text.rose_spawn,"ax",@progbits
.align 2
.globl rose_spawn
.type rose_spawn, @function
rose_spawn:
    /* a0-a2 contain path, argv, and envp for the new child image. */
    li a7, USER_SYSCALL_SPAWN
    ecall
    ret
.size rose_spawn, . - rose_spawn

.section .text.rose_fork,"ax",@progbits
.align 2
.globl rose_fork
.type rose_fork, @function
rose_fork:
    /* The child resumes after this ECALL with zero; the parent receives PID. */
    li a7, USER_SYSCALL_FORK
    ecall
    ret
.size rose_fork, . - rose_fork

.section .text.rose_getpid,"ax",@progbits
.align 2
.globl rose_getpid
.type rose_getpid, @function
rose_getpid:
    li a7, USER_SYSCALL_GETPID
    ecall
    ret
.size rose_getpid, . - rose_getpid

.section .text.rose_waitpid,"ax",@progbits
.align 2
.globl rose_waitpid
.type rose_waitpid, @function
rose_waitpid:
    /* a0-a2 contain the child selector, status pointer, and options. */
    li a7, USER_SYSCALL_WAITPID
    ecall
    ret
.size rose_waitpid, . - rose_waitpid

.section .text.rose_sigaction,"ax",@progbits
.align 2
.globl rose_sigaction
.type rose_sigaction, @function
rose_sigaction:
    /* a3 gives the kernel the trusted userspace return trampoline associated
     * with this runtime. It is retained only for a caught disposition. */
    la a3, rose_signal_trampoline
    li a7, USER_SYSCALL_SIGNAL_ACTION
    ecall
    ret
.size rose_sigaction, . - rose_sigaction

.section .text.rose_kill,"ax",@progbits
.align 2
.globl rose_kill
.type rose_kill, @function
rose_kill:
    li a7, USER_SYSCALL_KILL
    ecall
    ret
.size rose_kill, . - rose_kill

.section .text.rose_setpgid,"ax",@progbits
.align 2
.globl rose_setpgid
.type rose_setpgid, @function
rose_setpgid:
    li a7, USER_SYSCALL_SET_PROCESS_GROUP
    ecall
    ret
.size rose_setpgid, . - rose_setpgid

.section .text.rose_getpgrp,"ax",@progbits
.align 2
.globl rose_getpgrp
.type rose_getpgrp, @function
rose_getpgrp:
    li a7, USER_SYSCALL_GET_PROCESS_GROUP
    ecall
    ret
.size rose_getpgrp, . - rose_getpgrp

.section .text.rose_tcsetpgrp,"ax",@progbits
.align 2
.globl rose_tcsetpgrp
.type rose_tcsetpgrp, @function
rose_tcsetpgrp:
    li a7, USER_SYSCALL_TERMINAL_SET_FOREGROUND_GROUP
    ecall
    ret
.size rose_tcsetpgrp, . - rose_tcsetpgrp

.section .text.rose_tcgetpgrp,"ax",@progbits
.align 2
.globl rose_tcgetpgrp
.type rose_tcgetpgrp, @function
rose_tcgetpgrp:
    li a7, USER_SYSCALL_TERMINAL_GET_FOREGROUND_GROUP
    ecall
    ret
.size rose_tcgetpgrp, . - rose_tcgetpgrp

.section .text.rose_signal_trampoline,"ax",@progbits
.align 2
.type rose_signal_trampoline, @function
rose_signal_trampoline:
    /* A signal handler returns here through ra. sigreturn restores the entire
     * interrupted register frame and therefore never resumes this sequence. */
    li a7, USER_SYSCALL_SIGNAL_RETURN
    ecall
1:
    j 1b
.size rose_signal_trampoline, . - rose_signal_trampoline

.section .text.rose_brk,"ax",@progbits
.align 2
.globl rose_brk
.type rose_brk, @function
rose_brk:
    /* a0 is the requested process break, or zero to query it. */
    li a7, USER_SYSCALL_BRK
    ecall
    ret
.size rose_brk, . - rose_brk

.section .text.rose_graphics_map,"ax",@progbits
.align 2
.globl rose_graphics_map
.type rose_graphics_map, @function
rose_graphics_map:
    li a7, USER_SYSCALL_GRAPHICS_MAP
    ecall
    ret
.size rose_graphics_map, . - rose_graphics_map

.section .text.rose_graphics_flush,"ax",@progbits
.align 2
.globl rose_graphics_flush
.type rose_graphics_flush, @function
rose_graphics_flush:
    li a7, USER_SYSCALL_GRAPHICS_FLUSH
    ecall
    ret
.size rose_graphics_flush, . - rose_graphics_flush

.section .text.rose_input_read,"ax",@progbits
.align 2
.globl rose_input_read
.type rose_input_read, @function
rose_input_read:
    li a7, USER_SYSCALL_INPUT_READ
    ecall
    ret
.size rose_input_read, . - rose_input_read

.section .text.rose_shared_memory_create,"ax",@progbits
.align 2
.globl rose_shared_memory_create
.type rose_shared_memory_create, @function
rose_shared_memory_create:
    li a7, USER_SYSCALL_SHARED_MEMORY_CREATE
    ecall
    ret
.size rose_shared_memory_create, . - rose_shared_memory_create

.section .text.rose_shared_memory_map,"ax",@progbits
.align 2
.globl rose_shared_memory_map
.type rose_shared_memory_map, @function
rose_shared_memory_map:
    li a7, USER_SYSCALL_SHARED_MEMORY_MAP
    ecall
    ret
.size rose_shared_memory_map, . - rose_shared_memory_map

.section .text.rose_shared_memory_unmap,"ax",@progbits
.align 2
.globl rose_shared_memory_unmap
.type rose_shared_memory_unmap, @function
rose_shared_memory_unmap:
    li a7, USER_SYSCALL_SHARED_MEMORY_UNMAP
    ecall
    ret
.size rose_shared_memory_unmap, . - rose_shared_memory_unmap

.section .text.rose_openpty,"ax",@progbits
.align 2
.globl rose_openpty
.type rose_openpty, @function
rose_openpty:
    li a7, USER_SYSCALL_OPEN_PSEUDO_TERMINAL
    ecall
    ret
.size rose_openpty, . - rose_openpty

.section .text.rose_system_info,"ax",@progbits
.align 2
.globl rose_system_info
.type rose_system_info, @function
rose_system_info:
    li a7, USER_SYSCALL_SYSTEM_INFO
    ecall
    ret
.size rose_system_info, . - rose_system_info

.section .text.rose_mmap,"ax",@progbits
.align 2
.globl rose_mmap
.type rose_mmap, @function
rose_mmap:
    li a7, USER_SYSCALL_MMAP
    ecall
    ret
.size rose_mmap, . - rose_mmap

.section .text.rose_mprotect,"ax",@progbits
.align 2
.globl rose_mprotect
.type rose_mprotect, @function
rose_mprotect:
    li a7, USER_SYSCALL_MPROTECT
    ecall
    ret
.size rose_mprotect, . - rose_mprotect

.section .text.rose_munmap,"ax",@progbits
.align 2
.globl rose_munmap
.type rose_munmap, @function
rose_munmap:
    li a7, USER_SYSCALL_MUNMAP
    ecall
    ret
.size rose_munmap, . - rose_munmap

.section .text.rose_exit,"ax",@progbits
.align 2
.globl rose_exit
.type rose_exit, @function
rose_exit:
    /* a0 contains the status. A successful exit never resumes this stub. */
    li a7, USER_SYSCALL_EXIT
    ecall
1:
    j 1b
.size rose_exit, . - rose_exit

.section .text.rose_yield,"ax",@progbits
.align 2
.globl rose_yield
.type rose_yield, @function
rose_yield:
    /* No arguments or result are required. */
    li a7, USER_SYSCALL_YIELD
    ecall
    ret
.size rose_yield, . - rose_yield

.section .text.rose_syscall,"ax",@progbits
.align 2
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
