/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file mudp_sockfd_api.h
 *
 * Interfaces to udp transport context.
 * API has same parameters to standard socket.h
 * Use should pass a json cfg file to the lib to use these APIs. Like below:
 * MUFD_CFG=app/udp/ufd_client.json ./build/app/UfdClientSample
 * Refer to app/udp/ufd_client.json for a sample json config file.
 *
 */

#include "mudp_api.h"

#ifndef _MUDP_SOCKFD_API_HEAD_H_
/** Marco for re-inculde protect */
#define _MUDP_SOCKFD_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** The MUFD_CFG file env name */
#define MUFD_CFG_ENV_NAME "MUFD_CFG"

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
 *   the sockfd by mufd_socket.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_close(int sockfd);

/**
 * Bind the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
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
 *   the sockfd by mufd_socket.
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
 *   Point to pollfd request, fd is by mufd_socket.
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
 *   the sockfd by mufd_socket.
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

/**
 * getsockopt on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param level
 *   the sockets API level, only SOL_SOCKET.
 * @param optname
      specified options are passed uninterpreted to the appropriate protocol
      module for interpretation.
 * @param optval
      identify a buffer in which the value for the requested option.
 * @param optlen
      identify a buffer len for the requested option.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);

/**
 * setsockopt on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param level
 *   the sockets API level, only SOL_SOCKET.
 * @param optname
      specified options are passed uninterpreted to the appropriate protocol
      module for interpretation.
 * @param optval
      identify a buffer in which the value for the requested option.
 * @param optlen
      identify a buffer len for the requested option.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_setsockopt(int sockfd, int level, int optname, const void* optval,
                    socklen_t optlen);

/**
 * manipulate file descriptor on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param cmd
 *   cmd.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_fcntl(int sockfd, int cmd, ...);

/* below are the extra APIs */

/**
 * Cleanup the mufd context(dpdk resource) created by mufd_socket call.
 *
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_cleanup(void);

/**
 * Abort the mufd context.
 * Usually called in the exception case, e.g CTRL-C.
 *
 * @return
 *   - 0: Success, device aborted.
 *   - <0: Error code of the device abort.
 */
int mufd_abort(void);

/**
 * Set the rate(speed) for one udp transport socket. Call before mudp_bind.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param bps
 *   Bit per second.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int mufd_set_tx_rate(int sockfd, uint64_t bps);

/**
 * Get the rate(speed) for one udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @return
 *   - bps, Bit per second.
 */
uint64_t mufd_get_tx_rate(int sockfd);

/**
 * Helper to init a IPv4 ANY addr.
 *
 * @param saddr
 *   Point to addr.
 * @param port
 *   UDP port.
 */
static void inline mufd_init_sockaddr_any(struct sockaddr_in* saddr, uint16_t port) {
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
static void inline mufd_init_sockaddr(struct sockaddr_in* saddr,
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
