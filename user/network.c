#include "rose/network.h"

#include "rose/runtime.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum { DNS_PACKET_MAX = 512, DNS_PORT = 53, DNS_TIMEOUT_MS = 3000 };

static uint16_t read_be16(const uint8_t *bytes) {
        return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static void write_be16(uint8_t *bytes, uint16_t value) {
        bytes[0] = (uint8_t)(value >> 8U);
        bytes[1] = (uint8_t)value;
}

static bool dns_skip_name(const uint8_t *packet, size_t length,
                          size_t *offset) {
        size_t cursor = *offset;
        size_t labels = 0U;
        while (cursor < length && labels++ < 128U) {
                uint8_t label_length = packet[cursor++];
                if (label_length == 0U) {
                        *offset = cursor;
                        return true;
                }
                if ((label_length & 0xc0U) == 0xc0U) {
                        if (cursor >= length) return false;
                        *offset = cursor + 1U;
                        return true;
                }
                if ((label_length & 0xc0U) != 0U ||
                    label_length > 63U || label_length > length - cursor) {
                        return false;
                }
                cursor += label_length;
        }
        return false;
}

bool rose_ipv4_parse(const char *text, uint32_t *address) {
        if (text == NULL || address == NULL) return false;
        uint32_t result = 0U;
        for (size_t part = 0U; part < 4U; part++) {
                if (*text < '0' || *text > '9') return false;
                uint32_t value = 0U;
                size_t digits = 0U;
                while (*text >= '0' && *text <= '9') {
                        value = value * 10U + (uint32_t)(*text++ - '0');
                        if (++digits > 3U || value > 255U) return false;
                }
                result = (result << 8U) | value;
                if (part != 3U) {
                        if (*text++ != '.') return false;
                } else if (*text != '\0') {
                        return false;
                }
        }
        *address = result;
        return true;
}

static size_t format_octet(uint8_t value, char *buffer) {
        if (value >= 100U) {
                buffer[0] = (char)('0' + value / 100U);
                buffer[1] = (char)('0' + (value / 10U) % 10U);
                buffer[2] = (char)('0' + value % 10U);
                return 3U;
        }
        if (value >= 10U) {
                buffer[0] = (char)('0' + value / 10U);
                buffer[1] = (char)('0' + value % 10U);
                return 2U;
        }
        buffer[0] = (char)('0' + value);
        return 1U;
}

void rose_ipv4_format(uint32_t address, char buffer[16]) {
        size_t offset = 0U;
        for (size_t part = 0U; part < 4U; part++) {
                uint32_t shift = (uint32_t)(3U - part) * 8U;
                offset +=
                    format_octet((uint8_t)(address >> shift), &buffer[offset]);
                if (part != 3U) buffer[offset++] = '.';
        }
        buffer[offset] = '\0';
}

uint16_t rose_internet_checksum(const void *data, size_t length) {
        const uint8_t *bytes = data;
        uint32_t sum = 0U;
        while (length >= 2U) {
                sum += read_be16(bytes);
                bytes += 2U;
                length -= 2U;
        }
        if (length != 0U) sum += (uint16_t)bytes[0] << 8U;
        while ((sum >> 16U) != 0U) {
                sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
        }
        return (uint16_t)~sum;
}

static long dns_build_query(const char *name, uint16_t identifier,
                            uint8_t packet[DNS_PACKET_MAX]) {
        for (size_t index = 0U; index < 12U; index++) packet[index] = 0U;
        write_be16(&packet[0], identifier);
        write_be16(&packet[2], UINT16_C(0x0100));
        write_be16(&packet[4], 1U);

        size_t output = 12U;
        size_t label_start = 0U;
        size_t name_length = rose_string_length(name);
        if (name_length == 0U || name_length > 253U) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        for (size_t index = 0U; index <= name_length; index++) {
                if (name[index] != '.' && name[index] != '\0') continue;
                size_t label_length = index - label_start;
                if (label_length == 0U || label_length > 63U ||
                    output + 1U + label_length + 5U > DNS_PACKET_MAX) {
                        return -USER_ERROR_INVALID_ARGUMENT;
                }
                packet[output++] = (uint8_t)label_length;
                for (size_t byte = 0U; byte < label_length; byte++) {
                        packet[output++] = (uint8_t)name[label_start + byte];
                }
                label_start = index + 1U;
        }
        packet[output++] = 0U;
        write_be16(&packet[output], 1U);
        write_be16(&packet[output + 2U], 1U);
        return (long)(output + 4U);
}

static long dns_parse_response(const uint8_t *packet, size_t length,
                               uint16_t identifier, uint32_t *address) {
        if (length < 12U || read_be16(&packet[0]) != identifier ||
            (read_be16(&packet[2]) & UINT16_C(0x800f)) != UINT16_C(0x8000)) {
                return -USER_ERROR_IO;
        }
        uint16_t question_count = read_be16(&packet[4]);
        uint16_t answer_count = read_be16(&packet[6]);
        size_t offset = 12U;
        for (uint16_t question = 0U; question < question_count; question++) {
                if (!dns_skip_name(packet, length, &offset) ||
                    offset > length || length - offset < 4U) {
                        return -USER_ERROR_IO;
                }
                offset += 4U;
        }
        for (uint16_t answer = 0U; answer < answer_count; answer++) {
                if (!dns_skip_name(packet, length, &offset) ||
                    offset > length || length - offset < 10U) {
                        return -USER_ERROR_IO;
                }
                uint16_t type = read_be16(&packet[offset]);
                uint16_t class_value = read_be16(&packet[offset + 2U]);
                uint16_t data_length = read_be16(&packet[offset + 8U]);
                offset += 10U;
                if (data_length > length - offset) return -USER_ERROR_IO;
                if (type == 1U && class_value == 1U && data_length == 4U) {
                        *address = ((uint32_t)packet[offset] << 24U) |
                                   ((uint32_t)packet[offset + 1U] << 16U) |
                                   ((uint32_t)packet[offset + 2U] << 8U) |
                                   packet[offset + 3U];
                        return 0;
                }
                offset += data_length;
        }
        return -USER_ERROR_NO_ENTRY;
}

long rose_resolve_ipv4(const char *name, uint32_t *address) {
        if (rose_ipv4_parse(name, address)) return 0;

        struct user_network_information information;
        long result = rose_network_information(&information);
        if (result < 0) return result;
        if ((information.flags & USER_NETWORK_ONLINE) == 0U) {
                return -USER_ERROR_NO_DEVICE;
        }

        uint8_t packet[DNS_PACKET_MAX];
        uint16_t identifier = (uint16_t)(rose_monotonic_time() ^
                                         (uint64_t)rose_getpid());
        long query_length = dns_build_query(name, identifier, packet);
        if (query_length < 0) return query_length;

        long descriptor = rose_socket(USER_SOCKET_DATAGRAM,
                                      USER_INTERNET_PROTOCOL_UDP);
        if (descriptor < 0) return descriptor;
        struct user_socket_address server = {
            .address = information.dns_server,
            .port = DNS_PORT,
        };
        result = rose_socket_send_to((int)descriptor, packet,
                                     (size_t)query_length, &server);
        if (result != query_length) {
                (void)rose_close((int)descriptor);
                return result < 0 ? result : -USER_ERROR_IO;
        }
        result = rose_set_descriptor_flags((int)descriptor,
                                           USER_DESCRIPTOR_NONBLOCK);
        if (result < 0) {
                (void)rose_close((int)descriptor);
                return result;
        }
        struct user_poll_descriptor poll_descriptor = {
            .descriptor = (int32_t)descriptor,
            .events = USER_POLL_READABLE,
        };
        result = rose_poll(&poll_descriptor, 1U, DNS_TIMEOUT_MS);
        if (result != 1 ||
            (poll_descriptor.returned_events & USER_POLL_READABLE) == 0U) {
                (void)rose_close((int)descriptor);
                return result < 0 ? result : -USER_ERROR_TRY_AGAIN;
        }
        result = rose_socket_receive_from((int)descriptor, packet,
                                          sizeof(packet), NULL);
        (void)rose_close((int)descriptor);
        return result < 0 ? result
                          : dns_parse_response(packet, (size_t)result,
                                               identifier, address);
}
