/* Small IPv4 stack for the QEMU user-mode network: Ethernet, ARP, ICMP, UDP,
 * TCP clients, and descriptor-backed sockets. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "network.h"
#include "scheduler.h"
#include "timer.h"
#include "user_abi.h"
#include "virtio_net.h"

enum {
        ETHERNET_HEADER_SIZE = 14,
        ETHERNET_TYPE_IPV4 = 0x0800,
        ETHERNET_TYPE_ARP = 0x0806,
        ARP_PACKET_SIZE = 28,
        ARP_OPERATION_REQUEST = 1,
        ARP_OPERATION_REPLY = 2,
        IPV4_HEADER_SIZE = 20,
        IPV4_PROTOCOL_ICMP = 1,
        IPV4_PROTOCOL_TCP = 6,
        IPV4_PROTOCOL_UDP = 17,
        ICMP_TYPE_ECHO_REPLY = 0,
        ICMP_TYPE_ECHO_REQUEST = 8,
        UDP_HEADER_SIZE = 8,
        TCP_HEADER_SIZE = 20,
        TCP_SEND_MAX = 1024,
        TCP_RECEIVE_WINDOW = 4096,
        TCP_RETRANSMIT_NANOSECONDS = 500000000,
        TCP_RETRY_LIMIT = 6,
        SOCKET_LIMIT = 24,
        SOCKET_QUEUE_LIMIT = 8,
        SOCKET_PACKET_MAX = 1460,
        ARP_CACHE_LIMIT = 8,
        EPHEMERAL_PORT_FIRST = 49152,
        EPHEMERAL_PORT_LAST = 65535,
};

enum tcp_flag {
        TCP_FLAG_FIN = 0x01,
        TCP_FLAG_SYN = 0x02,
        TCP_FLAG_RST = 0x04,
        TCP_FLAG_PSH = 0x08,
        TCP_FLAG_ACK = 0x10,
};

enum tcp_state {
        TCP_STATE_CLOSED,
        TCP_STATE_SYN_SENT,
        TCP_STATE_ESTABLISHED,
        TCP_STATE_CLOSE_WAIT,
        TCP_STATE_ERROR,
};

#define NETWORK_ADDRESS USER_IPV4_ADDRESS(10, 0, 2, 15)
#define NETWORK_NETMASK USER_IPV4_ADDRESS(255, 255, 255, 0)
#define NETWORK_GATEWAY USER_IPV4_ADDRESS(10, 0, 2, 2)
#define NETWORK_DNS USER_IPV4_ADDRESS(10, 0, 2, 3)
#define NETWORK_BROADCAST UINT32_C(0xffffffff)
#define NETWORK_SUBNET_BROADCAST                                             \
        ((NETWORK_ADDRESS & NETWORK_NETMASK) | ~NETWORK_NETMASK)

struct queued_packet {
        size_t length;
        size_t offset;
        struct user_socket_address source;
        uint8_t data[SOCKET_PACKET_MAX];
};

struct network_socket {
        bool used;
        uint32_t type;
        uint32_t protocol;
        uint32_t local_address;
        uint16_t local_port;
        uint32_t remote_address;
        uint16_t remote_port;
        bool connected;
        uint32_t unresolved_address;
        uint8_t resolution_attempts;
        enum tcp_state tcp_state;
        uint32_t tcp_send_next;
        uint32_t tcp_receive_next;
        bool tcp_unacknowledged;
        uint32_t tcp_unacknowledged_sequence;
        uint8_t tcp_unacknowledged_flags;
        size_t tcp_unacknowledged_length;
        uint8_t tcp_unacknowledged_data[TCP_SEND_MAX];
        uint64_t tcp_last_transmit;
        uint8_t tcp_retries;
        int tcp_error;
        bool tcp_remote_closed;
        size_t queue_read;
        size_t queue_write;
        size_t queue_count;
        struct queued_packet queue[SOCKET_QUEUE_LIMIT];
};

struct arp_cache_entry {
        bool used;
        uint32_t address;
        uint8_t hardware_address[6];
};

static struct network_socket sockets[SOCKET_LIMIT];
static struct arp_cache_entry arp_cache[ARP_CACHE_LIMIT];
static uint8_t local_hardware_address[6];
static uint16_t next_ephemeral_port = EPHEMERAL_PORT_FIRST;
static uint16_t next_ipv4_identifier = 1U;
static uint32_t next_tcp_sequence = UINT32_C(0x524f5345);
static size_t next_arp_replacement;
static bool online;

/* Packet assembly lives in static storage. A process has one 4 KiB supervisor
 * trap stack, so nesting Ethernet, IPv4, and transport MTU-sized arrays there
 * would exceed that trusted stack. The single-hart kernel serializes access. */
