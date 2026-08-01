#ifndef VIRTIO_BLOCK_H
#define VIRTIO_BLOCK_H

#include <stdbool.h>

/* Find and initialize the first VirtIO-MMIO block device from the DTB. */
bool virtio_block_init(void);

#endif
