#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "user_abi.h"

struct network_socket;

/* ROSE currently obtains the conventional QEMU user-network addresses from a
 * fixed configuration. The link remains optional so headless tests which do
 * not attach a network device continue to boot. */
bool network_init(void);
bool network_is_online(void);
void network_receive_frame(const void *frame, size_t length);
void network_tick(uint64_t now_nanoseconds);

struct network_socket *network_socket_create(uint32_t type, uint32_t protocol,
                                             int *error);
void network_socket_close(struct network_socket *socket);
int network_socket_bind(struct network_socket *socket,
                        const struct user_socket_address *address);
int network_socket_connect(struct network_socket *socket,
                           const struct user_socket_address *address);
long network_socket_send(struct network_socket *socket, const void *buffer,
                         size_t length,
                         const struct user_socket_address *destination);
long network_socket_receive(struct network_socket *socket, void *buffer,
                            size_t length,
                            struct user_socket_address *source);
uint32_t network_socket_ready(const struct network_socket *socket,
                              uint32_t requested);

void network_get_information(struct user_network_information *information);

#endif
