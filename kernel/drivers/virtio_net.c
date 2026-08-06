/* Interrupt-driven modern VirtIO-MMIO Ethernet device. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "network.h"
#include "plic.h"
#include "scheduler.h"
#include "virtio_mmio.h"
#include "virtio_net.h"

enum {
        VIRTIO_NET_QUEUE_SIZE = 16,
        VIRTIO_NET_FEATURE_MAC = 5,
        /* The modern version-1 header used by QEMU includes the final
         * num_buffers word in both queue directions. */
        VIRTIO_NET_HEADER_SIZE = 12,
        VIRTIO_NET_BUFFER_SIZE =
            VIRTIO_NET_HEADER_SIZE + VIRTIO_NET_ETHERNET_FRAME_MAX,
};

struct virtio_net_buffer {
        uint8_t bytes[VIRTIO_NET_BUFFER_SIZE];
} __attribute__((aligned(16)));

static struct virtio_mmio_device transport;
static struct virtio_queue receive_queue;
static struct virtio_queue transmit_queue;
static struct virtio_net_buffer receive_buffers[VIRTIO_NET_QUEUE_SIZE];
static struct virtio_net_buffer transmit_buffers[VIRTIO_NET_QUEUE_SIZE];
static bool transmit_in_flight[VIRTIO_NET_QUEUE_SIZE];
static uint8_t device_mac[6];
static bool device_ready;

static void bytes_copy(uint8_t *destination, const uint8_t *source,
                       size_t length) {
        for (size_t index = 0U; index < length; index++) {
                destination[index] = source[index];
        }
}

static void clean_transmit_queue(void) {
        uint32_t identifier;
        while (virtio_queue_pop_used(&transmit_queue, &identifier, NULL)) {
                if (identifier < VIRTIO_NET_QUEUE_SIZE) {
                        transmit_in_flight[identifier] = false;
                }
        }
}

static void handle_interrupt(void) {
        (void)virtio_mmio_ack_interrupt(&transport);
        clean_transmit_queue();

        uint32_t identifier;
        uint32_t length;
        while (virtio_queue_pop_used(&receive_queue, &identifier, &length)) {
                if (identifier < VIRTIO_NET_QUEUE_SIZE &&
                    length >= VIRTIO_NET_HEADER_SIZE &&
                    length <= VIRTIO_NET_BUFFER_SIZE) {
                        network_receive_frame(
                            &receive_buffers[identifier]
                                 .bytes[VIRTIO_NET_HEADER_SIZE],
                            length - VIRTIO_NET_HEADER_SIZE);
                }
                if (identifier < VIRTIO_NET_QUEUE_SIZE) {
                        virtio_queue_submit(&transport, &receive_queue,
                                            (uint16_t)identifier);
                }
        }
        (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
}

bool virtio_net_init(void) {
        uint64_t mac_feature = UINT64_C(1) << VIRTIO_NET_FEATURE_MAC;
        if (!virtio_mmio_begin(&transport, VIRTIO_DEVICE_NETWORK, 0U,
                               mac_feature, mac_feature) ||
            !virtio_mmio_queue_init(&transport, &receive_queue, 0U,
                                    VIRTIO_NET_QUEUE_SIZE) ||
            !virtio_mmio_queue_init(&transport, &transmit_queue, 1U,
                                    VIRTIO_NET_QUEUE_SIZE)) {
                virtio_mmio_fail(&transport);
                return false;
        }

        for (size_t index = 0U; index < sizeof(device_mac); index++) {
                device_mac[index] =
                    virtio_mmio_config_read8(&transport, index);
        }
        bool nonzero_mac = false;
        for (size_t index = 0U; index < sizeof(device_mac); index++) {
                nonzero_mac = nonzero_mac || device_mac[index] != 0U;
        }
        if (!nonzero_mac ||
            !plic_register_handler(transport.interrupt, handle_interrupt)) {
                virtio_mmio_fail(&transport);
                return false;
        }

        for (size_t index = 0U; index < VIRTIO_NET_QUEUE_SIZE; index++) {
                virtio_queue_set_descriptor(
                    &receive_queue, (uint16_t)index,
                    (uintptr_t)&receive_buffers[index],
                    sizeof(receive_buffers[index]), VIRTIO_DESCRIPTOR_WRITE,
                    0U);
        }

        virtio_mmio_finish(&transport);
        for (size_t index = 0U; index < VIRTIO_NET_QUEUE_SIZE; index++) {
                virtio_queue_submit(&transport, &receive_queue,
                                    (uint16_t)index);
        }
        device_ready = true;
        return true;
}

bool virtio_net_send(const void *frame, size_t length) {
        if (!device_ready || frame == NULL || length < 14U ||
            length > VIRTIO_NET_ETHERNET_FRAME_MAX) {
                return false;
        }

        clean_transmit_queue();
        size_t slot = 0U;
        while (slot < VIRTIO_NET_QUEUE_SIZE && transmit_in_flight[slot]) {
                slot++;
        }
        if (slot == VIRTIO_NET_QUEUE_SIZE) {
                return false;
        }

        struct virtio_net_buffer *buffer = &transmit_buffers[slot];
        for (size_t index = 0U; index < VIRTIO_NET_HEADER_SIZE; index++) {
                buffer->bytes[index] = 0U;
        }
        bytes_copy(&buffer->bytes[VIRTIO_NET_HEADER_SIZE], frame, length);
        virtio_queue_set_descriptor(
            &transmit_queue, (uint16_t)slot, (uintptr_t)buffer,
            (uint32_t)(VIRTIO_NET_HEADER_SIZE + length), 0U, 0U);
        transmit_in_flight[slot] = true;
        virtio_queue_submit(&transport, &transmit_queue, (uint16_t)slot);
        return true;
}

bool virtio_net_mac_address(uint8_t address[6]) {
        if (!device_ready || address == NULL) {
                return false;
        }
        bytes_copy(address, device_mac, sizeof(device_mac));
        return true;
}
