/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mudp_win.h
 *
 * Define to udp transport context for Windows.
 *
 */

#ifndef _MUDP_WIN_HEAD_H_
#define _MUDP_WIN_HEAD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** Structure describing iovec for sendmsg and recvmsg */
struct iovec {
  /** Pointer to data. */
  void* iov_base;
  /** Length of data. */
  size_t iov_len;
};

/** Structure describing messages sent by `sendmsg' and received by `recvmsg'. */
struct msghdr {
  /** Address to send to/receive from.  */
  void* msg_name;
  /** Length of address data.  */
  socklen_t msg_namelen;
  /** Vector of data to send/receive into.  */
  struct iovec* msg_iov;
  /** Number of elements in the vector.  */
  size_t msg_iovlen;
  /** Ancillary data (eg BSD filedesc passing). */
  void* msg_control;
  /** Ancillary data buffer length. */
  size_t msg_controllen;
  /** Flags on received message.  */
  int msg_flags;
};

/** Structure used for storage of ancillary data object information.  */
struct cmsghdr {
  /** Length of data in cmsg_data plus length of cmsghdr structure. */
  size_t cmsg_len;
  /** Originating protocol.  */
  int cmsg_level;
  /** Protocol specific type.  */
  int cmsg_type;
};

#define CMSGDATA_ALIGN(length) \
  (((length) + MAX_NATURAL_ALIGNMENT - 1) & (~(MAX_NATURAL_ALIGNMENT - 1)))

#define CMSG_FIRSTHDR(msg) \
  (((msg)->msg_controllen >= sizeof(struct cmsghdr)) ? (msg)->msg_control : NULL)

#define CMSG_LEN(length) (CMSGDATA_ALIGN(sizeof(struct cmsghdr)) + length)

#ifdef CMSG_DATA
#undef CMSG_DATA
#endif
#define CMSG_DATA(cmsg) ((PUCHAR)(cmsg) + CMSGDATA_ALIGN(sizeof(struct cmsghdr)))

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK (0x800)
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE 32
#endif

#ifndef SO_RCVBUFFORCE
#define SO_RCVBUFFORCE 33
#endif

#ifndef SO_COOKIE
#define SO_COOKIE 57
#endif

#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER 10
#endif

#ifndef SOL_UDP
#define SOL_UDP 17 /* sockopt level for UDP */
#endif

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103 /* Set GSO segmentation size */
#endif

#if defined(__cplusplus)
}
#endif

#endif
