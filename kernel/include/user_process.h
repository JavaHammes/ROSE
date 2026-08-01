#ifndef USER_PROCESS_H
#define USER_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trap_frame;

/* The kernel copies these values onto the new process's guarded user stack. */
struct user_process_startup {
        size_t argument_count;
        const char *const *arguments;
        size_t environment_count;
        const char *const *environment;
};

/* Kernel boot entry for a userspace executable. */
void user_process_run_path(const char *path,
                           const struct user_process_startup *startup);

/* Reap kernel-owned zombies after the final userspace process exits. */
size_t user_process_reap_exited(void);
bool user_process_is_active(void);

/* Entry points called only from the high-level trap dispatcher. */
void user_process_handle_timer(struct trap_frame *frame);
void user_process_handle_syscall(struct trap_frame *frame);
void user_process_handle_fault(struct trap_frame *frame, uint64_t cause);
void user_process_prepare_user_return(struct trap_frame *frame);

/* Consume terminal control characters and signal the foreground group. */
bool user_process_handle_console_control(char character);

#endif
