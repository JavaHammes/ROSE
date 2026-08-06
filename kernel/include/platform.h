#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parse the firmware-provided DTB. Must run before memory and driver setup. */
void platform_init(const void *device_tree);

/* Firmware supplies kernel command-line options through /chosen/bootargs. */
const char *platform_boot_arguments(void);

/* Discovered RAM is represented as one half-open physical range. */
uintptr_t platform_ram_start(void);
uintptr_t platform_ram_end(void);

/* Timer frequency is expressed in rdtime counter ticks per second. */
uint64_t platform_timebase_frequency(void);

/* MMIO locations and the PLIC interrupt source used by the serial driver. */
uintptr_t platform_uart_base(void);
uint32_t platform_uart_interrupt(void);
uintptr_t platform_plic_base(void);
size_t platform_plic_size(void);

/* QEMU exposes each VirtIO-MMIO transport as a separate DTB node. */
size_t platform_virtio_count(void);
bool platform_virtio_device(size_t index, uintptr_t *base, size_t *size,
                            uint32_t *interrupt);

/* Enumerate physical regions which the allocator must permanently reserve. */
size_t platform_reserved_region_count(void);
bool platform_reserved_region(size_t index, uintptr_t *start, size_t *size);

#endif
