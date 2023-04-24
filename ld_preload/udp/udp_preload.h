/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/queue.h>

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

enum mtl_log_level upl_get_log_level(void);

/* log define */
#ifdef DEBUG
#define dbg(...)                                                                 \
  do {                                                                           \
    if (upl_get_log_level() <= MTL_LOG_LEVEL_DEBUG) printf("UPL: " __VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)                                                               \
  do {                                                                          \
    if (upl_get_log_level() <= MTL_LOG_LEVEL_INFO) printf("UPL: " __VA_ARGS__); \
  } while (0)
#define notice(...)                                                              \
  do {                                                                           \
    if (upl_get_log_level() <= MTL_LOG_LEVEL_NOTICE) printf("UPL: "__VA_ARGS__); \
  } while (0)
#define warn(...)                                                                       \
  do {                                                                                  \
    if (upl_get_log_level() <= MTL_LOG_LEVEL_WARNING) printf("UPL: Warn: "__VA_ARGS__); \
  } while (0)
#define err(...)                                                                       \
  do {                                                                                 \
    if (upl_get_log_level() <= MTL_LOG_LEVEL_ERROR) printf("UPL: Error: "__VA_ARGS__); \
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
  int (*ppoll)(struct pollfd* fds, nfds_t nfds, const struct timespec* tmo_p,
               const sigset_t* sigmask);
  int (*select)(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                struct timeval* timeout);
  int (*pselect)(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                 const struct timespec* timeout, const sigset_t* sigmask);
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
  /* epoll */
  int (*epoll_create)(int size);
  int (*epoll_create1)(int flags);
  int (*epoll_ctl)(int epfd, int op, int fd, struct epoll_event* event);
  int (*epoll_wait)(int epfd, struct epoll_event* events, int maxevents, int timeout);
  int (*epoll_pwait)(int epfd, struct epoll_event* events, int maxevents, int timeout,
                     const sigset_t* sigmask);
};

enum upl_entry_type {
  UPL_ENTRY_UNKNOWN = 0,
  UPL_ENTRY_UFD,
  UPL_ENTRY_EPOLL,
  UPL_ENTRY_MAX,
};

struct upl_ctx; /* forward declare */

struct upl_base_entry {
  struct upl_ctx* parent;
  enum upl_entry_type upl_type;
};

/* ufd entry for socket */
struct upl_ufd_entry {
  /* base, always the first element */
  struct upl_base_entry base;

  int ufd;
  int kfd;
  bool bind_kfd; /* fallback to kernel fd in the bind */

  int efd; /* the efd by epoll_ctl add */

  int stat_tx_ufd_cnt;
  int stat_rx_ufd_cnt;
  int stat_tx_kfd_cnt;
  int stat_rx_kfd_cnt;
  int stat_epoll_cnt;
  int stat_epoll_revents_cnt;
  int stat_select_cnt;
  int stat_select_revents_cnt;
  int stat_poll_cnt;
  int stat_poll_revents_cnt;
};

struct upl_efd_fd_item {
  struct epoll_event event;
  struct upl_ufd_entry* ufd;
  /* linked list */
  TAILQ_ENTRY(upl_efd_fd_item) next;
};
/* List of efd fd items */
TAILQ_HEAD(upl_efd_fd_list, upl_efd_fd_item);

/* efd entry for epoll */
struct upl_efd_entry {
  /* base, always the first element */
  struct upl_base_entry base;
  int efd;
  pthread_mutex_t mutex; /* protect fds */
  struct upl_efd_fd_list fds;
  int fds_cnt;
  atomic_int kfd_cnt;
  /* for kfd query */
  struct epoll_event* events;
  int maxevents;
  const sigset_t* sigmask;
  int kfd_ret;
};

struct upl_ctx {
  bool init_succ;
  enum mtl_log_level log_level;

  bool has_mtl_udp;
  int mtl_fd_base;

  struct upl_functions libc_fn;

  int upl_entires_nb; /* the number of upl_entires */
  void** upl_entires; /* upl entries */
};

struct upl_select_ctx {
  struct upl_ctx* parent;
  int nfds;
  fd_set* readfds;
  fd_set* writefds;
  fd_set* exceptfds;
  struct timeval* timeout;
  /* for select */
  const struct timespec* timeout_spec;
  const sigset_t* sigmask;
};

struct upl_poll_ctx {
  struct upl_ctx* parent;
  struct pollfd* fds;
  nfds_t nfds;
  int timeout;
  /* for ppoll */
  const struct timespec* tmo_p;
  const sigset_t* sigmask;
};

#endif
