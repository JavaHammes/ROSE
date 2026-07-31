#ifndef ROSE_USER_SYSCALL_H
#define ROSE_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* Write bytes to the kernel console. Failures are returned as negative errors. */
long rose_write(const void *buffer, size_t length);

/* Terminate the calling process or voluntarily give another process a turn. */
_Noreturn void rose_exit(uint64_t status);
void rose_yield(void);

/* Raw entry used by the ABI validation program to issue an unknown syscall. */
long rose_syscall(uint64_t number, uintptr_t argument0, uintptr_t argument1);

#endif
