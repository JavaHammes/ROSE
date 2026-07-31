#include "timer.h"
#include "platform.h"
#include "sbi.h"
#include "uart.h"
#include <stdint.h>

enum { TIMER_INTERRUPTS_PER_SECOND = 1000 };

/*
 * Read the current value of the RISC-V time counter.
 * Returns current absolute timer value.
 */
uint64_t read_time(void) {
        uint64_t value;

        __asm__ volatile("rdtime %0" : "=r"(value));

        return value;
}

/*
 * Program the next timer interrupt.
 * After one interrupt occurs we must set another deadline if we want
 * periodic timer interrupts.
 */
void timer_schedule_next(void) {
        /* Derive a one-millisecond quantum from the DTB timebase rather than
         * assuming QEMU's usual 10 MHz counter. SBI expects an absolute
         * deadline, not a relative interval. */
        uint64_t interval =
            platform_timebase_frequency() / TIMER_INTERRUPTS_PER_SECOND;

        if (interval == 0U) {
                interval = 1U;
        }

        uint64_t deadline = read_time() + interval;
        long error = sbi_set_timer(deadline);

        if (error != 0) {
                uart_puts("Failed to schedule timer\n");
        }
}
