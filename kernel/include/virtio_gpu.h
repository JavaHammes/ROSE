#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
        VIRTIO_GPU_MAX_WIDTH = 1024,
        VIRTIO_GPU_MAX_HEIGHT = 768,
};

bool virtio_gpu_init(void);
bool virtio_gpu_available(void);
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);
uint32_t virtio_gpu_stride(void);
uint32_t *virtio_gpu_framebuffer(void);
size_t virtio_gpu_framebuffer_size(void);
bool virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

#endif
