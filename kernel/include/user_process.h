#ifndef USER_PROCESS_H
#define USER_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trap_frame;

/* Convenience foreground demonstrations exposed by the kernel shell. */
void user_process_run(void);
void user_process_run_fault_test(void);
void user_process_run_multi(void);

/* Persistent process-table lifecycle operations used by spawn/wait/kill/reap. */
bool user_process_spawn(uint64_t program, uint64_t *pid);
bool user_process_run_ready(void);
bool user_process_kill(uint64_t pid);
size_t user_process_reap_exited(void);
void user_process_print_table(void);
bool user_process_is_active(void);

/* Entry points called only from the high-level trap dispatcher. */
void user_process_handle_timer(struct trap_frame *frame);
void user_process_handle_syscall(struct trap_frame *frame);
void user_process_handle_fault(struct trap_frame *frame, uint64_t cause);
void user_process_prepare_user_return(struct trap_frame *frame);

#endif
