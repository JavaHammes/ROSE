#include "kernel.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"

void kernel_main(unsigned long hart_id, const void *dtb) {
        (void)hart_id;
        (void)dtb;

        uart_puts("Kernel initialized\n");

        trap_init();
        timer_schedule_next();
        interrupts_enable();

        while (1) {
                __asm__ volatile("wfi");
        }
}