static uint8_t transmit_ethernet_frame[VIRTIO_NET_ETHERNET_FRAME_MAX];
static uint8_t transmit_ipv4_packet[VIRTIO_NET_ETHERNET_FRAME_MAX -
                                    ETHERNET_HEADER_SIZE];
static uint8_t transmit_udp_packet[UDP_HEADER_SIZE + SOCKET_PACKET_MAX];
static uint8_t transmit_tcp_packet[TCP_HEADER_SIZE + TCP_SEND_MAX];
static uint8_t icmp_reply_packet[SOCKET_PACKET_MAX];

static void bytes_zero(void *destination, size_t length) {
        uint8_t *bytes = destination;
        for (size_t index = 0U; index < length; index++) {
                bytes[index] = 0U;
        }
}

static void bytes_copy(void *destination, const void *source, size_t length) {
        uint8_t *to = destination;
        const uint8_t *from = source;
        for (size_t index = 0U; index < length; index++) {
                to[index] = from[index];
        }
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
                        size_t length) {
        for (size_t index = 0U; index < length; index++) {
                if (left[index] != right[index]) {
                        return false;
                }
        }
        return true;
}

static uint16_t read_be16(const uint8_t *bytes) {
        return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static uint32_t read_be32(const uint8_t *bytes) {
        return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
               ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value) {
        bytes[0] = (uint8_t)(value >> 8U);
        bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value) {
        bytes[0] = (uint8_t)(value >> 24U);
        bytes[1] = (uint8_t)(value >> 16U);
        bytes[2] = (uint8_t)(value >> 8U);
        bytes[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *bytes,
                             size_t length) {
        while (length >= 2U) {
                sum += read_be16(bytes);
                bytes += 2U;
                length -= 2U;
        }
        if (length != 0U) {
                sum += (uint16_t)bytes[0] << 8U;
        }
        return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
        while ((sum >> 16U) != 0U) {
                sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
        }
        return (uint16_t)~sum;
}

static uint16_t checksum(const uint8_t *bytes, size_t length) {
        return checksum_finish(checksum_add(0U, bytes, length));
}

static uint16_t transport_checksum(uint32_t source, uint32_t destination,
                                   uint8_t protocol, const uint8_t *bytes,
                                   size_t length) {
        uint8_t pseudo_header[12];
        write_be32(&pseudo_header[0], source);
        write_be32(&pseudo_header[4], destination);
        pseudo_header[8] = 0U;
        pseudo_header[9] = protocol;
        write_be16(&pseudo_header[10], (uint16_t)length);
        uint32_t sum = checksum_add(0U, pseudo_header, sizeof(pseudo_header));
        return checksum_finish(checksum_add(sum, bytes, length));
}

static bool hardware_is_broadcast(const uint8_t address[6]) {
        static const uint8_t broadcast[6] = {0xff, 0xff, 0xff,
                                             0xff, 0xff, 0xff};
        return bytes_equal(address, broadcast, sizeof(broadcast));
}

static bool send_ethernet(const uint8_t destination[6], uint16_t type,
                          const void *payload, size_t length) {
        if (length > VIRTIO_NET_ETHERNET_FRAME_MAX - ETHERNET_HEADER_SIZE) {
                return false;
        }
        uint8_t *frame = transmit_ethernet_frame;
        bytes_copy(&frame[0], destination, 6U);
        bytes_copy(&frame[6], local_hardware_address, 6U);
        write_be16(&frame[12], type);
        bytes_copy(&frame[ETHERNET_HEADER_SIZE], payload, length);
        return virtio_net_send(frame, ETHERNET_HEADER_SIZE + length);
}

static const uint8_t *arp_lookup(uint32_t address) {
        for (size_t index = 0U; index < ARP_CACHE_LIMIT; index++) {
                if (arp_cache[index].used &&
                    arp_cache[index].address == address) {
                        return arp_cache[index].hardware_address;
                }
        }
        return NULL;
}

static void arp_remember(uint32_t address, const uint8_t hardware[6]) {
        struct arp_cache_entry *entry = NULL;
        for (size_t index = 0U; index < ARP_CACHE_LIMIT; index++) {
                if (arp_cache[index].used &&
                    arp_cache[index].address == address) {
                        entry = &arp_cache[index];
                        break;
                }
                if (!arp_cache[index].used && entry == NULL) {
                        entry = &arp_cache[index];
                }
        }
        if (entry == NULL) {
                entry = &arp_cache[next_arp_replacement++ % ARP_CACHE_LIMIT];
        }
        entry->used = true;
        entry->address = address;
        bytes_copy(entry->hardware_address, hardware, 6U);
        (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
}

static bool send_arp(uint16_t operation, const uint8_t target_hardware[6],
                     uint32_t target_address) {
        static const uint8_t broadcast[6] = {0xff, 0xff, 0xff,
                                             0xff, 0xff, 0xff};
        uint8_t packet[ARP_PACKET_SIZE];
        write_be16(&packet[0], 1U);
        write_be16(&packet[2], ETHERNET_TYPE_IPV4);
        packet[4] = 6U;
        packet[5] = 4U;
        write_be16(&packet[6], operation);
        bytes_copy(&packet[8], local_hardware_address, 6U);
        write_be32(&packet[14], NETWORK_ADDRESS);
        if (target_hardware != NULL) {
                bytes_copy(&packet[18], target_hardware, 6U);
        } else {
                bytes_zero(&packet[18], 6U);
        }
        write_be32(&packet[24], target_address);
        return send_ethernet(operation == ARP_OPERATION_REQUEST
                                 ? broadcast
                                 : target_hardware,
                             ETHERNET_TYPE_ARP, packet, sizeof(packet));
}

static int send_ipv4(uint32_t destination, uint8_t protocol,
                     const void *payload, size_t length) {
        if (!online) {
                return -USER_ERROR_NETWORK_UNREACHABLE;
        }
        if (length > VIRTIO_NET_ETHERNET_FRAME_MAX - ETHERNET_HEADER_SIZE -
                         IPV4_HEADER_SIZE) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }

        static const uint8_t broadcast_hardware[6] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        const uint8_t *hardware = broadcast_hardware;
        if (destination != NETWORK_BROADCAST &&
            destination != NETWORK_SUBNET_BROADCAST) {
                uint32_t next_hop =
                    (destination & NETWORK_NETMASK) ==
                            (NETWORK_ADDRESS & NETWORK_NETMASK)
                        ? destination
                        : NETWORK_GATEWAY;
                hardware = arp_lookup(next_hop);
                if (hardware == NULL) {
                        (void)send_arp(ARP_OPERATION_REQUEST, NULL, next_hop);
                        return -USER_ERROR_TRY_AGAIN;
                }
        }

        uint8_t *packet = transmit_ipv4_packet;
        bytes_zero(packet, IPV4_HEADER_SIZE);
        packet[0] = 0x45U;
        write_be16(&packet[2], (uint16_t)(IPV4_HEADER_SIZE + length));
        write_be16(&packet[4], next_ipv4_identifier++);
        write_be16(&packet[6], UINT16_C(0x4000));
        packet[8] = 64U;
        packet[9] = protocol;
        write_be32(&packet[12], NETWORK_ADDRESS);
        write_be32(&packet[16], destination);
        write_be16(&packet[10], checksum(packet, IPV4_HEADER_SIZE));
        bytes_copy(&packet[IPV4_HEADER_SIZE], payload, length);
        return send_ethernet(hardware, ETHERNET_TYPE_IPV4, packet,
                             IPV4_HEADER_SIZE + length)
                   ? 0
                   : -USER_ERROR_TRY_AGAIN;
}

static bool port_is_available(uint16_t port, uint32_t type,
                              const struct network_socket *except) {
        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                const struct network_socket *socket = &sockets[index];
                if (socket != except && socket->used &&
                    socket->type == type &&
                    socket->local_port == port) {
                        return false;
                }
        }
        return true;
}

static uint16_t allocate_ephemeral_port(uint32_t type) {
        size_t count =
            (size_t)EPHEMERAL_PORT_LAST - EPHEMERAL_PORT_FIRST + 1U;
        for (size_t attempt = 0U; attempt < count; attempt++) {
                uint16_t port = next_ephemeral_port++;
                if (next_ephemeral_port < EPHEMERAL_PORT_FIRST) {
                        next_ephemeral_port = EPHEMERAL_PORT_FIRST;
                }
                if (port_is_available(port, type, NULL)) {
                        return port;
                }
        }
        return 0U;
}

static bool queue_packet(struct network_socket *socket, uint32_t address,
                         uint16_t port, const uint8_t *data, size_t length) {
        if (socket->queue_count == SOCKET_QUEUE_LIMIT) {
                return false;
        }
        struct queued_packet *packet = &socket->queue[socket->queue_write];
        packet->length = length > SOCKET_PACKET_MAX ? SOCKET_PACKET_MAX
                                                     : length;
        packet->offset = 0U;
        packet->source = (struct user_socket_address){
            .address = address,
            .port = port,
            .reserved = 0U,
        };
        bytes_copy(packet->data, data, packet->length);
        socket->queue_write =
            (socket->queue_write + 1U) % SOCKET_QUEUE_LIMIT;
        socket->queue_count++;
        (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
        return true;
}

static int tcp_emit(struct network_socket *socket, uint32_t sequence,
                    uint8_t flags, const uint8_t *data, size_t length) {
        if (length > TCP_SEND_MAX) return -USER_ERROR_INVALID_ARGUMENT;
        uint8_t *packet = transmit_tcp_packet;
        bytes_zero(packet, TCP_HEADER_SIZE);
        write_be16(&packet[0], socket->local_port);
        write_be16(&packet[2], socket->remote_port);
        write_be32(&packet[4], sequence);
        write_be32(&packet[8], socket->tcp_receive_next);
        packet[12] = 5U << 4U;
        packet[13] = flags;
        write_be16(&packet[14], TCP_RECEIVE_WINDOW);
        bytes_copy(&packet[TCP_HEADER_SIZE], data, length);
        write_be16(&packet[16], transport_checksum(
                                    NETWORK_ADDRESS, socket->remote_address,
                                    IPV4_PROTOCOL_TCP, packet,
                                    TCP_HEADER_SIZE + length));
        return send_ipv4(socket->remote_address, IPV4_PROTOCOL_TCP, packet,
                         TCP_HEADER_SIZE + length);
}

static int tcp_transmit_tracked(struct network_socket *socket, uint8_t flags,
                                const uint8_t *data, size_t length) {
        if (socket->tcp_unacknowledged) return -USER_ERROR_TRY_AGAIN;
        uint32_t sequence = socket->tcp_send_next;
        int result = tcp_emit(socket, sequence, flags, data, length);
        if (result != 0) return result;

        socket->tcp_unacknowledged = true;
        socket->tcp_unacknowledged_sequence = sequence;
        socket->tcp_unacknowledged_flags = flags;
        socket->tcp_unacknowledged_length = length;
        bytes_copy(socket->tcp_unacknowledged_data, data, length);
        socket->tcp_send_next += (uint32_t)length;
        if ((flags & TCP_FLAG_SYN) != 0U) socket->tcp_send_next++;
        if ((flags & TCP_FLAG_FIN) != 0U) socket->tcp_send_next++;
        socket->tcp_last_transmit = timer_monotonic_nanoseconds();
        socket->tcp_retries = 0U;
        return 0;
}

static bool tcp_sequence_reached(uint32_t sequence, uint32_t target) {
        return (int32_t)(sequence - target) >= 0;
}

static void tcp_fail(struct network_socket *socket, int error) {
        socket->tcp_state = TCP_STATE_ERROR;
        socket->tcp_error = error;
        socket->tcp_unacknowledged = false;
        (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
}

static void receive_tcp(uint32_t source, uint32_t destination,
                        const uint8_t *packet, size_t length) {
        if (length < TCP_HEADER_SIZE ||
            transport_checksum(source, destination, IPV4_PROTOCOL_TCP, packet,
                               length) != 0U) {
                return;
        }
        size_t header_length = (size_t)(packet[12] >> 4U) * 4U;
        if (header_length < TCP_HEADER_SIZE || header_length > length) return;

        uint16_t source_port = read_be16(&packet[0]);
        uint16_t destination_port = read_be16(&packet[2]);
        uint32_t sequence = read_be32(&packet[4]);
        uint32_t acknowledgement = read_be32(&packet[8]);
        uint8_t flags = packet[13];
        const uint8_t *data = &packet[header_length];
        size_t data_length = length - header_length;

        struct network_socket *socket = NULL;
        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                struct network_socket *candidate = &sockets[index];
                if (candidate->used &&
                    candidate->type == USER_SOCKET_STREAM &&
                    candidate->local_port == destination_port &&
                    candidate->remote_address == source &&
                    candidate->remote_port == source_port &&
                    (candidate->local_address == 0U ||
                     candidate->local_address == destination)) {
                        socket = candidate;
                        break;
                }
        }
        if (socket == NULL) return;

        if ((flags & TCP_FLAG_RST) != 0U) {
                tcp_fail(socket, socket->tcp_state == TCP_STATE_SYN_SENT
                                     ? -USER_ERROR_CONNECTION_REFUSED
                                     : -USER_ERROR_CONNECTION_RESET);
                return;
        }
        if (socket->tcp_state == TCP_STATE_SYN_SENT) {
                if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
                        (TCP_FLAG_SYN | TCP_FLAG_ACK) ||
                    !socket->tcp_unacknowledged ||
                    acknowledgement != socket->tcp_send_next) {
                        return;
                }
                socket->tcp_unacknowledged = false;
                socket->tcp_receive_next = sequence + 1U;
                socket->tcp_state = TCP_STATE_ESTABLISHED;
                socket->connected = true;
                (void)tcp_emit(socket, socket->tcp_send_next, TCP_FLAG_ACK,
                               NULL, 0U);
                (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
                return;
        }
        if (socket->tcp_state != TCP_STATE_ESTABLISHED &&
            socket->tcp_state != TCP_STATE_CLOSE_WAIT) {
                return;
        }

        if ((flags & TCP_FLAG_ACK) != 0U &&
            socket->tcp_unacknowledged &&
            tcp_sequence_reached(acknowledgement, socket->tcp_send_next)) {
                socket->tcp_unacknowledged = false;
                (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
        }

        bool should_acknowledge = false;
        if (data_length != 0U) {
                should_acknowledge = true;
                if (sequence == socket->tcp_receive_next &&
                    queue_packet(socket, source, source_port, data,
                                 data_length)) {
                        socket->tcp_receive_next += (uint32_t)data_length;
                }
        }
        if ((flags & TCP_FLAG_FIN) != 0U) {
                should_acknowledge = true;
                uint32_t fin_sequence = sequence + (uint32_t)data_length;
                if (fin_sequence == socket->tcp_receive_next) {
                        socket->tcp_receive_next++;
                        socket->tcp_remote_closed = true;
                        socket->tcp_state = TCP_STATE_CLOSE_WAIT;
                        (void)scheduler_wake_all(SCHEDULER_WAIT_NETWORK);
                }
        }
        if (should_acknowledge) {
                (void)tcp_emit(socket, socket->tcp_send_next, TCP_FLAG_ACK,
                               NULL, 0U);
        }
}

static void receive_arp(const uint8_t *packet, size_t length) {
        if (length < ARP_PACKET_SIZE || read_be16(&packet[0]) != 1U ||
            read_be16(&packet[2]) != ETHERNET_TYPE_IPV4 || packet[4] != 6U ||
            packet[5] != 4U) {
                return;
        }
        uint16_t operation = read_be16(&packet[6]);
        uint32_t sender = read_be32(&packet[14]);
        uint32_t target = read_be32(&packet[24]);
        arp_remember(sender, &packet[8]);
        if (operation == ARP_OPERATION_REQUEST && target == NETWORK_ADDRESS) {
                (void)send_arp(ARP_OPERATION_REPLY, &packet[8], sender);
        }
}

static void receive_icmp(uint32_t source, const uint8_t *packet,
                         size_t length) {
        if (length < 8U || checksum(packet, length) != 0U) {
                return;
        }
        if (packet[0] == ICMP_TYPE_ECHO_REQUEST) {
                uint8_t *reply = icmp_reply_packet;
                size_t reply_length =
                    length > SOCKET_PACKET_MAX ? SOCKET_PACKET_MAX : length;
                bytes_copy(reply, packet, reply_length);
                reply[0] = ICMP_TYPE_ECHO_REPLY;
                reply[2] = 0U;
                reply[3] = 0U;
                write_be16(&reply[2], checksum(reply, reply_length));
                (void)send_ipv4(source, IPV4_PROTOCOL_ICMP, reply,
                                reply_length);
        }
        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                struct network_socket *socket = &sockets[index];
                if (socket->used && socket->type == USER_SOCKET_RAW &&
                    socket->protocol == USER_INTERNET_PROTOCOL_ICMP &&
                    (!socket->connected ||
                     socket->remote_address == source)) {
                        (void)queue_packet(socket, source, 0U, packet, length);
                }
        }
}

static void receive_udp(uint32_t source, uint32_t destination,
                        const uint8_t *packet, size_t length) {
        if (length < UDP_HEADER_SIZE) {
                return;
        }
        uint16_t source_port = read_be16(&packet[0]);
        uint16_t destination_port = read_be16(&packet[2]);
        uint16_t udp_length = read_be16(&packet[4]);
        if (udp_length < UDP_HEADER_SIZE || udp_length > length) {
                return;
        }
        uint16_t received_checksum = read_be16(&packet[6]);
        if (received_checksum != 0U &&
            transport_checksum(source, destination, IPV4_PROTOCOL_UDP, packet,
                               udp_length) != 0U) {
                return;
        }

        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                struct network_socket *socket = &sockets[index];
                if (!socket->used ||
                    socket->type != USER_SOCKET_DATAGRAM ||
                    socket->local_port != destination_port ||
                    (socket->local_address != 0U &&
                     socket->local_address != destination) ||
                    (socket->connected &&
                     (socket->remote_address != source ||
                      socket->remote_port != source_port))) {
                        continue;
                }
                (void)queue_packet(socket, source, source_port,
                                   &packet[UDP_HEADER_SIZE],
                                   udp_length - UDP_HEADER_SIZE);
                return;
        }
}

static void receive_ipv4(const uint8_t *packet, size_t length) {
        if (length < IPV4_HEADER_SIZE || (packet[0] >> 4U) != 4U) {
                return;
        }
        size_t header_length = (size_t)(packet[0] & 0x0fU) * 4U;
        uint16_t total_length = read_be16(&packet[2]);
        uint16_t fragment = read_be16(&packet[6]);
        uint32_t destination = read_be32(&packet[16]);
        if (header_length < IPV4_HEADER_SIZE || header_length > length ||
            total_length < header_length || total_length > length ||
            (fragment & UINT16_C(0x3fff)) != 0U ||
            checksum(packet, header_length) != 0U ||
            (destination != NETWORK_ADDRESS &&
             destination != NETWORK_SUBNET_BROADCAST &&
             destination != NETWORK_BROADCAST)) {
                return;
        }
        uint32_t source = read_be32(&packet[12]);
        const uint8_t *payload = &packet[header_length];
        size_t payload_length = total_length - header_length;
        if (packet[9] == IPV4_PROTOCOL_ICMP) {
                receive_icmp(source, payload, payload_length);
        } else if (packet[9] == IPV4_PROTOCOL_TCP) {
                receive_tcp(source, destination, payload, payload_length);
        } else if (packet[9] == IPV4_PROTOCOL_UDP) {
                receive_udp(source, destination, payload, payload_length);
        }
}

bool network_init(void) {
        if (!virtio_net_init() ||
            !virtio_net_mac_address(local_hardware_address)) {
                return false;
        }
        online = true;
        return true;
}

bool network_is_online(void) { return online; }

void network_receive_frame(const void *frame_pointer, size_t length) {
        if (!online || frame_pointer == NULL || length < ETHERNET_HEADER_SIZE) {
                return;
        }
        const uint8_t *frame = frame_pointer;
        if (!bytes_equal(&frame[0], local_hardware_address, 6U) &&
            !hardware_is_broadcast(&frame[0])) {
                return;
        }
        uint16_t type = read_be16(&frame[12]);
        if (type == ETHERNET_TYPE_ARP) {
                receive_arp(&frame[ETHERNET_HEADER_SIZE],
                            length - ETHERNET_HEADER_SIZE);
        } else if (type == ETHERNET_TYPE_IPV4) {
                receive_ipv4(&frame[ETHERNET_HEADER_SIZE],
                             length - ETHERNET_HEADER_SIZE);
        }
}

struct network_socket *network_socket_create(uint32_t type, uint32_t protocol,
                                             int *error) {
        if (!online) {
                if (error != NULL) *error = -USER_ERROR_NO_DEVICE;
                return NULL;
        }
        if (type == USER_SOCKET_DATAGRAM &&
            (protocol == USER_INTERNET_PROTOCOL_DEFAULT ||
             protocol == USER_INTERNET_PROTOCOL_UDP)) {
                protocol = USER_INTERNET_PROTOCOL_UDP;
        } else if (type == USER_SOCKET_STREAM &&
                   (protocol == USER_INTERNET_PROTOCOL_DEFAULT ||
                    protocol == USER_INTERNET_PROTOCOL_TCP)) {
                protocol = USER_INTERNET_PROTOCOL_TCP;
        } else if (type == USER_SOCKET_RAW &&
                   protocol == USER_INTERNET_PROTOCOL_ICMP) {
                /* Supported below. */
        } else {
                if (error != NULL) {
                        *error = -USER_ERROR_OPERATION_NOT_SUPPORTED;
                }
                return NULL;
        }

        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                if (!sockets[index].used) {
                        struct network_socket *socket = &sockets[index];
                        bytes_zero(socket, sizeof(*socket));
                        socket->used = true;
                        socket->type = type;
                        socket->protocol = protocol;
                        if (error != NULL) *error = 0;
                        return socket;
                }
        }
        if (error != NULL) *error = -USER_ERROR_TOO_MANY_FILES;
        return NULL;
}

