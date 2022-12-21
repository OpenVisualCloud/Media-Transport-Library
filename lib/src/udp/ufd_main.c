/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "ufd_main.h"

#include "../mt_log.h"
#include "udp_main.h"

static struct ufd_mt_ctx* g_ufd_mt_ctx;

#ifdef WINDOWSENV
/* no PTHREAD_MUTEX_INITIALIZER support for win */
#define UFD_CTX_SPIN_LOCK
#endif

#ifdef UFD_CTX_SPIN_LOCK
static rte_spinlock_t g_ufd_mt_ctx_lock = RTE_SPINLOCK_INITIALIZER;
static inline int ufd_mtl_ctx_lock(void) {
  rte_spinlock_lock(&g_ufd_mt_ctx_lock);
  return 0;
}

static inline int ufd_mtl_ctx_unlock(void) {
  rte_spinlock_unlock(&g_ufd_mt_ctx_lock);
  return 0;
}
#else
static pthread_mutex_t g_ufd_mt_ctx_lock = PTHREAD_MUTEX_INITIALIZER;
static inline int ufd_mtl_ctx_lock(void) {
  return mt_pthread_mutex_lock(&g_ufd_mt_ctx_lock);
}

static inline int ufd_mtl_ctx_unlock(void) {
  return mt_pthread_mutex_unlock(&g_ufd_mt_ctx_lock);
}
#endif

static inline int ufd_idx2fd(struct ufd_mt_ctx* ctx, int idx) {
  return (ctx->fd_base + idx);
}

static inline int ufd_fd2idx(struct ufd_mt_ctx* ctx, int fd) {
  return (fd - ctx->fd_base);
}

static int ufd_free_slot(struct ufd_mt_ctx* ctx, struct ufd_slot* slot) {
  int idx = slot->idx;

  if (ctx->slots[idx] != slot) {
    err("%s(%d), slot mismatch %p %p\n", __func__, idx, ctx->slots[idx], slot);
  }

  if (slot->handle) {
    mudp_close(slot->handle);
    slot->handle = NULL;
  }
  mt_rte_free(slot);
  ctx->slots[idx] = NULL;
  return 0;
}

static int ufd_free_mt_ctx(struct ufd_mt_ctx* ctx) {
  if (ctx->slots) {
    for (int i = 0; i < ctx->slots_nb_max; i++) {
      /* check if any not free slot */
      if (!ctx->slots[i]) continue;
      warn("%s, not close slot on idx %d\n", __func__, i);
      ufd_free_slot(ctx, ctx->slots[i]);
    }
    mt_rte_free(ctx->slots);
    ctx->slots = NULL;
  }
  if (ctx->mt) {
    mtl_uninit(ctx->mt);
    ctx->mt = NULL;
  }
  mt_pthread_mutex_destroy(&ctx->slots_lock);
  mt_free(ctx);
  return 0;
}

