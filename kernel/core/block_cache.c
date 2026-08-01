/* Small write-through cache for the 1 KiB blocks used by the root ext2. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_cache.h"

enum { BLOCK_CACHE_ENTRY_COUNT = 16 };

struct block_cache_entry {
        bool valid;
        uint32_t block;
        uint8_t bytes[BLOCK_CACHE_BLOCK_SIZE];
};

static struct block_device *cache_device;
static struct block_cache_entry entries[BLOCK_CACHE_ENTRY_COUNT];
static size_t replacement_index;

static void copy_bytes(void *destination, const void *source, size_t length) {
        uint8_t *output = destination;
        const uint8_t *input = source;

        for (size_t index = 0U; index < length; index++) {
                output[index] = input[index];
        }
}

static struct block_cache_entry *find_entry(uint32_t block) {
        for (size_t index = 0U; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
                if (entries[index].valid && entries[index].block == block) {
                        return &entries[index];
                }
        }

        return NULL;
}

static struct block_cache_entry *select_entry(void) {
        for (size_t index = 0U; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
                if (!entries[index].valid) {
                        return &entries[index];
                }
        }

        struct block_cache_entry *entry = &entries[replacement_index];
        replacement_index = (replacement_index + 1U) % BLOCK_CACHE_ENTRY_COUNT;
        return entry;
}

bool block_cache_init(struct block_device *device) {
        if (device == NULL ||
            device->sector_count < BLOCK_CACHE_BLOCK_SIZE /
                                       BLOCK_DEVICE_SECTOR_SIZE) {
                return false;
        }

        cache_device = device;
        replacement_index = 0U;
        for (size_t index = 0U; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
                entries[index].valid = false;
        }
        return true;
}

uint64_t block_cache_block_count(void) {
        if (cache_device == NULL) {
                return 0U;
        }
        return cache_device->sector_count /
               (BLOCK_CACHE_BLOCK_SIZE / BLOCK_DEVICE_SECTOR_SIZE);
}

bool block_cache_read(uint32_t block, void *buffer) {
        if (cache_device == NULL || buffer == NULL ||
            block >= block_cache_block_count()) {
                return false;
        }

        struct block_cache_entry *entry = find_entry(block);

        if (entry == NULL) {
                entry = select_entry();
                uint64_t first_sector =
                    (uint64_t)block * BLOCK_CACHE_BLOCK_SIZE /
                    BLOCK_DEVICE_SECTOR_SIZE;

                if (!block_device_read(cache_device, first_sector,
                                       entry->bytes) ||
                    !block_device_read(cache_device, first_sector + 1U,
                                       &entry->bytes[BLOCK_DEVICE_SECTOR_SIZE])) {
                        entry->valid = false;
                        return false;
                }
                entry->block = block;
                entry->valid = true;
        }

        copy_bytes(buffer, entry->bytes, BLOCK_CACHE_BLOCK_SIZE);
        return true;
}

bool block_cache_write(uint32_t block, const void *buffer) {
        if (cache_device == NULL || buffer == NULL ||
            block >= block_cache_block_count()) {
                return false;
        }

        uint64_t first_sector = (uint64_t)block * BLOCK_CACHE_BLOCK_SIZE /
                                BLOCK_DEVICE_SECTOR_SIZE;

        if (!block_device_write(cache_device, first_sector, buffer) ||
            !block_device_write(
                cache_device, first_sector + 1U,
                (const uint8_t *)buffer + BLOCK_DEVICE_SECTOR_SIZE)) {
                return false;
        }

        struct block_cache_entry *entry = find_entry(block);
        if (entry == NULL) {
                entry = select_entry();
        }
        copy_bytes(entry->bytes, buffer, BLOCK_CACHE_BLOCK_SIZE);
        entry->block = block;
        entry->valid = true;
        return true;
}

bool block_cache_read_bytes(uint64_t offset, void *buffer, size_t length) {
        static uint8_t scratch[BLOCK_CACHE_BLOCK_SIZE];
        uint8_t *output = buffer;

        if (buffer == NULL && length != 0U) {
                return false;
        }

        while (length != 0U) {
                uint64_t block64 = offset / BLOCK_CACHE_BLOCK_SIZE;
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                size_t count = BLOCK_CACHE_BLOCK_SIZE - within;
                if (count > length) {
                        count = length;
                }
                if (block64 > UINT32_MAX ||
                    !block_cache_read((uint32_t)block64, scratch)) {
                        return false;
                }
                copy_bytes(output, &scratch[within], count);
                output += count;
                offset += count;
                length -= count;
        }

        return true;
}

bool block_cache_write_bytes(uint64_t offset, const void *buffer,
                             size_t length) {
        static uint8_t scratch[BLOCK_CACHE_BLOCK_SIZE];
        const uint8_t *input = buffer;

        if (buffer == NULL && length != 0U) {
                return false;
        }

        while (length != 0U) {
                uint64_t block64 = offset / BLOCK_CACHE_BLOCK_SIZE;
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                size_t count = BLOCK_CACHE_BLOCK_SIZE - within;
                if (count > length) {
                        count = length;
                }
                if (block64 > UINT32_MAX ||
                    !block_cache_read((uint32_t)block64, scratch)) {
                        return false;
                }
                copy_bytes(&scratch[within], input, count);
                if (!block_cache_write((uint32_t)block64, scratch)) {
                        return false;
                }
                input += count;
                offset += count;
                length -= count;
        }

        return true;
}
