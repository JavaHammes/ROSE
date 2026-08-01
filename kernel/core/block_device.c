#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

static struct block_device *primary_device;

bool block_device_register(struct block_device *device) {
        if (device == NULL || device->sector_count == 0U ||
            device->operations == NULL ||
            device->operations->read_sector == NULL ||
            device->operations->write_sector == NULL ||
            primary_device != NULL) {
                return false;
        }

        primary_device = device;
        return true;
}

struct block_device *block_device_primary(void) { return primary_device; }

bool block_device_read(struct block_device *device, uint64_t sector,
                       void *buffer) {
        return device != NULL && buffer != NULL && sector < device->sector_count &&
               device->operations != NULL &&
               device->operations->read_sector(device, sector, buffer);
}

bool block_device_write(struct block_device *device, uint64_t sector,
                        const void *buffer) {
        return device != NULL && buffer != NULL && sector < device->sector_count &&
               device->operations != NULL &&
               device->operations->write_sector(device, sector, buffer);
}
