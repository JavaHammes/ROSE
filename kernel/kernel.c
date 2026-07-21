#include "uart.h"


void kernel_main(unsigned long hart_id, const void *dtb) {
    	(void)hart_id;
    	(void)dtb;

		uart_puts("Hello World!\n");	

		while (1) {
			asm volatile("wfi");
		}
}
