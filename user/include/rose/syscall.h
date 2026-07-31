#ifndef ROSE_USER_SYSCALL_H
#define ROSE_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* Descriptor I/O and path lookup. Failures are returned as negative errors. */
long rose_read(int descriptor, void *buffer, size_t length);
long rose_write(int descriptor, const void *buffer, size_t length);
long rose_open(const char *path);
long rose_close(int descriptor);

/* Terminate the calling process or voluntarily give another process a turn. */
_Noreturn void rose_exit(uint64_t status);
void rose_yield(void);

/* Raw entry used by the ABI validation program to issue an unknown syscall. */
long rose_syscall(uint64_t number, uintptr_t argument0, uintptr_t argument1,
                  uintptr_t argument2);

#endif
