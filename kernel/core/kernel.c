#include "block_device.h"
#include "ext2.h"
#include "graphics_console.h"
#include "interrupt.h"
#include "page_allocator.h"
#include "panic.h"
#include "platform.h"
#include "plic.h"
#include "ramfs.h"
#include "sbi.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"
#include "user_process.h"
#include "vfs.h"
#include "virtio_block.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "virtual_memory.h"

void kernel_main(unsigned long hart_id, const void *dtb) {
        /* The current kernel boots only the hart selected by OpenSBI. The ID is
         * retained in the interface for future multi-hart initialization. */
        (void)hart_id;

        /* Platform discovery precedes every subsystem which consumes physical
         * addresses or the timer frequency. */
        platform_init(dtb);
        trap_init();

        /* Physical allocation must exist before Sv39 can allocate table pages.
         */
        page_allocator_init();
        page_allocator_self_test();
        virtual_memory_init();
        ramfs_init();
        if (virtio_block_init()) {
                (void)ext2_mount(block_device_primary());
        }
        if (virtio_gpu_init()) {
                uart_puts("ROSE graphics: ");
                uart_put_uint64(virtio_gpu_width());
                uart_putc('x');
                uart_put_uint64(virtio_gpu_height());
                uart_putc('\n');
                graphics_console_init();
        }
        if (virtio_input_init()) {
                uart_puts("ROSE input: VirtIO keyboard/tablet online\n");
        }

        /* Configure the device-to-PLIC path before globally enabling traps. */
        if (!plic_register_handler(platform_uart_interrupt(),
                                   uart_handle_interrupt)) {
                panic("Could not register UART interrupt");
        }
        plic_init();
        uart_interrupts_enable();

        timer_schedule_next();

        timer_interrupts_enable();
        external_interrupts_enable();
        global_interrupts_enable();

        size_t idle_page_count = page_used_count();

        /* A disk root enters its long-lived init image, which starts /bin/sh
         * as a child and waits for it. The embedded ramfs starts the same shell
         * directly as a diagnostic fallback when no valid disk mounts. */
        if (vfs_uses_disk_root()) {
                user_process_run_path("/sbin/init", NULL);
        } else {
                user_process_run_path("/bin/sh", NULL);
        }
        (void)user_process_reap_exited();
        if (page_used_count() != idle_page_count) {
                uart_puts("Idle pages: ");
                uart_put_uint64(idle_page_count);
                uart_puts("; current pages: ");
                uart_put_uint64(page_used_count());
                uart_putc('\n');
                panic("User process page leak after userspace exit");
        }

        /* Returning from init after it reaps the shell is the disk-root
         * shutdown request; the fallback shell returns here directly. Keep a
         * stopped machine quiescent if firmware rejects the request. */
        if (sbi_shutdown() != 0) {
                uart_puts("SBI shutdown request failed\n");
        }

        while (1) {
                __asm__ volatile("wfi");
        }
}
#include "block_device.h"
#include "ext2.h"
