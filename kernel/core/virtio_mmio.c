#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "virtio_mmio.h"

enum {
        VIRTIO_MAGIC = 0x74726976,
        VIRTIO_VERSION_MODERN = 2,
        VIRTIO_STATUS_ACKNOWLEDGE = 1,
        VIRTIO_STATUS_DRIVER = 2,
        VIRTIO_STATUS_DRIVER_OK = 4,
        VIRTIO_STATUS_FEATURES_OK = 8,
        VIRTIO_STATUS_FAILED = 128,
        VIRTIO_FEATURE_VERSION_1 = 32,
        VIRTIO_POLL_LIMIT = 10000000,
};

enum virtio_mmio_register {
        VIRTIO_MMIO_MAGIC = 0x000,
        VIRTIO_MMIO_VERSION = 0x004,
        VIRTIO_MMIO_DEVICE_ID = 0x008,
        VIRTIO_MMIO_DEVICE_FEATURES = 0x010,
        VIRTIO_MMIO_DEVICE_FEATURES_SELECT = 0x014,
        VIRTIO_MMIO_DRIVER_FEATURES = 0x020,
        VIRTIO_MMIO_DRIVER_FEATURES_SELECT = 0x024,
        VIRTIO_MMIO_QUEUE_SELECT = 0x030,
        VIRTIO_MMIO_QUEUE_SIZE_MAX = 0x034,
        VIRTIO_MMIO_QUEUE_SIZE = 0x038,
        VIRTIO_MMIO_QUEUE_READY = 0x044,
        VIRTIO_MMIO_QUEUE_NOTIFY = 0x050,
        VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
        VIRTIO_MMIO_INTERRUPT_ACK = 0x064,
        VIRTIO_MMIO_STATUS = 0x070,
        VIRTIO_MMIO_QUEUE_DESCRIPTOR_LOW = 0x080,
        VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
        VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
        VIRTIO_MMIO_CONFIG = 0x100,
};

static volatile uint32_t *virtio_register(uintptr_t base, size_t offset) {
        return (volatile uint32_t *)(base + offset);
}

static uint32_t virtio_read(uintptr_t base, size_t offset) {
        return *virtio_register(base, offset);
}

static void virtio_write(uintptr_t base, size_t offset, uint32_t value) {
        *virtio_register(base, offset) = value;
}

static void zero_bytes(void *pointer, size_t size) {
        uint8_t *bytes = pointer;

        for (size_t index = 0U; index < size; index++) {
                bytes[index] = 0U;
        }
}

static void write_address(uintptr_t base, size_t low_register,
                          uintptr_t address) {
        virtio_write(base, low_register, (uint32_t)address);
        virtio_write(base, low_register + sizeof(uint32_t),
                     (uint32_t)((uint64_t)address >> 32U));
}

