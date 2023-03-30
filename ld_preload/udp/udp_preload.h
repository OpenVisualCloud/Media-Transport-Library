/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "../preload_platform.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#ifdef WINDOWSENV
#include <mtl/mudp_win.h>
#endif
#include <mtl/mudp_sockfd_api.h>
#include <mtl/mudp_sockfd_internal.h>
// clang-format on

#ifndef _MT_UDP_PRELOAD_H_
#define _MT_UDP_PRELOAD_H_

/* log define */
#ifdef DEBUG
#define dbg(...)                 \
  do {                           \
    printf("UPL: " __VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)                \
  do {                           \
    printf("UPL: " __VA_ARGS__); \
  } while (0)
#define warn(...)                     \
  do {                                \
    printf("UPL: Warn: "__VA_ARGS__); \
  } while (0)
#define err(...)                       \
  do {                                 \
    printf("UPL: Error: "__VA_ARGS__); \
  } while (0)

/* On error, -1 is returned, and errno is set appropriately. */
#define UPL_ERR_RET(code) \
  do {                    \
    errno = code;         \
    return -1;            \
  } while (0)

static inline void* upl_malloc(size_t sz) { return malloc(sz); }

static inline void* upl_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void upl_free(void* p) { free(p); }

struct upl_functions {
  int (*socket)(int domain, int type, int protocol);
  int (*close)(int sockfd);
  int (*bind)(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
  ssize_t (*send)(int sockfd, const void* buf, size_t len, int flags);
  ssize_t (*sendto)(int sockfd, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen);
  ssize_t (*sendmsg)(int sockfd, const struct msghdr* msg, int flags);
  int (*poll)(struct pollfd* fds, nfds_t nfds, int timeout);
  int (*select)(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                struct timeval* timeout);
  ssize_t (*recvfrom)(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen);
  ssize_t (*recv)(int sockfd, void* buf, size_t len, int flags);
  ssize_t (*recvmsg)(int sockfd, struct msghdr* msg, int flags);
  int (*getsockopt)(int sockfd, int level, int optname, void* optval, socklen_t* optlen);
  int (*setsockopt)(int sockfd, int level, int optname, const void* optval,
                    socklen_t optlen);
  int (*fcntl)(int sockfd, int cmd, va_list args);
  int (*fcntl64)(int sockfd, int cmd, va_list args);
  int (*ioctl)(int sockfd, unsigned long cmd, va_list args);
};

struct upl_ufd_entry {
  int ufd;
  bool bind_kfd; /* fallback to kernel fd in the bind */

  int stat_tx_ufd_cnt;
  int stat_rx_ufd_cnt;
  int stat_tx_kfd_cnt;
  int stat_rx_kfd_cnt;
};

struct upl_ctx {
  bool init_succ;

  bool has_mtl_udp;
  int mtl_fd_base;

  struct upl_functions libc_fn;

  int ufd_entires_nb;                 /* the number of ufd_entires */
  struct upl_ufd_entry** ufd_entires; /* ufd entries */
};

#endif
