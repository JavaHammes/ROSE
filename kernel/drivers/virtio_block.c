/* Minimal modern VirtIO-MMIO block transport with one synchronous queue. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "platform.h"
#include "virtio_block.h"

enum {
        VIRTIO_MAGIC = 0x74726976,
        VIRTIO_DEVICE_BLOCK = 2,
        VIRTIO_VERSION_MODERN = 2,
        VIRTIO_STATUS_ACKNOWLEDGE = 1,
        VIRTIO_STATUS_DRIVER = 2,
        VIRTIO_STATUS_DRIVER_OK = 4,
        VIRTIO_STATUS_FEATURES_OK = 8,
        VIRTIO_STATUS_FAILED = 128,
        VIRTIO_QUEUE_SIZE = 8,
        VIRTIO_DESCRIPTOR_NEXT = 1,
        VIRTIO_DESCRIPTOR_WRITE = 2,
        VIRTIO_BLOCK_REQUEST_READ = 0,
        VIRTIO_BLOCK_REQUEST_WRITE = 1,
        VIRTIO_BLOCK_STATUS_OK = 0,
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
        VIRTIO_MMIO_QUEUE_DESCRIPTOR_HIGH = 0x084,
        VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
        VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
        VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
        VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0a4,
        VIRTIO_MMIO_CONFIG = 0x100,
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
        uint16_t ring[VIRTIO_QUEUE_SIZE];
        uint16_t used_event;
};

struct virtio_used_element {
        uint32_t identifier;
        uint32_t length;
};

struct virtio_used {
        uint16_t flags;
        uint16_t index;
        struct virtio_used_element ring[VIRTIO_QUEUE_SIZE];
        uint16_t available_event;
};

struct virtio_block_request {
        uint32_t type;
        uint32_t reserved;
        uint64_t sector;
};

static struct virtio_descriptor descriptors[VIRTIO_QUEUE_SIZE]
    __attribute__((aligned(16)));
static struct virtio_available available __attribute__((aligned(2)));
static struct virtio_used used __attribute__((aligned(4)));
static struct virtio_block_request request __attribute__((aligned(16)));
static volatile uint8_t request_status;
static struct block_device device;
static uintptr_t active_base;
static uint16_t last_used_index;

static volatile uint32_t *virtio_register(uintptr_t base, size_t offset) {
        return (volatile uint32_t *)(base + offset);
}

static uint32_t virtio_read(uintptr_t base, size_t offset) {
        return *virtio_register(base, offset);
}

static void virtio_write(uintptr_t base, size_t offset, uint32_t value) {
        *virtio_register(base, offset) = value;
}

static void set_queue_address(size_t low_register, uintptr_t address) {
        virtio_write(active_base, low_register, (uint32_t)address);
        virtio_write(active_base, low_register + sizeof(uint32_t),
                     (uint32_t)((uint64_t)address >> 32U));
}

static bool virtio_block_transfer(uint64_t sector, void *buffer, bool write) {
        request.type = write ? VIRTIO_BLOCK_REQUEST_WRITE
                             : VIRTIO_BLOCK_REQUEST_READ;
        request.reserved = 0U;
        request.sector = sector;
        request_status = UINT8_MAX;

        descriptors[0] = (struct virtio_descriptor){
            .address = (uintptr_t)&request,
            .length = sizeof(request),
            .flags = VIRTIO_DESCRIPTOR_NEXT,
            .next = 1U,
        };
        descriptors[1] = (struct virtio_descriptor){
            .address = (uintptr_t)buffer,
            .length = BLOCK_DEVICE_SECTOR_SIZE,
            .flags = VIRTIO_DESCRIPTOR_NEXT |
                     (write ? 0U : VIRTIO_DESCRIPTOR_WRITE),
            .next = 2U,
        };
        descriptors[2] = (struct virtio_descriptor){
            .address = (uintptr_t)&request_status,
            .length = sizeof(request_status),
            .flags = VIRTIO_DESCRIPTOR_WRITE,
            .next = 0U,
        };

        uint16_t available_index = available.index;
        available.ring[available_index % VIRTIO_QUEUE_SIZE] = 0U;
        __asm__ volatile("fence rw, rw" : : : "memory");
        available.index = (uint16_t)(available_index + 1U);
        __asm__ volatile("fence rw, rw" : : : "memory");
        virtio_write(active_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0U);

        for (size_t polls = 0U; polls < VIRTIO_POLL_LIMIT; polls++) {
                __asm__ volatile("fence r, r" : : : "memory");
                if (used.index != last_used_index) {
                        last_used_index = used.index;
                        uint32_t interrupts = virtio_read(
                            active_base, VIRTIO_MMIO_INTERRUPT_STATUS);
                        if (interrupts != 0U) {
                                virtio_write(active_base,
                                             VIRTIO_MMIO_INTERRUPT_ACK,
                                             interrupts);
                        }
                        return request_status == VIRTIO_BLOCK_STATUS_OK;
                }
        }

        return false;
}

static bool read_sector(struct block_device *block, uint64_t sector,
                        void *buffer) {
        (void)block;
        return virtio_block_transfer(sector, buffer, false);
}

static bool write_sector(struct block_device *block, uint64_t sector,
                         const void *buffer) {
        (void)block;
        return virtio_block_transfer(sector, (void *)buffer, true);
}

static const struct block_device_operations block_operations = {
    .read_sector = read_sector,
    .write_sector = write_sector,
};

static bool initialize_transport(uintptr_t base) {
        if (virtio_read(base, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC ||
            virtio_read(base, VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_MODERN ||
            virtio_read(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_BLOCK) {
                return false;
        }

        active_base = base;
        virtio_write(base, VIRTIO_MMIO_STATUS, 0U);
        virtio_write(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
        virtio_write(base, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

        /* VirtIO 1.0 requires feature bit 32 (bit zero of feature page one). */
        virtio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SELECT, 1U);
        if ((virtio_read(base, VIRTIO_MMIO_DEVICE_FEATURES) & 1U) == 0U) {
                goto fail;
        }
        virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SELECT, 0U);
        virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0U);
        virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SELECT, 1U);
        virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 1U);
        virtio_write(base, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_FEATURES_OK);
        if ((virtio_read(base, VIRTIO_MMIO_STATUS) &
             VIRTIO_STATUS_FEATURES_OK) == 0U) {
                goto fail;
        }

        virtio_write(base, VIRTIO_MMIO_QUEUE_SELECT, 0U);
        if (virtio_read(base, VIRTIO_MMIO_QUEUE_SIZE_MAX) <
                VIRTIO_QUEUE_SIZE ||
            virtio_read(base, VIRTIO_MMIO_QUEUE_READY) != 0U) {
                goto fail;
        }

        virtio_write(base, VIRTIO_MMIO_QUEUE_SIZE, VIRTIO_QUEUE_SIZE);
        set_queue_address(VIRTIO_MMIO_QUEUE_DESCRIPTOR_LOW,
                          (uintptr_t)descriptors);
        set_queue_address(VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uintptr_t)&available);
        set_queue_address(VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uintptr_t)&used);
        virtio_write(base, VIRTIO_MMIO_QUEUE_READY, 1U);

        uint64_t capacity = virtio_read(base, VIRTIO_MMIO_CONFIG);
        capacity |= (uint64_t)virtio_read(base,
                                         VIRTIO_MMIO_CONFIG + sizeof(uint32_t))
                    << 32U;
        if (capacity == 0U) {
                goto fail;
        }

        device = (struct block_device){.sector_count = capacity,
                                       .transport_base = base,
                                       .operations = &block_operations};
        virtio_write(base, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
        return block_device_register(&device);

fail:
        virtio_write(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        active_base = 0U;
        return false;
}

bool virtio_block_init(void) {
        for (size_t index = 0U; index < platform_virtio_count(); index++) {
                uintptr_t base;
                size_t size;
                uint32_t interrupt;

                if (platform_virtio_device(index, &base, &size, &interrupt) &&
                    size >= VIRTIO_MMIO_CONFIG + sizeof(uint64_t) &&
                    initialize_transport(base)) {
                        return true;
                }
                (void)interrupt;
        }

        return false;
}
