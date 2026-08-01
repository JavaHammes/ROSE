#ifndef ROSE_USER_SYSCALL_H
#define ROSE_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include "user_abi.h"

/* Descriptor I/O and path lookup. Failures are returned as negative errors. */
long rose_read(int descriptor, void *buffer, size_t length);
long rose_write(int descriptor, const void *buffer, size_t length);
long rose_open(const char *path, uint32_t flags);
long rose_close(int descriptor);
long rose_stat(const char *path, struct user_file_status *status);
long rose_lseek(int descriptor, int64_t offset, uint32_t whence);
long rose_read_directory(int descriptor, struct user_directory_entry *entry);
long rose_mkdir(const char *path);
long rose_unlink(const char *path);

/* Replace the calling process image. Success does not return. */
long rose_execve(const char *path, char *const arguments[],
                 char *const environment[]);

/* Create a child process and wait for child state changes. */
long rose_spawn(const char *path, char *const arguments[],
                char *const environment[]);
long rose_getpid(void);
long rose_waitpid(int64_t pid, int *status, uint32_t options);

/* Terminate the calling process or voluntarily give another process a turn. */
_Noreturn void rose_exit(uint64_t status);
void rose_yield(void);

/* Raw entry used by the ABI validation program to issue an unknown syscall. */
long rose_syscall(uint64_t number, uintptr_t argument0, uintptr_t argument1,
                  uintptr_t argument2);

#endif
