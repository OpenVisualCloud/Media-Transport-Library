/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file mudp_api.h
 *
 * Interfaces to udp transport context.
 *
 */

#include "mtl_api.h"

#ifndef _MUDP_API_HEAD_H_
#define _MUDP_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Standard UDP is 1460 bytes, mtu is 1500 */
#define MUDP_MAX_BYTES (MTL_UDP_MAX_BYTES)

/** Max GSO bytes, 64k */
#define MUDP_MAX_GSO_BYTES (64 * 1024)

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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
ssize_t mudp_sendto(mudp_handle ut, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen);

/**
 * Send data on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param msg
 *   The struct msghdr.
 * @param flags
 *   Not support any flags now.
 * @return
 *   - >0: the number of bytes sent.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
ssize_t mudp_sendmsg(mudp_handle ut, const struct msghdr* msg, int flags);

/**
 * The structure describing a polling request on mudp.
 */
struct mudp_pollfd {
  /** The handle to udp transport socket. */
  mudp_handle fd;
  /** requested events, only support POLLIN(data to read) */
  short events;
  /** returned events */
  short revents;
};

/**
 * Alias to nfds_t
 */
typedef unsigned long int mudp_nfds_t;

/**
 * Poll the udp transport socket, blocks until one of the events occurs.
 * Only support POLLIN now.
 *
 * @param fds
 *   Point to mudp_pollfd request.
 * @param nfds
 *   The number of request in fds.
 * @param timeout
 *   timeout value in ms.
 * @return
 *   - > 0: Success, returns a nonnegative value which is the number of elements
 *          in the fds whose revents fields have been set to a nonzero value.
 *   - =0: Timeout.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_poll(struct mudp_pollfd* fds, mudp_nfds_t nfds, int timeout);

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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
ssize_t mudp_recvfrom(mudp_handle ut, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen);

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
 * @return
 *   - >0: the number of bytes received.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
static inline ssize_t mudp_recv(mudp_handle ut, void* buf, size_t len, int flags) {
  return mudp_recvfrom(ut, buf, len, flags, NULL, NULL);
}

/**
 * Receive data on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param msg
 *   The msghdr structure.
 * @param flags
 *   Only support MSG_DONTWAIT now.
 * @return
 *   - >0: the number of bytes received.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
ssize_t mudp_recvmsg(mudp_handle ut, struct msghdr* msg, int flags);

/**
 * getsockopt on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_getsockopt(mudp_handle ut, int level, int optname, void* optval,
                    socklen_t* optlen);

/**
 * setsockopt on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
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
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_setsockopt(mudp_handle ut, int level, int optname, const void* optval,
                    socklen_t optlen);

/**
 * ioctl on the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param cmd
 *   cmd.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_ioctl(mudp_handle ut, unsigned long cmd, va_list args);

/**
 * Set the tx dst mac for the udp transport socket. MTL focus on data plane and only has
 * ARP support. For the WAN transport, user can use this API to manual set the dst mac.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param mac
 *   The mac address.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_tx_mac(mudp_handle ut, uint8_t mac[MTL_MAC_ADDR_LEN]);

/**
 * Set the rate(speed) for one udp transport socket. Call before mudp_bind.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param bps
 *   Bit per second.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_tx_rate(mudp_handle ut, uint64_t bps);

/**
 * Get the rate(speed) for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @return
 *   - bps, Bit per second.
 */
uint64_t mudp_get_tx_rate(mudp_handle ut);

/**
 * Set the tx timeout(us) for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ms
 *   Tx timeout ms.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_tx_timeout(mudp_handle ut, unsigned int us);

/**
 * Get the tx timeout value for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @return
 *   - tx timeout: us.
 */
unsigned int mudp_get_tx_timeout(mudp_handle ut);

/**
 * Set the rx timeout(us) for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ms
 *   Rx timeout us.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_rx_timeout(mudp_handle ut, unsigned int us);

/**
 * Get the rx timeout value for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @return
 *   - Rx timeout: us.
 */
unsigned int mudp_get_rx_timeout(mudp_handle ut);

/**
 * Set the arp timeout(us) for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ms
 *   Arp timeout us.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_arp_timeout(mudp_handle ut, unsigned int us);

/**
 * Get the arp timeout value for one udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @return
 *   - arp timeout: us.
 */
unsigned int mudp_get_arp_timeout(mudp_handle ut);

/**
 * Set the count for rx ring in udp transport.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param count
 *   RX ring count.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_rx_ring_count(mudp_handle ut, unsigned int count);

/**
 * Set the count for wake thresh for lcore mode.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param count
 *   Wake threshold count.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_wake_thresh_count(mudp_handle ut, unsigned int count);

/**
 * Set wake timeout(ms) for lcore mode.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ms
 *   Timeout value in us.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_wake_timeout(mudp_handle ut, unsigned int us);

/**
 * Set rx poll sleep(ms) time.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ms
 *   Time to sleep inside poll, value in us.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_set_rx_poll_sleep(mudp_handle ut, unsigned int us);

/**
 * Enable/Disable the bind address check.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param enable
 *   Enable or not.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_bind_address_check(mudp_handle ut, bool enable);

/**
 * Get IP address of the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param ip
 *   The pointer to the IP address buffer.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_get_sip(mudp_handle ut, uint8_t ip[MTL_IP_ADDR_LEN]);

/**
 * Check if the dst ip is reach by the udp transport socket.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param dip
 *   The pointer to the dst IP address buffer.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_tx_valid_ip(mudp_handle ut, uint8_t dip[MTL_IP_ADDR_LEN]);

/**
 * Register stat dump callback fuction.
 *
 * @param ut
 *   The handle to udp transport socket.
 * @param dump
 *   The dump callback fuction.
 * @param priv
 *   The priv data to the callback fuction.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mudp_register_stat_dump_cb(mudp_handle ut, int (*dump)(void* priv), void* priv);

/**
 * Create a udp transport socket on a PCIE port.
 *
 * @param mt
 *   The pointer to the media transport device context.
 * @param domain
 *   A communication domain, only AF_INET(IPv4) now.
 * @param type
 *   Which specifies the communication semantics, only SOCK_DGRAM now.
 * @param protocol
 *   Specifies a particular protocol to be used with the socket, only zero now.
 * @param port
 *   Specifies the PCIE port, MTL_PORT_P or MTL_PORT_R.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to a udp transport socket.
 */
mudp_handle mudp_socket_port(mtl_handle mt, int domain, int type, int protocol,
                             enum mtl_port port);

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

/**
 * Helper to check if one addr is multicast.
 *
 * @param saddr
 *   Point to addr.
 * @return
 *   - multicast or not.
 */
bool mudp_is_multicast(const struct sockaddr_in* saddr);

#if defined(__cplusplus)
}
#endif

#endif
