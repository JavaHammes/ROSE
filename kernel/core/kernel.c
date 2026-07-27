#include "interrupt.h"
#include "kernel.h"
#include "plic.h"
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

        global_interrupts_enable();
        external_interrupts_enable();
        timer_interrupts_enable();

        //uart_puts("Kernel initialized\n");

        while (1) {
                __asm__ volatile("wfi");
        }
}
