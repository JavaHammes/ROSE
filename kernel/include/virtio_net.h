#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { VIRTIO_NET_ETHERNET_FRAME_MAX = 1514 };

/* Initialize the first modern VirtIO network device. Received Ethernet frames
 * are delivered to network_receive_frame from the device interrupt handler. */
bool virtio_net_init(void);
bool virtio_net_send(const void *frame, size_t length);
bool virtio_net_mac_address(uint8_t address[6]);

#endif