void network_socket_close(struct network_socket *socket) {
        if (socket != NULL && socket->used) {
                if (socket->type == USER_SOCKET_STREAM &&
                    (socket->tcp_state == TCP_STATE_ESTABLISHED ||
                     socket->tcp_state == TCP_STATE_CLOSE_WAIT) &&
                    !socket->tcp_unacknowledged) {
                        (void)tcp_emit(socket, socket->tcp_send_next,
                                       TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0U);
                }
                bytes_zero(socket, sizeof(*socket));
        }
}

int network_socket_bind(struct network_socket *socket,
                        const struct user_socket_address *address) {
        if (socket == NULL || !socket->used || address == NULL ||
            address->reserved != 0U ||
            (address->address != 0U &&
             address->address != NETWORK_ADDRESS)) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        if (socket->type == USER_SOCKET_RAW) {
                socket->local_address = address->address;
                return address->port == 0U ? 0
                                           : -USER_ERROR_INVALID_ARGUMENT;
        }
        uint16_t port = address->port;
        if (port == 0U) {
                port = allocate_ephemeral_port(socket->type);
                if (port == 0U) return -USER_ERROR_ADDRESS_IN_USE;
        } else if (!port_is_available(port, socket->type, socket)) {
                return -USER_ERROR_ADDRESS_IN_USE;
        }
        socket->local_address = address->address;
        socket->local_port = port;
        return 0;
}

