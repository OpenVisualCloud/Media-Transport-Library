/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "udp_preload.h"

#include <dlfcn.h>

static struct upl_ctx g_upl_ctx;

static inline struct upl_ctx* upl_get_ctx(void) { return &g_upl_ctx; }

static int upl_uinit_ctx(struct upl_ctx* ctx) {
  if (ctx->libc_dl_handle) {
    dlclose(ctx->libc_dl_handle);
    ctx->libc_dl_handle = NULL;
  }
  info("%s, succ ctx %p\n", __func__, ctx);
  return 0;
}

static const char* upl_libc_so_paths[] = {
    "/lib/x86_64-linux-gnu/libc.so.6",
};

static void* upl_open_libc(void) {
  void* handle = NULL;

  for (int i = 0; i < MTL_ARRAY_SIZE(upl_libc_so_paths); i++) {
    handle = dlopen(upl_libc_so_paths[i], RTLD_LAZY);
    if (handle) {
      info("%s, dlopen %s succ\n", __func__, upl_libc_so_paths[i]);
      return handle;
    }
  }

  err("%s, all libc path fail, pls add your libc to upl_libc_so_paths\n", __func__);
  return NULL;
}

static int upl_get_libc_fn(struct upl_functions* fns, void* dl_handle) {
#define UPL_LIBC_FN(__name)                          \
  do {                                               \
    fns->__name = dlsym(dl_handle, #__name);         \
    if (!fns->__name) {                              \
      err("%s, dlsym %s fail\n", __func__, #__name); \
      return -EIO;                                   \
    }                                                \
  } while (0)

  UPL_LIBC_FN(socket);
  UPL_LIBC_FN(close);
  UPL_LIBC_FN(bind);
  UPL_LIBC_FN(sendto);
  UPL_LIBC_FN(poll);
  UPL_LIBC_FN(recvfrom);
  UPL_LIBC_FN(getsockopt);
  UPL_LIBC_FN(setsockopt);
  UPL_LIBC_FN(fcntl);
  UPL_LIBC_FN(fcntl64);

  info("%s, succ\n", __func__);
  return 0;
}

static int upl_init_ctx(struct upl_ctx* ctx) {
  int ret;

  ctx->libc_dl_handle = upl_open_libc();
  if (!ctx->libc_dl_handle) {
    upl_uinit_ctx(ctx);
    return -EIO;
  }
  ret = upl_get_libc_fn(&ctx->libc_fn, ctx->libc_dl_handle);
  if (ret < 0) {
    upl_uinit_ctx(ctx);
    return -EIO;
  }

  info("%s, succ ctx %p\n", __func__, ctx);
  return 0;
}

static void __attribute__((constructor)) upl_init() {
  struct upl_ctx* ctx = upl_get_ctx();
  int ret;

  ret = upl_init_ctx(ctx);
  if (ret < 0) {
    err("%s, init ctx fail %d\n", __func__, ret);
    return;
  }
  ctx->init_succ = true;

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

static bool upl_is_mtl_scoket(struct upl_ctx* ctx, int fd) {
  if (!ctx->has_mtl_udp) return false;
  if (fd < ctx->mtl_fd_base) return false;
  /* todo: add bitmap check */
  return true;
}

int socket(int domain, int type, int protocol) {
  struct upl_ctx* ctx = upl_get_ctx();
  int fd;

  dbg("%s, domain %d type %d protocol %d\n", __func__, domain, type, protocol);

  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (ctx->has_mtl_udp) {
    fd = mufd_socket(domain, type, protocol);
    if (fd >= 0) {
      info("%s, mufd %d for domain %d type %d protocol %d\n", __func__, fd, domain, type,
           protocol);
      return fd;
    }
  }

  /* the last option */
  fd = ctx->libc_fn.socket(domain, type, protocol);
  info("%s, libc fd %d for domain %d type %d protocol %d\n", __func__, fd, domain, type,
       protocol);
  return fd;
}

int close(int sockfd) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_close(sockfd);
  else
    return ctx->libc_fn.close(sockfd);
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_bind(sockfd, addr, addrlen);
  else
    return ctx->libc_fn.bind(sockfd, addr, addrlen);
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  dbg("%s(%d), len %d\n", __func__, sockfd, (int)len);
  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
  else
    return ctx->libc_fn.sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (nfds <= 0) {
    err("%s, invalid nfds %d\n", __func__, (int)nfds);
    return -EIO;
  }
  bool is_mtl = upl_is_mtl_scoket(ctx, fds[0].fd);
  /* check if all fds are the same type */
  for (nfds_t i = 1; i < nfds; i++) {
    if (upl_is_mtl_scoket(ctx, fds[i].fd) != is_mtl) {
      err("%s, not same type on %d, fd %d\n", __func__, (int)i, fds[i].fd);
      return -EIO;
    }
  }
  if (is_mtl) {
    return mufd_poll(fds, nfds, timeout);
  } else {
    return ctx->libc_fn.poll(fds, nfds, timeout);
  }
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr,
                 socklen_t* addrlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  else
    return ctx->libc_fn.recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_getsockopt(sockfd, level, optname, optval, optlen);
  else
    return ctx->libc_fn.getsockopt(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_setsockopt(sockfd, level, optname, optval, optlen);
  else
    return ctx->libc_fn.setsockopt(sockfd, level, optname, optval, optlen);
}

int fcntl(int sockfd, int cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    return -EIO;
  }
  info("%s, sockfd %d cmd %d\n", __func__, sockfd, cmd);

  if (upl_is_mtl_scoket(ctx, sockfd))
    return mufd_fcntl(sockfd, cmd, args);
  else
    return ctx->libc_fn.fcntl(sockfd, cmd, args);
}

int fcntl64(int sockfd, int cmd, va_list args) {
  info("%s, sockfd %d cmd %d\n", __func__, sockfd, cmd);
  return fcntl(sockfd, cmd, args);
}
