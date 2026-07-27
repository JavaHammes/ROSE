#include "interrupt.h"
#include "plic.h"
#include "terminal.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"

void kernel_main(unsigned long hart_id, const void *dtb) {
        (void)hart_id;
        (void)dtb;

        trap_init();

        plic_init();
        uart_interrupts_enable();

        timer_schedule_next();

        external_interrupts_enable();
        global_interrupts_enable();

        terminal_init();

        while (1) {
                terminal_poll();

                /*
                 * Sleep until the next interrupt.
                 *
                 * A UART interrupt will place characters in the receive
                 * buffer. After returning from the trap handler, execution
                 * resumes here and terminal_poll() processes them.
                 */
                __asm__ volatile("wfi");
        }
}
