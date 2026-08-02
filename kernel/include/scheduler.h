#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trap_frame;

/* A small typed wait-channel namespace keeps drivers independent of processes. */
enum scheduler_wait_channel {
        SCHEDULER_WAIT_NONE,
        SCHEDULER_WAIT_UART_TX,
        SCHEDULER_WAIT_UART_RX,
        SCHEDULER_WAIT_CHILD,
        SCHEDULER_WAIT_PIPE_READ,
        SCHEDULER_WAIT_PIPE_WRITE,
        SCHEDULER_WAIT_TIMER,
        SCHEDULER_WAIT_EVENT,
};

/* Block the current U-mode process and wake sleepers from an interrupt. */
void scheduler_block_current(struct trap_frame *frame,
                             enum scheduler_wait_channel channel);
void scheduler_block_current_until(struct trap_frame *frame,
                                   enum scheduler_wait_channel channel,
                                   uint64_t deadline_nanoseconds);
size_t scheduler_wake_all(enum scheduler_wait_channel channel);
bool scheduler_wake_one(enum scheduler_wait_channel channel);
size_t scheduler_wake_expired(uint64_t now_nanoseconds);

#endif
