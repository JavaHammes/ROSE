#ifndef ROSE_USER_NETWORK_H
#define ROSE_USER_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Convert between dotted decimal and the integer address representation used
 * by user_socket_address. The formatter requires at least 16 bytes. */
bool rose_ipv4_parse(const char *text, uint32_t *address);
void rose_ipv4_format(uint32_t address, char buffer[16]);

/* Resolve a numeric address or a DNS A record through the configured server.
 * Returns zero on success or a negative USER_ERROR value. */
long rose_resolve_ipv4(const char *name, uint32_t *address);

/* Internet checksum helper used by raw ICMP applications. */
uint16_t rose_internet_checksum(const void *data, size_t length);

#endif
