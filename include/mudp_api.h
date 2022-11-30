/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file mudi_api.h
 *
 * Interfaces to udp transport context.
 *
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include "mtl_api.h"

#ifndef _MUDP_API_HEAD_H_
#define _MUDP_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Standard UDP is 1460 bytes, mtu is 1500 */
#define MUDP_MAX_BYTES (1460)

/**
 * Handle to udp transport context
 */
typedef struct mudp_impl* mudp_handle;

/**
 * Create a udp transport socket.
 *
 * @param mt
 *   The pointer to the media transport device context.
 * @param domain
 *   A communication domain, only AF_INET(IPv4) now.
 * @param type
 *   Which specifies the communication semantics, only SOCK_DGRAM now.
 * @param protocol
 *   Specifies a particular protocol to be used with the socket, only zero now.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to a udp transport socket.
 */
mudp_handle mudp_socket(mtl_handle mt, int domain, int type, int protocol);

/**
 * Un-initialize the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mudp_close(mudp_handle ut);

/**
 * Bind the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param addr
 *   The address specified, only AF_INET now.
 * @param addrlen
 *   Specifies the size, in bytes, of the address structure pointed to by addr.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mudp_bind(mudp_handle ut, const struct sockaddr* addr, socklen_t addrlen);

/**
 * Send data on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param buf
 *   The data buffer.
 * @param len
 *   Specifies the size, in bytes, of the data pointed to by buf.
 *   Only support size < MUDP_MAX_BYTES
 * @param flags
 *   Not support any flags now.
 * @param dest_addr
 *   The address specified, only AF_INET now.
 * @param addrlen
 *   Specifies the size, in bytes, of the address structure pointed to by dest_addr.
 * @return
 *   - >0: the number of bytes sent.
 *   - <0: Error code.
 */
ssize_t mudp_sendto(mudp_handle ut, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen);

/**
 * Receive data on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param buf
 *   The data buffer.
 * @param len
 *   Specifies the size, in bytes, of the data pointed to by buf.
 * @param flags
 *   Only support MSG_DONTWAIT now.
 * @param src_addr
 *   The address specified, only AF_INET now.
 * @param addrlen
 *   Specifies the size, in bytes, of the address structure pointed to by src_addr.
 * @return
 *   - >0: the number of bytes received.
 *   - <0: Error code.
 */
ssize_t mudp_recvfrom(mudp_handle ut, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen);

/**
 * Helper to init a IPv4 ANY addr.
 *
 * @param saddr
 *   Point to addr.
 * @param port
 *   UDP port.
 */
static void inline mudp_init_sockaddr_any(struct sockaddr_in* saddr, uint16_t port) {
  memset(saddr, 0, sizeof(*saddr));
  saddr->sin_family = AF_INET;
  saddr->sin_addr.s_addr = INADDR_ANY;
  saddr->sin_port = htons(port);
}

/**
 * Helper to init a IPv4 addr.
 *
 * @param saddr
 *   Point to addr.
 * @param ip
 *   Point to ip.
 * @param port
 *   UDP port.
 */
static void inline mudp_init_sockaddr(struct sockaddr_in* saddr,
                                      uint8_t ip[MTL_IP_ADDR_LEN], uint16_t port) {
  memset(saddr, 0, sizeof(*saddr));
  saddr->sin_family = AF_INET;
  memcpy(&saddr->sin_addr.s_addr, ip, MTL_IP_ADDR_LEN);
  saddr->sin_port = htons(port);
}

#if defined(__cplusplus)
}
#endif

#endif