static int ufd_parse_interfaces(struct ufd_mt_ctx* ctx, json_object* obj) {
  struct mtl_init_params* p = &ctx->mt_params;

  const char* name = json_object_get_string(mt_json_object_get(obj, "port"));
  if (!name) {
    err("%s, no port in the json interface\n", __func__);
    return -EINVAL;
  }
  snprintf(p->port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", name);

  const char* sip = json_object_get_string(mt_json_object_get(obj, "ip"));
  if (!sip) {
    err("%s, no ip in the json interface\n", __func__);
    return -EINVAL;
  }
  int ret = inet_pton(AF_INET, sip, p->sip_addr[MTL_PORT_P]);
  if (ret != 1) {
    err("%s, inet pton fail ip %s\n", __func__, sip);
    return -EINVAL;
  }

  p->num_ports++;

  return 0;
}

static int ufd_parse_json(struct ufd_mt_ctx* ctx, const char* filename) {
  json_object* root = json_object_from_file(filename);
  if (root == NULL) {
    err("%s, open json file %s fail\n", __func__, filename);
    return -EIO;
  }
  info("%s, parse %s with json-c version: %s\n", __func__, filename, json_c_version());
  int ret = -EIO;

  /* parse interfaces for system */
  json_object* interfaces_array = mt_json_object_get(root, "interfaces");
  if (interfaces_array == NULL ||
      json_object_get_type(interfaces_array) != json_type_array) {
    err("%s, can not parse interfaces\n", __func__);
    ret = -EINVAL;
    goto out;
  }
  int num_interfaces = json_object_array_length(interfaces_array);
  if (num_interfaces != 1) {
    err("%s, only support one, interfaces nb %d\n", __func__, num_interfaces);
    ret = -EINVAL;
    goto out;
  }
  ret = ufd_parse_interfaces(ctx, json_object_array_get_idx(interfaces_array, 0));
  if (ret < 0) goto out;

  struct mtl_init_params* p = &ctx->mt_params;
  json_object* obj;

  obj = mt_json_object_get(root, "nb_nic_queues");
  if (obj) {
    int nb_nic_queues = json_object_get_int(obj);
    if ((nb_nic_queues < 0) || (nb_nic_queues > 512)) {
      err("%s, invalid nb_nic_queues %d\n", __func__, nb_nic_queues);
      ret = -EINVAL;
      goto out;
    }
    p->tx_sessions_cnt_max = nb_nic_queues;
    p->rx_sessions_cnt_max = nb_nic_queues;
    info("%s, nb_nic_queues %d\n", __func__, nb_nic_queues);
  }

  obj = mt_json_object_get(root, "nb_udp_sockets");
  if (obj) {
    int nb_udp_sockets = json_object_get_int(obj);
    if ((nb_udp_sockets < 0) || (nb_udp_sockets > 4096)) {
      err("%s, invalid nb_udp_sockets %d\n", __func__, nb_udp_sockets);
      ret = -EINVAL;
      goto out;
    }
    ctx->slots_nb_max = nb_udp_sockets;
    info("%s, nb_udp_sockets %d\n", __func__, nb_udp_sockets);
  }

  ret = 0;

out:
  json_object_put(root);
  return ret;
}

static int ufd_config_init(struct ufd_mt_ctx* ctx) {
  const char* cfg_path = getenv(MUFD_CFG_ENV_NAME);
  int ret;

  if (cfg_path) {
    info("%s, env %s: %s\n", __func__, MUFD_CFG_ENV_NAME, cfg_path);
    ret = ufd_parse_json(ctx, cfg_path);
  } else {
    /* fallback path */
    ret = ufd_parse_json(ctx, "ufd.json");
  }

  return ret;
}

static struct ufd_mt_ctx* ufd_create_mt_ctx(void) {
  struct ufd_mt_ctx* ctx = mt_zmalloc(sizeof(*ctx));

  if (!ctx) { /* create a new ctx */
    err("%s, malloc ctx mem fail\n", __func__);
    return NULL;
  }

  ctx->slots_nb_max = 1024;
  ctx->fd_base = UFD_FD_BASE_DEFAULT;
  mt_pthread_mutex_init(&ctx->slots_lock, NULL);

  /* init mtl context */
  struct mtl_init_params* p = &ctx->mt_params;
  p->flags |= MTL_FLAG_BIND_NUMA;     /* default bind to numa */
  p->flags |= MTL_FLAG_UDP_TRANSPORT; /* udp transport */
  p->log_level = MTL_LOG_LEVEL_INFO;  /* default to info */

  /* get user config from json */
  int ret = ufd_config_init(ctx);
  if (ret < 0) {
    err("%s, ufd config init fail %d\n", __func__, ret);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }

  ctx->mt = mtl_init(p);
  if (!ctx->mt) {
    err("%s, mtl init fail\n", __func__);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }
  ctx->socket = mt_socket_id(ctx->mt, MTL_PORT_P);

  ctx->slots =
      mt_rte_zmalloc_socket(sizeof(*ctx->slots) * ctx->slots_nb_max, ctx->socket);
  if (!ctx->slots) {
    err("%s, slots malloc fail\n", __func__);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }

  info("%s, succ, slots_nb_max %d\n", __func__, ctx->slots_nb_max);
  return ctx;
}

static struct ufd_mt_ctx* ufd_get_mt_ctx(bool create) {
  struct ufd_mt_ctx* ctx = NULL;

  /* get the mt ctx */
  ufd_mtl_ctx_lock();
  ctx = g_ufd_mt_ctx;
  if (!ctx && create) { /* create a new ctx */
    info("%s, start to create mt ctx\n", __func__);
    ctx = ufd_create_mt_ctx();
    g_ufd_mt_ctx = ctx;
  }
  ufd_mtl_ctx_unlock();

  return ctx;
}

static void ufd_clear_mt_ctx(void) {
  ufd_mtl_ctx_lock();
  g_ufd_mt_ctx = NULL;
  ufd_mtl_ctx_unlock();
  dbg("%s, succ\n", __func__);
}

static inline struct ufd_slot* ufd_fd2slot(int sockfd) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  int idx = ufd_fd2idx(ctx, sockfd);
  struct ufd_slot* slot = ctx->slots[idx];

  if (!slot) err("%s, invalid sockfd %d\n", __func__, sockfd);
  return slot;
}

