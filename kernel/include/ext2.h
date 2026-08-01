#ifndef EXT2_H
#define EXT2_H

#include <stdbool.h>

struct block_device;

/* Mount a one-group, 1 KiB-block ext2 filesystem as the VFS root. */
bool ext2_mount(struct block_device *device);

#endif
