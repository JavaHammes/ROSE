#include "interrupt.h"
#include "page_allocator.h"
#include "platform.h"
#include "plic.h"
#include "terminal.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "virtual_memory.h"

void kernel_main(unsigned long hart_id, const void *dtb) {
        /* The current kernel boots only the hart selected by OpenSBI. The ID is
         * retained in the interface for future multi-hart initialization. */
        (void)hart_id;

        /* Platform discovery precedes every subsystem which consumes physical
         * addresses or the timer frequency. */
        platform_init(dtb);
        trap_init();

        /* Physical allocation must exist before Sv39 can allocate table pages. */
        page_allocator_init();
        page_allocator_self_test();
        virtual_memory_init();

        /* Configure the device-to-PLIC path before globally enabling traps. */
        plic_init();
        uart_interrupts_enable();

        timer_schedule_next();

        timer_interrupts_enable();
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
