#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BLOCK_DEVICE_SECTOR_SIZE = 512 };

struct block_device;

struct block_device_operations {
        bool (*read_sector)(struct block_device *device, uint64_t sector,
                            void *buffer);
        bool (*write_sector)(struct block_device *device, uint64_t sector,
                             const void *buffer);
};

struct block_device {
        uint64_t sector_count;
        uintptr_t transport_base;
        const struct block_device_operations *operations;
};

bool block_device_register(struct block_device *device);
struct block_device *block_device_primary(void);
bool block_device_read(struct block_device *device, uint64_t sector,
                       void *buffer);
bool block_device_write(struct block_device *device, uint64_t sector,
                        const void *buffer);

#endif
