#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>

struct trap_frame;

/* A small typed wait-channel namespace keeps drivers independent of processes. */
enum scheduler_wait_channel {
        SCHEDULER_WAIT_NONE,
        SCHEDULER_WAIT_UART_TX,
        SCHEDULER_WAIT_UART_RX,
        SCHEDULER_WAIT_CHILD,
};

/* Block the current U-mode process and wake sleepers from an interrupt. */
void scheduler_block_current(struct trap_frame *frame,
                             enum scheduler_wait_channel channel);
size_t scheduler_wake_all(enum scheduler_wait_channel channel);
bool scheduler_wake_one(enum scheduler_wait_channel channel);

#endif