int network_socket_connect(struct network_socket *socket,
                           const struct user_socket_address *address) {
        if (socket == NULL || !socket->used || address == NULL ||
            address->reserved != 0U || address->address == 0U ||
            ((socket->type == USER_SOCKET_DATAGRAM ||
              socket->type == USER_SOCKET_STREAM) &&
             address->port == 0U) ||
            (socket->type == USER_SOCKET_RAW && address->port != 0U)) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        if (socket->type != USER_SOCKET_RAW && socket->local_port == 0U) {
                socket->local_port = allocate_ephemeral_port(socket->type);
                if (socket->local_port == 0U) {
                        return -USER_ERROR_ADDRESS_IN_USE;
                }
        }
        if (socket->type == USER_SOCKET_STREAM) {
                if (socket->tcp_state == TCP_STATE_ESTABLISHED) return 0;
                if (socket->tcp_state == TCP_STATE_ERROR) {
                        return socket->tcp_error;
                }
                if (socket->tcp_state == TCP_STATE_SYN_SENT &&
                    (socket->remote_address != address->address ||
                     socket->remote_port != address->port)) {
                        return -USER_ERROR_INVALID_ARGUMENT;
                }
                if (socket->tcp_state == TCP_STATE_CLOSED) {
                        socket->remote_address = address->address;
                        socket->remote_port = address->port;
                        socket->tcp_send_next = next_tcp_sequence;
                        next_tcp_sequence += UINT32_C(65537);
                        socket->tcp_state = TCP_STATE_SYN_SENT;
                }
                if (!socket->tcp_unacknowledged) {
                        int result = tcp_transmit_tracked(
                            socket, TCP_FLAG_SYN, NULL, 0U);
                        if (result == -USER_ERROR_TRY_AGAIN) {
                                if (socket->unresolved_address ==
                                    address->address) {
                                        socket->resolution_attempts++;
                                } else {
                                        socket->unresolved_address =
                                            address->address;
                                        socket->resolution_attempts = 1U;
                                }
                                if (socket->resolution_attempts >= 4U) {
                                        tcp_fail(
                                            socket,
                                            -USER_ERROR_NETWORK_UNREACHABLE);
                                        return socket->tcp_error;
                                }
                                return result;
                        }
                        if (result != 0) return result;
                        socket->unresolved_address = 0U;
                        socket->resolution_attempts = 0U;
                }
                return -USER_ERROR_TRY_AGAIN;
        }
        socket->remote_address = address->address;
        socket->remote_port = address->port;
        socket->connected = true;
        return 0;
}

