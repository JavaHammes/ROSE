
void kernel_main(unsigned long hart_id, const void *dtb) {

        (void)hart_id;
        (void)dtb;

        while (1) {
			asm volatile("wfi");
        }
}

