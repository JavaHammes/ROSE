#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

enum { BLOCK_CACHE_BLOCK_SIZE = 1024 };

bool block_cache_init(struct block_device *device);
bool block_cache_read(uint32_t block, void *buffer);
bool block_cache_write(uint32_t block, const void *buffer);
bool block_cache_read_bytes(uint64_t offset, void *buffer, size_t length);
bool block_cache_write_bytes(uint64_t offset, const void *buffer,
                             size_t length);
uint64_t block_cache_block_count(void);

#endif
