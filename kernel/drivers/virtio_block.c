/* Modern VirtIO-MMIO block device using the shared synchronous transport. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "virtio_block.h"
#include "virtio_mmio.h"

enum {
        VIRTIO_BLOCK_QUEUE_SIZE = 8,
        VIRTIO_BLOCK_REQUEST_READ = 0,
        VIRTIO_BLOCK_REQUEST_WRITE = 1,
        VIRTIO_BLOCK_STATUS_OK = 0,
};

struct virtio_block_request {
        uint32_t type;
        uint32_t reserved;
        uint64_t sector;
};

static struct virtio_mmio_device transport;
static struct virtio_queue request_queue;
static struct virtio_block_request request __attribute__((aligned(16)));
static volatile uint8_t request_status;
static struct block_device device;

static bool virtio_block_transfer(uint64_t sector, void *buffer, bool write) {
        request.type =
            write ? VIRTIO_BLOCK_REQUEST_WRITE : VIRTIO_BLOCK_REQUEST_READ;
        request.reserved = 0U;
        request.sector = sector;
        request_status = UINT8_MAX;

        virtio_queue_set_descriptor(&request_queue, 0U, (uintptr_t)&request,
                                    sizeof(request), VIRTIO_DESCRIPTOR_NEXT,
                                    1U);
        virtio_queue_set_descriptor(
            &request_queue, 1U, (uintptr_t)buffer, BLOCK_DEVICE_SECTOR_SIZE,
            VIRTIO_DESCRIPTOR_NEXT | (write ? 0U : VIRTIO_DESCRIPTOR_WRITE),
            2U);
        virtio_queue_set_descriptor(
            &request_queue, 2U, (uintptr_t)&request_status,
            sizeof(request_status), VIRTIO_DESCRIPTOR_WRITE, 0U);
        virtio_queue_submit(&transport, &request_queue, 0U);
        return virtio_queue_wait_used(&transport, &request_queue, 0U, NULL) &&
               request_status == VIRTIO_BLOCK_STATUS_OK;
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

bool virtio_block_init(void) {
        if (!virtio_mmio_begin(&transport, VIRTIO_DEVICE_BLOCK, 0U, 0U, 0U) ||
            !virtio_mmio_queue_init(&transport, &request_queue, 0U,
                                    VIRTIO_BLOCK_QUEUE_SIZE)) {
                virtio_mmio_fail(&transport);
                return false;
        }

        uint64_t capacity = virtio_mmio_config_read32(&transport, 0U);
        capacity |=
            (uint64_t)virtio_mmio_config_read32(&transport, sizeof(uint32_t))
            << 32U;
        if (capacity == 0U) {
                virtio_mmio_fail(&transport);
                return false;
        }

        device = (struct block_device){
            .sector_count = capacity,
            .transport_base = transport.base,
            .operations = &block_operations,
        };
        virtio_mmio_finish(&transport);
        if (!block_device_register(&device)) {
                virtio_mmio_fail(&transport);
                return false;
        }
        return true;
}
