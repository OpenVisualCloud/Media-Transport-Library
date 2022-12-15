/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file mudi_sockfd_api.h
 *
 * Interfaces to udp transport context.
 * API has same parameters to standard socket.h
 * Use should pass a json cfg file to the lib to use these APIs. Like below:
 * MUFD_CFG_PATH=mufd_cfg.json ./your_app
 *
 */

#include "mudp_api.h"

#ifndef _MUDP_SOCKFD_API_HEAD_H_
#define _MUDP_SOCKFD_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

#define MUFD_CFG_ENV "MUFD_CFG_PATH"

/**
 * Create a sockfd udp transport socket.
 *
 * @param domain
 *   A communication domain, only AF_INET(IPv4) now.
 * @param type
 *   Which specifies the communication semantics, only SOCK_DGRAM now.
 * @param protocol
 *   Specifies a particular protocol to be used with the socket, only zero now.
 * @return
 *   - >=0: Success, the sockfd.
 *   - <0: Error code.
 */
int mufd_socket(int domain, int type, int protocol);

/**
 * Close the udp transport socket.
 *
 * @param sockfd
 *   the sockfd.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_close(int sockfd);

/**
 * Bind the udp transport socket.
 *
 * @param sockfd
 *   the sockfd.
 * @param addr
 *   The address specified, only AF_INET now.
 * @param addrlen
 *   Specifies the size, in bytes, of the address structure pointed to by addr.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);

/**
 * Send data on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd.
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
ssize_t mufd_sendto(int sockfd, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen);

/**
 * Poll the udp transport socket, blocks until one of the events occurs.
 * Only support POLLIN now.
 *
 * @param fds
 *   Point to pollfd request.
 * @param nfds
 *   The number of request in fds.
 * @param timeout
 *   timeout value in ms.
 * @return
 *   - > 0: Success, returns a nonnegative value which is the number of elements
 *          in the fds whose revents fields have been set to a nonzero value.
 *   - =0: Timeosockfd.
 *   - <0: Error code.
 */
int mufd_poll(struct pollfd* fds, nfds_t nfds, int timeout);

/**
 * Receive data on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd.
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
ssize_t mufd_recvfrom(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen);

#if defined(__cplusplus)
}
#endif

#endif