int mufd_socket(int domain, int type, int protocol) {
  int ret;
  struct ufd_mt_ctx* ctx;
  struct ufd_slot* slot = NULL;

  ret = mudp_verfiy_socket_args(domain, type, protocol);
  if (ret < 0) return ret;
  ctx = ufd_get_mt_ctx(true);
  if (!ctx) {
    err("%s, fail to get ufd mt ctx\n", __func__);
    return -EIO;
  }

  /* find one empty slot */
  mt_pthread_mutex_lock(&ctx->slots_lock);
  for (int i = 0; i < ctx->slots_nb_max; i++) {
    if (ctx->slots[i]) continue;
    /* create a slot */
    slot = mt_rte_zmalloc_socket(sizeof(*slot), ctx->socket);
    if (!slot) {
      err("%s, slot malloc fail\n", __func__);
      mt_pthread_mutex_unlock(&ctx->slots_lock);
      return -ENOMEM;
    }
    slot->idx = i;
    ctx->slots[i] = slot;
    break;
  }
  mt_pthread_mutex_unlock(&ctx->slots_lock);

  if (!slot) {
    err("%s, all slot used\n", __func__);
    return -ENOMEM;
  }

  int idx = slot->idx;
  int fd = ufd_idx2fd(ctx, idx);
  /* update slot last idx */
  ctx->slot_last_idx = idx;

  slot->handle = mudp_socket(ctx->mt, domain, type, protocol);
  if (!slot->handle) {
    err("%s, socket create fail\n", __func__);
    ufd_free_slot(ctx, slot);
    return -ENOMEM;
  }

  info("%s(%d), succ, fd %d\n", __func__, idx, fd);
  return fd;
}

int mufd_close(int sockfd) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  int idx = ufd_fd2idx(ctx, sockfd);
  struct ufd_slot* slot = ufd_fd2slot(sockfd);

  if (!slot) {
    info("%s(%d), null slot for fd %d\n", __func__, idx, sockfd);
    return -EIO;
  }

  ufd_free_slot(ctx, slot);
  return 0;
}

int mufd_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_bind(slot->handle, addr, addrlen);
}

ssize_t mufd_sendto(int sockfd, const void* buf, size_t len, int flags,
                    const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_sendto(slot->handle, buf, len, flags, dest_addr, addrlen);
}

int mufd_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  struct mudp_pollfd mfds[nfds];
  struct ufd_slot* slot;

  for (nfds_t i = 0; i < nfds; i++) {
    dbg("%s, fd %d\n", __func__, fds[i].fd);
    slot = ufd_fd2slot(fds[i].fd);
    mfds[i].fd = slot->handle;
    mfds[i].events = fds[i].events;
  }

  int ret = mudp_poll(mfds, nfds, timeout);
  for (nfds_t i = 0; i < nfds; i++) {
    fds[i].revents = mfds[i].revents;
  }
  return ret;
}

ssize_t mufd_recvfrom(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_recvfrom(slot->handle, buf, len, flags, src_addr, addrlen);
}

int mufd_getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_getsockopt(slot->handle, level, optname, optval, optlen);
}

int mufd_setsockopt(int sockfd, int level, int optname, const void* optval,
                    socklen_t optlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_setsockopt(slot->handle, level, optname, optval, optlen);
}

int mufd_fcntl(int sockfd, int cmd, ...) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  int idx = slot->idx;

  if (cmd != F_SETFD) {
    err("%s(%d), invalid cmd %d\n", __func__, idx, cmd);
    return -1;
  }

  dbg("%s(%d), cmd %d\n", __func__, idx, cmd);
  return 0;
}

int mufd_cleanup(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (ctx) {
    ufd_free_mt_ctx(ctx);
    ufd_clear_mt_ctx();
  }
  return 0;
}

/* lib destructor to cleanup the resources */
RTE_FINI(mufd_finish) {
  mufd_cleanup();
  dbg("%s, succ\n", __func__);
}

int mufd_abort(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (ctx) mtl_abort(ctx->mt);
  return 0;
}

int mufd_set_tx_rate(int sockfd, uint64_t bps) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_set_tx_rate(slot->handle, bps);
}

uint64_t mufd_get_tx_rate(int sockfd) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_get_tx_rate(slot->handle);
}