long network_socket_send(struct network_socket *socket, const void *buffer,
                         size_t length,
                         const struct user_socket_address *destination) {
        if (socket == NULL || !socket->used ||
            (length != 0U && buffer == NULL) || length > SOCKET_PACKET_MAX) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        if (socket->type == USER_SOCKET_STREAM) {
                if (destination != NULL || length > TCP_SEND_MAX) {
                        return -USER_ERROR_INVALID_ARGUMENT;
                }
                if (socket->tcp_state == TCP_STATE_ERROR) {
                        return socket->tcp_error;
                }
                if (socket->tcp_state != TCP_STATE_ESTABLISHED ||
                    socket->tcp_remote_closed) {
                        return -USER_ERROR_NOT_CONNECTED;
                }
                if (length == 0U) return 0;
                int result = tcp_transmit_tracked(
                    socket, TCP_FLAG_PSH | TCP_FLAG_ACK, buffer, length);
                return result == 0 ? (long)length : result;
        }
        struct user_socket_address target;
        if (destination != NULL) {
                target = *destination;
        } else if (socket->connected) {
                target = (struct user_socket_address){
                    .address = socket->remote_address,
                    .port = socket->remote_port,
                };
        } else {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        if (target.reserved != 0U || target.address == 0U) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }

        if (socket->type == USER_SOCKET_RAW) {
                if (target.port != 0U || length < 8U) {
                        return -USER_ERROR_INVALID_ARGUMENT;
                }
                int result = send_ipv4(target.address, (uint8_t)socket->protocol,
                                       buffer, length);
                if (result == -USER_ERROR_TRY_AGAIN) {
                        if (socket->unresolved_address == target.address) {
                                socket->resolution_attempts++;
                        } else {
                                socket->unresolved_address = target.address;
                                socket->resolution_attempts = 1U;
                        }
                        return socket->resolution_attempts >= 4U
                                   ? -USER_ERROR_NETWORK_UNREACHABLE
                                   : result;
                }
                socket->unresolved_address = 0U;
                socket->resolution_attempts = 0U;
                return result == 0 ? (long)length : result;
        }
        if (target.port == 0U) return -USER_ERROR_INVALID_ARGUMENT;
        if (socket->local_port == 0U) {
                socket->local_port = allocate_ephemeral_port(socket->type);
                if (socket->local_port == 0U) {
                        return -USER_ERROR_ADDRESS_IN_USE;
                }
        }

        uint8_t *packet = transmit_udp_packet;
        write_be16(&packet[0], socket->local_port);
        write_be16(&packet[2], target.port);
        write_be16(&packet[4], (uint16_t)(UDP_HEADER_SIZE + length));
        packet[6] = 0U;
        packet[7] = 0U;
        bytes_copy(&packet[UDP_HEADER_SIZE], buffer, length);
        uint16_t packet_checksum = transport_checksum(
            NETWORK_ADDRESS, target.address, IPV4_PROTOCOL_UDP, packet,
            UDP_HEADER_SIZE + length);
        if (packet_checksum == 0U) packet_checksum = UINT16_MAX;
        write_be16(&packet[6], packet_checksum);
        int result = send_ipv4(target.address, IPV4_PROTOCOL_UDP, packet,
                               UDP_HEADER_SIZE + length);
        if (result == -USER_ERROR_TRY_AGAIN) {
                if (socket->unresolved_address == target.address) {
                        socket->resolution_attempts++;
                } else {
                        socket->unresolved_address = target.address;
                        socket->resolution_attempts = 1U;
                }
                if (socket->resolution_attempts >= 4U) {
                        return -USER_ERROR_NETWORK_UNREACHABLE;
                }
        } else {
                socket->unresolved_address = 0U;
                socket->resolution_attempts = 0U;
        }
        return result == 0 ? (long)length : result;
}