static bool find_device(uint32_t device_id, size_t instance,
                        struct virtio_mmio_device *device) {
        size_t matched = 0U;

        for (size_t index = 0U; index < platform_virtio_count(); index++) {
                uintptr_t base;
                size_t size;
                uint32_t interrupt;

                if (!platform_virtio_device(index, &base, &size, &interrupt) ||
                    size < VIRTIO_MMIO_CONFIG + sizeof(uint32_t) ||
                    virtio_read(base, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC ||
                    virtio_read(base, VIRTIO_MMIO_VERSION) !=
                        VIRTIO_VERSION_MODERN ||
                    virtio_read(base, VIRTIO_MMIO_DEVICE_ID) != device_id) {
                        continue;
                }

                if (matched++ != instance) {
                        continue;
                }

                *device = (struct virtio_mmio_device){
                    .base = base,
                    .interrupt = interrupt,
                    .device_id = device_id,
                    .status = 0U,
                };
                return true;
        }

        return false;
}

bool virtio_mmio_begin(struct virtio_mmio_device *device, uint32_t device_id,
                       size_t instance, uint64_t supported_features,
                       uint64_t required_features) {
        if (device == NULL || !find_device(device_id, instance, device)) {
                return false;
        }

        required_features |= UINT64_C(1) << VIRTIO_FEATURE_VERSION_1;
        supported_features |= UINT64_C(1) << VIRTIO_FEATURE_VERSION_1;

        virtio_write(device->base, VIRTIO_MMIO_STATUS, 0U);
        device->status = VIRTIO_STATUS_ACKNOWLEDGE;
        virtio_write(device->base, VIRTIO_MMIO_STATUS, device->status);
        device->status |= VIRTIO_STATUS_DRIVER;
        virtio_write(device->base, VIRTIO_MMIO_STATUS, device->status);

        uint64_t offered = 0U;
        for (uint32_t page = 0U; page < 2U; page++) {
                virtio_write(device->base, VIRTIO_MMIO_DEVICE_FEATURES_SELECT,
                             page);
                offered |= (uint64_t)virtio_read(device->base,
                                                 VIRTIO_MMIO_DEVICE_FEATURES)
                           << (page * 32U);
        }
        if ((offered & required_features) != required_features) {
                virtio_mmio_fail(device);
                return false;
        }

        uint64_t accepted = offered & supported_features;
        for (uint32_t page = 0U; page < 2U; page++) {
                virtio_write(device->base, VIRTIO_MMIO_DRIVER_FEATURES_SELECT,
                             page);
                virtio_write(device->base, VIRTIO_MMIO_DRIVER_FEATURES,
                             (uint32_t)(accepted >> (page * 32U)));
        }

        device->status |= VIRTIO_STATUS_FEATURES_OK;
        virtio_write(device->base, VIRTIO_MMIO_STATUS, device->status);
        if ((virtio_read(device->base, VIRTIO_MMIO_STATUS) &
             VIRTIO_STATUS_FEATURES_OK) == 0U) {
                virtio_mmio_fail(device);
                return false;
        }

        return true;
}

bool virtio_mmio_queue_init(struct virtio_mmio_device *device,
                            struct virtio_queue *queue, uint16_t queue_index,
                            uint16_t queue_size) {
        if (device == NULL || device->base == 0U || queue == NULL ||
            queue_size == 0U || queue_size > VIRTIO_MMIO_QUEUE_CAPACITY) {
                return false;
        }

        virtio_write(device->base, VIRTIO_MMIO_QUEUE_SELECT, queue_index);
        if (virtio_read(device->base, VIRTIO_MMIO_QUEUE_READY) != 0U ||
            virtio_read(device->base, VIRTIO_MMIO_QUEUE_SIZE_MAX) <
                queue_size) {
                return false;
        }

        zero_bytes(queue, sizeof(*queue));
        queue->size = queue_size;
        queue->queue_index = queue_index;
        virtio_write(device->base, VIRTIO_MMIO_QUEUE_SIZE, queue_size);
        write_address(device->base, VIRTIO_MMIO_QUEUE_DESCRIPTOR_LOW,
                      (uintptr_t)queue->descriptors);
        write_address(device->base, VIRTIO_MMIO_QUEUE_DRIVER_LOW,
                      (uintptr_t)&queue->available);
        write_address(device->base, VIRTIO_MMIO_QUEUE_DEVICE_LOW,
                      (uintptr_t)&queue->used);
        virtio_write(device->base, VIRTIO_MMIO_QUEUE_READY, 1U);
        return true;
}

void virtio_mmio_finish(struct virtio_mmio_device *device) {
        if (device == NULL || device->base == 0U) {
                return;
        }

        device->status |= VIRTIO_STATUS_DRIVER_OK;
        virtio_write(device->base, VIRTIO_MMIO_STATUS, device->status);
}

void virtio_mmio_fail(struct virtio_mmio_device *device) {
        if (device == NULL || device->base == 0U) {
                return;
        }

        device->status |= VIRTIO_STATUS_FAILED;
        virtio_write(device->base, VIRTIO_MMIO_STATUS, device->status);
}

uint32_t virtio_mmio_config_read32(const struct virtio_mmio_device *device,
                                   size_t offset) {
        return virtio_read(device->base, VIRTIO_MMIO_CONFIG + offset);
}

uint8_t virtio_mmio_config_read8(const struct virtio_mmio_device *device,
                                 size_t offset) {
        return *(volatile uint8_t *)(device->base + VIRTIO_MMIO_CONFIG +
                                     offset);
}

void virtio_mmio_config_write8(const struct virtio_mmio_device *device,
                               size_t offset, uint8_t value) {
        *(volatile uint8_t *)(device->base + VIRTIO_MMIO_CONFIG + offset) =
            value;
}

uint32_t virtio_mmio_ack_interrupt(const struct virtio_mmio_device *device) {
        uint32_t status =
            virtio_read(device->base, VIRTIO_MMIO_INTERRUPT_STATUS);

        if (status != 0U) {
                virtio_write(device->base, VIRTIO_MMIO_INTERRUPT_ACK, status);
        }
        return status;
}

void virtio_queue_set_descriptor(struct virtio_queue *queue, uint16_t index,
                                 uintptr_t address, uint32_t length,
                                 uint16_t flags, uint16_t next) {
        if (queue == NULL || index >= queue->size) {
                return;
        }

        queue->descriptors[index] = (struct virtio_descriptor){
            .address = address,
            .length = length,
            .flags = flags,
            .next = next,
        };
}

void virtio_queue_submit(const struct virtio_mmio_device *device,
                         struct virtio_queue *queue, uint16_t head) {
        uint16_t available_index = queue->available.index;

        queue->available.ring[available_index % queue->size] = head;
        __asm__ volatile("fence rw, rw" : : : "memory");
        queue->available.index = (uint16_t)(available_index + 1U);
        __asm__ volatile("fence rw, rw" : : : "memory");
        virtio_write(device->base, VIRTIO_MMIO_QUEUE_NOTIFY,
                     queue->queue_index);
}

bool virtio_queue_pop_used(struct virtio_queue *queue, uint32_t *identifier,
                           uint32_t *length) {
        __asm__ volatile("fence r, r" : : : "memory");
        if (queue->used.index == queue->last_used_index) {
                return false;
        }

        const struct virtio_used_element *element =
            &queue->used.ring[queue->last_used_index % queue->size];
        if (identifier != NULL) {
                *identifier = element->identifier;
        }
        if (length != NULL) {
                *length = element->length;
        }
        queue->last_used_index++;
        return true;
}

bool virtio_queue_wait_used(const struct virtio_mmio_device *device,
                            struct virtio_queue *queue, uint16_t expected_head,
                            uint32_t *length) {
        for (size_t polls = 0U; polls < VIRTIO_POLL_LIMIT; polls++) {
                uint32_t identifier;
                uint32_t used_length;

                if (!virtio_queue_pop_used(queue, &identifier, &used_length)) {
                        continue;
                }

                (void)virtio_mmio_ack_interrupt(device);
                if (identifier != expected_head) {
                        return false;
                }
                if (length != NULL) {
                        *length = used_length;
                }
                return true;
        }

        return false;
}
