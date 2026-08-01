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
long rose_fstat(int descriptor, struct user_file_status *status);
long rose_lseek(int descriptor, int64_t offset, uint32_t whence);
long rose_read_directory(int descriptor, struct user_directory_entry *entry);
long rose_mkdir(const char *path);
long rose_unlink(const char *path);
long rose_chdir(const char *path);
long rose_getcwd(char *buffer, size_t size);
long rose_dup(int descriptor);
long rose_dup2(int old_descriptor, int new_descriptor);
long rose_pipe(int descriptors[2]);
long rose_set_descriptor_flags(int descriptor, uint32_t flags);

/* Replace the calling process image. Success does not return. */
long rose_execve(const char *path, char *const arguments[],
                 char *const environment[]);

/* Clone the calling process, create a child image, and wait for children. */
long rose_fork(void);
long rose_spawn(const char *path, char *const arguments[],
                char *const environment[]);
long rose_getpid(void);
long rose_waitpid(int64_t pid, int *status, uint32_t options);

/* Install signal dispositions and direct a signal to one process. */
long rose_sigaction(int signal, const struct user_signal_action *action,
                    struct user_signal_action *old_action);
long rose_kill(int64_t pid, int signal);

/* Process groups and the foreground group attached to the console. */
long rose_setpgid(int64_t pid, int64_t process_group);
long rose_getpgrp(void);
long rose_tcsetpgrp(int descriptor, int64_t process_group);
long rose_tcgetpgrp(int descriptor);

/* Query or move the process break. Address zero queries without changing it;
 * success returns the resulting break and failure returns a negative error. */
long rose_brk(uintptr_t address);

/* Claim the 2D scanout, publish its mapped framebuffer, flush changed pixels,
 * and consume nonblocking keyboard/pointer events. */
long rose_graphics_map(struct user_graphics_info *information);
long rose_graphics_flush(uint32_t x, uint32_t y, uint32_t width,
                         uint32_t height);
long rose_input_read(struct user_input_event *event);

/* Terminate the calling process or voluntarily give another process a turn. */
_Noreturn void rose_exit(uint64_t status);
void rose_yield(void);

/* Raw entry used by the ABI validation program to issue an unknown syscall. */
long rose_syscall(uint64_t number, uintptr_t argument0, uintptr_t argument1,
                  uintptr_t argument2);

#endif
