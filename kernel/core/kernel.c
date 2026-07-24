#include "kernel.h"
#include "trap.h"
#include "uart.h"

void kernel_main(unsigned long hart_id, const void *dtb) {
        (void)hart_id;
        (void)dtb;

        trap_init();

        __asm__ volatile(".word 0");

        while (1) {
                __asm__ volatile("nop");
        }
}
