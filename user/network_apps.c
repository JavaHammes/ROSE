#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/network.h"
#include "rose/runtime.h"
#include "rose/syscall.h"
#include "user_abi.h"

static void print_address(uint32_t address) {
        char text[16];
        rose_ipv4_format(address, text);
        rose_print(text);
}

int rose_ifconfig_main(int argc) {
        if (argc != 1) {
                rose_print_error("usage: ifconfig\n");
                return 2;
        }
        struct user_network_information information;
        if (rose_network_information(&information) < 0 ||
            (information.flags & USER_NETWORK_ONLINE) == 0U) {
                rose_print_error("ifconfig: no network device\n");
                return 1;
        }
        rose_print("eth0: online\n  inet ");
        print_address(information.address);
        rose_print("  netmask ");
        print_address(information.netmask);
        rose_print("\n  gateway ");
        print_address(information.gateway);
        rose_print("  dns ");
        print_address(information.dns_server);
        rose_print("\n  ether ");
        static const char digits[] = "0123456789abcdef";
        for (size_t index = 0U; index < 6U; index++) {
                char octet[3] = {
                    digits[information.hardware_address[index] >> 4U],
                    digits[information.hardware_address[index] & 0x0fU],
                    index == 5U ? '\n' : ':',
                };
                (void)rose_write_all(USER_STDOUT_FILENO, octet, sizeof(octet));
        }
        return 0;
}

int rose_nslookup_main(int argc, char **argv) {
        if (argc != 2) {
                rose_print_error("usage: nslookup NAME\n");
                return 2;
        }
        uint32_t address;
        long result = rose_resolve_ipv4(argv[1], &address);
        if (result < 0) {
                rose_print_error("nslookup: name lookup failed\n");
                return 1;
        }
        rose_print("Name: ");
        rose_print(argv[1]);
        rose_print("\nAddress: ");
        print_address(address);
        rose_print("\n");
        return 0;
}

static void write_icmp_word(uint8_t *bytes, uint16_t value) {
        bytes[0] = (uint8_t)(value >> 8U);
        bytes[1] = (uint8_t)value;
}

static uint16_t read_icmp_word(const uint8_t *bytes) {
        return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static bool parse_ping_arguments(int argc, char **argv, const char **host,
                                 uint64_t *count) {
        *count = 4U;
        if (argc == 2) {
                *host = argv[1];
                return true;
        }
        if (argc == 4 && rose_strings_equal(argv[1], "-c") &&
            rose_parse_u64(argv[2], count) && *count != 0U && *count <= 100U) {
                *host = argv[3];
                return true;
        }
        return false;
}

int rose_ping_main(int argc, char **argv) {
        const char *host;
        uint64_t request_count;
        if (!parse_ping_arguments(argc, argv, &host, &request_count)) {
                rose_print_error("usage: ping [-c COUNT] HOST\n");
                return 2;
        }
        uint32_t address;
        if (rose_resolve_ipv4(host, &address) < 0) {
                rose_print_error("ping: unknown host\n");
                return 1;
        }
        long descriptor = rose_socket(USER_SOCKET_RAW,
                                      USER_INTERNET_PROTOCOL_ICMP);
        if (descriptor < 0) {
                rose_print_error("ping: cannot open socket\n");
                return 1;
        }
        struct user_socket_address destination = {.address = address};
        if (rose_socket_connect((int)descriptor, &destination) < 0) {
                (void)rose_close((int)descriptor);
                return 1;
        }
        rose_print("PING ");
        rose_print(host);
        rose_print(" (");
        print_address(address);
        rose_print(")\n");

        uint16_t identifier = (uint16_t)rose_getpid();
        uint64_t received = 0U;
        uint8_t packet[40];
        for (uint64_t sequence = 1U; sequence <= request_count; sequence++) {
                for (size_t index = 0U; index < sizeof(packet); index++) {
                        packet[index] = (uint8_t)index;
                }
                packet[0] = 8U;
                packet[1] = 0U;
                packet[2] = 0U;
                packet[3] = 0U;
                write_icmp_word(&packet[4], identifier);
                write_icmp_word(&packet[6], (uint16_t)sequence);
                write_icmp_word(&packet[2],
                                rose_internet_checksum(packet,
                                                       sizeof(packet)));
                uint64_t started = rose_monotonic_time();
                long sent = rose_socket_send_to((int)descriptor, packet,
                                                sizeof(packet), NULL);
                if (sent != (long)sizeof(packet)) {
                        rose_print_error("ping: send failed\n");
                        continue;
                }
                (void)rose_set_descriptor_flags(
                    (int)descriptor, USER_DESCRIPTOR_NONBLOCK);
                struct user_poll_descriptor ready = {
                    .descriptor = (int32_t)descriptor,
                    .events = USER_POLL_READABLE,
                };
                long poll_result = rose_poll(&ready, 1U, 1000);
                bool matched = false;
                while (poll_result == 1 &&
                       (ready.returned_events & USER_POLL_READABLE) != 0U) {
                        struct user_socket_address source;
                        long length = rose_socket_receive_from(
                            (int)descriptor, packet, sizeof(packet), &source);
                        if (length >= 8 && source.address == address &&
                            packet[0] == 0U &&
                            read_icmp_word(&packet[4]) == identifier &&
                            read_icmp_word(&packet[6]) == (uint16_t)sequence) {
                                uint64_t elapsed = rose_monotonic_time() - started;
                                rose_print_u64((uint64_t)length);
                                rose_print(" bytes from ");
                                print_address(source.address);
                                rose_print(": icmp_seq=");
                                rose_print_u64(sequence);
                                rose_print(" time=");
                                rose_print_u64(elapsed / UINT64_C(1000000));
                                rose_print(" ms\n");
                                received++;
                                matched = true;
                                break;
                        }
                        ready.returned_events = 0U;
                        poll_result = rose_poll(&ready, 1U, 0);
                }
                if (!matched) rose_print("Request timeout\n");
                uint64_t elapsed = rose_monotonic_time() - started;
                if (sequence != request_count &&
                    elapsed < UINT64_C(1000000000)) {
                        (void)rose_sleep(UINT64_C(1000000000) - elapsed);
                }
        }
        (void)rose_close((int)descriptor);
        rose_print("--- ");
        rose_print(host);
        rose_print(" ping statistics ---\n");
        rose_print_u64(request_count);
        rose_print(" packets transmitted, ");
        rose_print_u64(received);
        rose_print(" received\n");
        return received == 0U ? 1 : 0;
}
