#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_MMIO_QUEUE_CAPACITY 64U

enum virtio_device_id {
        VIRTIO_DEVICE_BLOCK = 2,
        VIRTIO_DEVICE_GPU = 16,
        VIRTIO_DEVICE_INPUT = 18,
};

enum virtio_descriptor_flag {
        VIRTIO_DESCRIPTOR_NEXT = 1,
        VIRTIO_DESCRIPTOR_WRITE = 2,
};

struct virtio_descriptor {
        uint64_t address;
        uint32_t length;
        uint16_t flags;
        uint16_t next;
};

struct virtio_available {
        uint16_t flags;
        uint16_t index;
        uint16_t ring[VIRTIO_MMIO_QUEUE_CAPACITY];
        uint16_t used_event;
};

struct virtio_used_element {
        uint32_t identifier;
        uint32_t length;
};

struct virtio_used {
        uint16_t flags;
        uint16_t index;
        struct virtio_used_element ring[VIRTIO_MMIO_QUEUE_CAPACITY];
        uint16_t available_event;
};

struct virtio_queue {
        struct virtio_descriptor descriptors[VIRTIO_MMIO_QUEUE_CAPACITY]
            __attribute__((aligned(16)));
        struct virtio_available available __attribute__((aligned(2)));
        struct virtio_used used __attribute__((aligned(4)));
        uint16_t size;
        uint16_t queue_index;
        uint16_t last_used_index;
};

struct virtio_mmio_device {
        uintptr_t base;
        uint32_t interrupt;
        uint32_t device_id;
        uint8_t status;
};

bool virtio_mmio_begin(struct virtio_mmio_device *device, uint32_t device_id,
                       size_t instance, uint64_t supported_features,
                       uint64_t required_features);
bool virtio_mmio_queue_init(struct virtio_mmio_device *device,
                            struct virtio_queue *queue, uint16_t queue_index,
                            uint16_t queue_size);
void virtio_mmio_finish(struct virtio_mmio_device *device);
void virtio_mmio_fail(struct virtio_mmio_device *device);

uint32_t virtio_mmio_config_read32(const struct virtio_mmio_device *device,
                                   size_t offset);
uint8_t virtio_mmio_config_read8(const struct virtio_mmio_device *device,
                                 size_t offset);
void virtio_mmio_config_write8(const struct virtio_mmio_device *device,
                               size_t offset, uint8_t value);
uint32_t virtio_mmio_ack_interrupt(const struct virtio_mmio_device *device);

void virtio_queue_set_descriptor(struct virtio_queue *queue, uint16_t index,
                                 uintptr_t address, uint32_t length,
                                 uint16_t flags, uint16_t next);
void virtio_queue_submit(const struct virtio_mmio_device *device,
                         struct virtio_queue *queue, uint16_t head);
bool virtio_queue_pop_used(struct virtio_queue *queue, uint32_t *identifier,
                           uint32_t *length);
bool virtio_queue_wait_used(const struct virtio_mmio_device *device,
                            struct virtio_queue *queue, uint16_t expected_head,
                            uint32_t *length);

#endif
