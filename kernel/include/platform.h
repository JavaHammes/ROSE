#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parse the firmware-provided DTB. Must run before memory and driver setup. */
void platform_init(const void *device_tree);

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

/* Enumerate physical regions which the allocator must permanently reserve. */
size_t platform_reserved_region_count(void);
bool platform_reserved_region(size_t index, uintptr_t *start, size_t *size);

#endif
