/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_UDP_PRELOAD_H_
#define _MT_UDP_PRELOAD_H_

#include <errno.h>
#include <mtl/mudp_sockfd_internal.h>
#include <stdio.h>
#include <stdlib.h>

#include "../preload_platform.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#include <mtl/mudp_sockfd_api.h>
// clang-format on

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
  ssize_t (*sendto)(int sockfd, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen);
  int (*poll)(struct pollfd* fds, nfds_t nfds, int timeout);
  ssize_t (*recvfrom)(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen);
  int (*getsockopt)(int sockfd, int level, int optname, void* optval, socklen_t* optlen);
  int (*setsockopt)(int sockfd, int level, int optname, const void* optval,
                    socklen_t optlen);
  int (*fcntl)(int sockfd, int cmd, ...);
};

struct upl_ctx {
  bool init_succ;

  bool has_mtl_udp;
  int mtl_fd_base;

  void* libc_dl_handle;
  struct upl_functions libc_fn;
};

#endif