long network_socket_receive(struct network_socket *socket, void *buffer,
                            size_t length,
                            struct user_socket_address *source) {
        if (socket == NULL || !socket->used ||
            (length != 0U && buffer == NULL)) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        if (length == 0U) return 0;
        if (socket->queue_count == 0U) {
                if (socket->type == USER_SOCKET_STREAM) {
                        if (socket->tcp_error != 0) return socket->tcp_error;
                        if (socket->tcp_remote_closed) return 0;
                        if (socket->tcp_state != TCP_STATE_ESTABLISHED) {
                                return -USER_ERROR_NOT_CONNECTED;
                        }
                }
                return -USER_ERROR_TRY_AGAIN;
        }
        struct queued_packet *packet = &socket->queue[socket->queue_read];
        size_t remaining = packet->length - packet->offset;
        size_t copy_length = remaining < length ? remaining : length;
        bytes_copy(buffer, &packet->data[packet->offset], copy_length);
        if (source != NULL) *source = packet->source;
        packet->offset += copy_length;
        if (socket->type != USER_SOCKET_STREAM ||
            packet->offset == packet->length) {
                socket->queue_read =
                    (socket->queue_read + 1U) % SOCKET_QUEUE_LIMIT;
                socket->queue_count--;
        }
        return (long)copy_length;
}

