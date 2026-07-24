#include "timer.h"
#include "sbi.h"
#include "uart.h"
#include <stdint.h>

// Equals one second. Hardcoded for now.
#define TIMER_INTERVAL UINT64_C(10000000)

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
        uint64_t deadline = read_time() + TIMER_INTERVAL;
        long error = sbi_set_timer(deadline);

        if (error != 0) {
                uart_puts("Faield to schedule timer\n");
        }
}
