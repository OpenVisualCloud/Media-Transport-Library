/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "udp_preload.h"

#include <dlfcn.h>

static struct upl_ctx g_upl_ctx;

static inline struct upl_ctx* upl_get_ctx(void) { return &g_upl_ctx; }

static int upl_uinit_ctx(struct upl_ctx* ctx) {
  if (ctx->ufd_entires) {
    for (int i = 0; i < ctx->ufd_entires_nb; i++) {
      if (ctx->ufd_entires[i]) {
        warn("%s, ufd still active on %d\n", __func__, i);
      }
    }
    upl_free(ctx->ufd_entires);
    ctx->ufd_entires = NULL;
  }

  return 0;
}

static int upl_get_libc_fn(struct upl_functions* fns) {
#define UPL_LIBC_FN(__name)                          \
  do {                                               \
    fns->__name = dlsym(RTLD_NEXT, #__name);         \
    if (!fns->__name) {                              \
      err("%s, dlsym %s fail\n", __func__, #__name); \
      UPL_ERR_RET(EIO);                              \
    }                                                \
  } while (0)

  UPL_LIBC_FN(socket);
  UPL_LIBC_FN(close);
  UPL_LIBC_FN(bind);
  UPL_LIBC_FN(sendto);
  UPL_LIBC_FN(send);
  UPL_LIBC_FN(sendmsg);
  UPL_LIBC_FN(poll);
  UPL_LIBC_FN(select);
  UPL_LIBC_FN(recv);
  UPL_LIBC_FN(recvfrom);
  UPL_LIBC_FN(recvmsg);
  UPL_LIBC_FN(getsockopt);
  UPL_LIBC_FN(setsockopt);
  UPL_LIBC_FN(fcntl);
  UPL_LIBC_FN(fcntl64);
  UPL_LIBC_FN(ioctl);

  info("%s, succ\n", __func__);
  return 0;
}

static int upl_init_ctx(struct upl_ctx* ctx) {
  int ret;

  ctx->ufd_entires_nb = 1024 * 10; /* max fd we support */
  ctx->ufd_entires = upl_zmalloc(sizeof(*ctx->ufd_entires) * ctx->ufd_entires_nb);
  if (!ctx->ufd_entires) {
    err("%s, ufd_entires malloc fail, nb %d\n", __func__, ctx->ufd_entires_nb);
    upl_uinit_ctx(ctx);
    UPL_ERR_RET(ENOMEM);
  }

  ret = upl_get_libc_fn(&ctx->libc_fn);
  if (ret < 0) {
    upl_uinit_ctx(ctx);
    UPL_ERR_RET(EIO);
  }

  ctx->init_succ = true;
  info("%s, succ ctx %p\n", __func__, ctx);
  return 0;
}

static inline int upl_set_ufd_entry(struct upl_ctx* ctx, int kfd,
                                    struct upl_ufd_entry* ufd) {
  ctx->ufd_entires[kfd] = ufd;
  info("%s(%d), ufd entry %p\n", __func__, kfd, ufd);
  return 0;
}

static inline int upl_clear_ufd_entry(struct upl_ctx* ctx, int kfd) {
  ctx->ufd_entires[kfd] = NULL;
  return 0;
}

static inline struct upl_ufd_entry* upl_get_ufd_entry(struct upl_ctx* ctx, int kfd) {
  if (!ctx->has_mtl_udp) return NULL;
  struct upl_ufd_entry* entry = ctx->ufd_entires[kfd];
  dbg("%s(%d), ufd entry %p\n", __func__, kfd, entry);
  return entry;
}

static bool upl_is_ufd_entry(struct upl_ctx* ctx, int kfd) {
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, kfd);
  dbg("%s(%d), ufd entry %p\n", __func__, kfd, entry);
  if (!entry || entry->bind_kfd)
    return false;
  else
    return true;
}

static void __attribute__((constructor)) upl_init() {
  struct upl_ctx* ctx = upl_get_ctx();
  int ret;

  ret = upl_init_ctx(ctx);
  if (ret < 0) {
    err("%s, init ctx fail %d\n", __func__, ret);
    return;
  }

  ret = mufd_init_context();
  if (ret < 0) {
    warn("%s, mufd init fail %d, fallback to posix socket\n", __func__, ret);
  } else {
    ctx->mtl_fd_base = mufd_base_fd();
    ctx->has_mtl_udp = true;
    info("%s, mufd init succ, base fd %d\n", __func__, ctx->mtl_fd_base);
  }
}

static void __attribute__((destructor)) upl_uinit() {
  struct upl_ctx* ctx = upl_get_ctx();
  upl_uinit_ctx(ctx);
  ctx->has_mtl_udp = false;
}

static int upl_stat_dump(void* priv) {
  struct upl_ufd_entry* entry = priv;
  if (entry->stat_tx_ufd_cnt || entry->stat_rx_ufd_cnt) {
    info("%s(%d), ufd pkt tx %d rx %d\n", __func__, entry->ufd, entry->stat_tx_ufd_cnt,
         entry->stat_rx_ufd_cnt);
    entry->stat_tx_ufd_cnt = 0;
    entry->stat_rx_ufd_cnt = 0;
  }
  if (entry->stat_tx_kfd_cnt || entry->stat_rx_kfd_cnt) {
    info("%s(%d), kfd pkt tx %d rx %d\n", __func__, entry->ufd, entry->stat_tx_kfd_cnt,
         entry->stat_rx_kfd_cnt);
    entry->stat_tx_kfd_cnt = 0;
    entry->stat_rx_kfd_cnt = 0;
  }
  return 0;
}

int socket(int domain, int type, int protocol) {
  struct upl_ctx* ctx = upl_get_ctx();
  int kfd;
  int ret;

  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  kfd = ctx->libc_fn.socket(domain, type, protocol);
  info("%s, kfd %d for domain %d type %d protocol %d\n", __func__, kfd, domain, type,
       protocol);
  if (kfd < 0) {
    err("%s, create kfd fail %d for domain %d type %d protocol %d\n", __func__, kfd,
        domain, type, protocol);
    return kfd;
  }

  if (kfd > ctx->ufd_entires_nb) {
    err("%s, kfd %d too big, consider enlarge entires space %d\n", __func__, kfd,
        ctx->ufd_entires_nb);
    return kfd;
  }

  if (!ctx->has_mtl_udp) return kfd;
  ret = mufd_socket_check(domain, type, protocol);
  if (ret < 0) return kfd; /* not support by mufd */

  int ufd = mufd_socket(domain, type, protocol);
  if (ufd < 0) {
    err("%s, create ufd fail %d for domain %d type %d protocol %d\n", __func__, ufd,
        domain, type, protocol);
    return kfd; /* return kfd for fallback path */
  }

  struct upl_ufd_entry* entry = upl_zmalloc(sizeof(*entry));
  if (!entry) {
    err("%s, entry malloc fail for ufd %d\n", __func__, ufd);
    mufd_close(ufd);
    return kfd; /* return kfd for fallback path */
  }
  entry->ufd = ufd;

  ret = mufd_register_stat_dump_cb(ufd, upl_stat_dump, entry);
  if (ret < 0) {
    err("%s, register stat dump for ufd %d\n", __func__, ufd);
    upl_free(entry);
    mufd_close(ufd);
    return kfd; /* return kfd for fallback path */
  }

  upl_set_ufd_entry(ctx, kfd, entry);
  info("%s, ufd %d kfd %d for domain %d type %d protocol %d\n", __func__, ufd, kfd,
       domain, type, protocol);
  return kfd;
}

int close(int sockfd) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s, sockfd %d\n", __func__, sockfd);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.close(sockfd);

  int ufd = entry->ufd;
  mufd_close(ufd);
  upl_free(entry);
  upl_clear_ufd_entry(ctx, sockfd);
  info("%s, ufd %d kfd %d\n", __func__, ufd, sockfd);
  /* close the kfd */
  return ctx->libc_fn.close(sockfd);
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
#if 0
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
  info("%s(%d), port %u\n", __func__, sockfd, htons(addr_in->sin_port));
#endif
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.bind(sockfd, addr, addrlen);

  int ufd = entry->ufd;
  int ret = mufd_bind(ufd, addr, addrlen);
  if (ret >= 0) return ret; /* mufd bind succ */

  /* try kernel fallback path */
  ret = ctx->libc_fn.bind(sockfd, addr, addrlen);
  if (ret < 0) return ret;
  entry->bind_kfd = true;
  info("%s(%d), mufd bind fail, fall back to libc\n", __func__, sockfd);
  return 0;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), len %" PRIu64 "\n", __func__, sockfd, len);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.sendto(sockfd, buf, len, flags, dest_addr, addrlen);

  /* ufd only support ipv4 now */
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)dest_addr;
  uint8_t* ip = (uint8_t*)&addr_in->sin_addr.s_addr;
  int ufd = entry->ufd;

  if (mufd_tx_valid_ip(ufd, ip) < 0) {
    /* fallback to kfd if it's not in ufd address scope */
    dbg("%s(%d), fallback to kernel for ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1],
        ip[2], ip[3]);
    entry->stat_tx_kfd_cnt++;
    return ctx->libc_fn.sendto(sockfd, buf, len, flags, dest_addr, addrlen);
  } else {
    entry->stat_tx_ufd_cnt++;
    return mufd_sendto(ufd, buf, len, flags, dest_addr, addrlen);
  }
}

ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), start\n", __func__, sockfd);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || !msg->msg_name) return ctx->libc_fn.sendmsg(sockfd, msg, flags);

  if (!msg->msg_name || msg->msg_namelen < sizeof(struct sockaddr_in)) {
    warn("%s(%d), no msg_name or msg_namelen not valid\n", __func__, sockfd);
    return ctx->libc_fn.sendmsg(sockfd, msg, flags);
  }

  /* ufd only support ipv4 now */
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)msg->msg_name;
  uint8_t* ip = (uint8_t*)&addr_in->sin_addr.s_addr;
  dbg("%s(%d), dst ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1], ip[2], ip[3]);
  int ufd = entry->ufd;

  if (mufd_tx_valid_ip(ufd, ip) < 0) {
    /* fallback to kfd if it's not in ufd address scope */
    dbg("%s(%d), fallback to kernel for ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1],
        ip[2], ip[3]);
    entry->stat_tx_kfd_cnt++;
    return ctx->libc_fn.sendmsg(sockfd, msg, flags);
  } else {
    entry->stat_tx_ufd_cnt++;
    return mufd_sendmsg(ufd, msg, flags);
  }
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), len %" PRIu64 "\n", __func__, sockfd, len);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.send(sockfd, buf, len, flags);

  err("%s(%d), not support ufd now\n", __func__, sockfd);
  UPL_ERR_RET(ENOTSUP);
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  if (nfds <= 0) {
    err("%s, invalid nfds %" PRIu64 "\n", __func__, nfds);
    UPL_ERR_RET(EIO);
  }

  bool is_ufd = upl_is_ufd_entry(ctx, fds[0].fd);
  /*
   * Check if all fds are the same type.
   * Todo: handle if fds is mixed with kfd and ufd.
   */
  for (nfds_t i = 1; i < nfds; i++) {
    if (upl_is_ufd_entry(ctx, fds[i].fd) != is_ufd) {
      err("%s, not same type on %" PRIu64 ", fd %d\n", __func__, i, fds[i].fd);
      UPL_ERR_RET(EIO);
    }
  }

  if (!is_ufd) return ctx->libc_fn.poll(fds, nfds, timeout);

  int kfds[nfds];
  for (nfds_t i = 0; i < nfds; i++) {
    int sockfd = fds[i].fd;
    struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
    /* save and replace with ufd */
    fds[i].fd = entry->ufd;
    kfds[i] = sockfd;
  }
  int ret = mufd_poll(fds, nfds, timeout);
  for (nfds_t i = 0; i < nfds; i++) {
    fds[i].fd = kfds[i]; /* restore */
  }
  return ret;
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout) {
  dbg("%s, nfds %d\n", __func__, nfds);
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  if (nfds <= 0) {
    err("%s, invalid nfds %d\n", __func__, nfds);
    UPL_ERR_RET(EIO);
  }

  nfds_t r_nfds = 0;
  /* Check if all fds are the same type. */
  bool first = true;
  bool is_ufd = false;
  for (int i = 0; i < nfds; i++) {
    bool is_set = false;
    if (readfds && FD_ISSET(i, readfds)) {
      is_set = true;
      r_nfds++;
    }
    if (writefds && FD_ISSET(i, writefds)) is_set = true;
    if (exceptfds && FD_ISSET(i, exceptfds)) is_set = true;
    if (!is_set) continue;
    bool c_is_ufd = upl_is_ufd_entry(ctx, i);
    if (first) {
      is_ufd = c_is_ufd;
      first = false;
    } else {
      if (c_is_ufd != is_ufd) {
        err("%s, not same type on %d, type %s\n", __func__, i, is_ufd ? "ufd" : "kfd");
        UPL_ERR_RET(EIO);
      }
    }
  }

  if (!is_ufd) return ctx->libc_fn.select(nfds, readfds, writefds, exceptfds, timeout);

  /* skip writefds and exceptfds */

  if (!readfds) return 0;

  /* reuse mufd_poll now */
  struct pollfd p_fds[r_nfds];
  int kfds[r_nfds];
  memset(p_fds, 0, sizeof(p_fds));
  int timeout_ms = 1000 * 2;
  if (timeout) timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
  nfds_t p_fds_cnt = 0;
  for (int i = 0; i < nfds; i++) {
    if (!FD_ISSET(i, readfds)) continue;
    struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, i);
    p_fds[p_fds_cnt].fd = entry->ufd;
    p_fds[p_fds_cnt].events = POLLIN;
    kfds[p_fds_cnt] = i;
    p_fds_cnt++;
    if (p_fds_cnt > r_nfds) {
      err("%s, invalid p_fds_cnt %" PRIu64 " r_nfds %" PRIu64 "\n", __func__, p_fds_cnt,
          r_nfds);
      UPL_ERR_RET(EIO);
    }
  }

  int ret = mufd_poll(p_fds, p_fds_cnt, timeout_ms);
  if (ret < 0) return ret;

  FD_ZERO(readfds);
  for (nfds_t i = 0; i < p_fds_cnt; i++) {
    if (!p_fds[i].revents) continue;
    dbg("%s, revents on ufd %d kfd %d\n", __func__, p_fds[i].fd, kfds[i]);
    FD_SET(kfds[i], readfds);
  }
  return ret;
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr,
                 socklen_t* addrlen) {
  dbg("%s(%d), start\n", __func__, sockfd);
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recvfrom(entry->ufd, buf, len, flags, src_addr, addrlen);
  }
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recv(sockfd, buf, len, flags);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recv(entry->ufd, buf, len, flags);
  }
}

ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recvmsg(sockfd, msg, flags);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recvmsg(entry->ufd, msg, flags);
  }
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.getsockopt(sockfd, level, optname, optval, optlen);
  else
    return mufd_getsockopt(entry->ufd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.setsockopt(sockfd, level, optname, optval, optlen);
  else
    return mufd_setsockopt(entry->ufd, level, optname, optval, optlen);
}

int fcntl(int sockfd, int cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.fcntl(sockfd, cmd, args);
  else
    return mufd_fcntl(entry->ufd, cmd, args);
}

int fcntl64(int sockfd, int cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.fcntl64(sockfd, cmd, args);
  else
    return mufd_fcntl(entry->ufd, cmd, args);
}

int ioctl(int sockfd, unsigned long cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.ioctl(sockfd, cmd, args);
  else
    return mufd_ioctl(entry->ufd, cmd, args);
}