uint32_t network_socket_ready(const struct network_socket *socket,
                              uint32_t requested) {
        if (socket == NULL || !socket->used) return USER_POLL_INVALID;
        uint32_t returned = 0U;
        if (!online || socket->tcp_error != 0) returned |= USER_POLL_ERROR;
        if ((requested & USER_POLL_READABLE) != 0U &&
            (socket->queue_count != 0U || socket->tcp_remote_closed ||
             socket->tcp_error != 0)) {
                returned |= USER_POLL_READABLE;
        }
        if ((requested & USER_POLL_WRITABLE) != 0U && online) {
                if (socket->type != USER_SOCKET_STREAM ||
                    (socket->tcp_state == TCP_STATE_ESTABLISHED &&
                     !socket->tcp_unacknowledged &&
                     !socket->tcp_remote_closed)) {
                        returned |= USER_POLL_WRITABLE;
                }
        }
        if (socket->tcp_remote_closed) returned |= USER_POLL_HANGUP;
        return returned;
}

void network_tick(uint64_t now_nanoseconds) {
        for (size_t index = 0U; index < SOCKET_LIMIT; index++) {
                struct network_socket *socket = &sockets[index];
                if (!socket->used ||
                    socket->type != USER_SOCKET_STREAM ||
                    !socket->tcp_unacknowledged ||
                    now_nanoseconds - socket->tcp_last_transmit <
                        TCP_RETRANSMIT_NANOSECONDS) {
                        continue;
                }
                if (socket->tcp_retries >= TCP_RETRY_LIMIT) {
                        tcp_fail(socket, -USER_ERROR_TIMED_OUT);
                        continue;
                }
                int result = tcp_emit(
                    socket, socket->tcp_unacknowledged_sequence,
                    socket->tcp_unacknowledged_flags,
                    socket->tcp_unacknowledged_data,
                    socket->tcp_unacknowledged_length);
                if (result == 0) {
                        socket->tcp_last_transmit = now_nanoseconds;
                        socket->tcp_retries++;
                }
        }
}

void network_get_information(struct user_network_information *information) {
        if (information == NULL) return;
        bytes_zero(information, sizeof(*information));
        if (online) information->flags = USER_NETWORK_ONLINE;
        information->address = NETWORK_ADDRESS;
        information->netmask = NETWORK_NETMASK;
        information->gateway = NETWORK_GATEWAY;
        information->dns_server = NETWORK_DNS;
        bytes_copy(information->hardware_address, local_hardware_address, 6U);
}
