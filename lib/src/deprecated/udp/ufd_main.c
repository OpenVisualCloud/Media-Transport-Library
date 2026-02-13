/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "ufd_main.h"

#include "../../mt_log.h"
#include "udp_main.h"

static struct ufd_mt_ctx* g_ufd_mt_ctx;
static pthread_mutex_t g_ufd_mt_ctx_lock;
static struct mufd_override_params* g_rt_para;
static struct mufd_init_params* g_init_para;

static inline int ufd_mtl_ctx_lock(void) {
  return mt_pthread_mutex_lock(&g_ufd_mt_ctx_lock);
}

static inline int ufd_mtl_ctx_unlock(void) {
  return mt_pthread_mutex_unlock(&g_ufd_mt_ctx_lock);
}

static int ufd_init_global(void) {
  mt_pthread_mutex_init(&g_ufd_mt_ctx_lock, NULL);
  return 0;
}

static int ufd_uinit_global(void) {
  mt_pthread_mutex_destroy(&g_ufd_mt_ctx_lock);
  return 0;
}

static inline int ufd_idx2fd(struct ufd_mt_ctx* ctx, int idx) {
  return (ctx->init_params.fd_base + idx);
}

static inline int ufd_fd2idx(struct ufd_mt_ctx* ctx, int fd) {
  return (fd - ctx->init_params.fd_base);
}

static inline int ufd_max_slot(struct ufd_mt_ctx* ctx) {
  return ctx->init_params.slots_nb_max;
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
  struct mtl_main_impl* mt = ctx->mt;

  if (ctx->slots) {
    for (int i = 0; i < ufd_max_slot(ctx); i++) {
      /* check if any not free slot */
      if (!ctx->slots[i]) continue;
      warn("%s, not close slot on idx %d\n", __func__, i);
      ufd_free_slot(ctx, ctx->slots[i]);
    }
    mt_rte_free(ctx->slots);
    ctx->slots = NULL;
  }
  mt_pthread_mutex_destroy(&ctx->slots_lock);
  if (ctx->alloc_with_rte)
    mt_rte_free(ctx);
  else
    mt_free(ctx);

  /* always mtl_uninit at the last */
  if (mt) {
    mtl_uninit(mt);
    mt = NULL;
  }

  return 0;
}

static int ufd_parse_interfaces(struct mufd_init_params* init, json_object* obj,
                                enum mtl_port port) {
  struct mtl_init_params* p = &init->mt_params;

  const char* name = json_object_get_string(mt_json_object_get(obj, "port"));
  if (!name) {
    err("%s, no port in the json interface\n", __func__);
    MUDP_ERR_RET(EINVAL);
  }
  snprintf(p->port[port], MTL_PORT_MAX_LEN, "%s", name);
  enum mtl_pmd_type pmd = mtl_pmd_by_port_name(name);

  json_object* obj_item = mt_json_object_get(obj, "proto");
  if (obj_item) {
    const char* proto = json_object_get_string(obj_item);
    if (strcmp(proto, "dhcp") == 0) {
      p->net_proto[port] = MTL_PROTO_DHCP;
    } else if (strcmp(proto, "static") == 0) {
      p->net_proto[port] = MTL_PROTO_STATIC;
    } else {
      err("%s, invalid network proto %s\n", __func__, proto);
      MUDP_ERR_RET(EINVAL);
    }
  }

  if ((p->net_proto[port] == MTL_PROTO_STATIC) && (pmd == MTL_PMD_DPDK_USER)) {
    obj_item = mt_json_object_get(obj, "ip");
    if (!obj_item) {
      err("%s, no ip in the json interface\n", __func__);
      MUDP_ERR_RET(EINVAL);
    }
    const char* sip = json_object_get_string(obj_item);
    int ret = inet_pton(AF_INET, sip, p->sip_addr[port]);
    if (ret != 1) {
      err("%s, inet pton fail ip %s\n", __func__, sip);
      MUDP_ERR_RET(EINVAL);
    }
    obj_item = mt_json_object_get(obj, "netmask");
    if (obj_item) {
      inet_pton(AF_INET, json_object_get_string(obj_item), p->netmask[port]);
    }
    obj_item = mt_json_object_get(obj, "gateway");
    if (obj_item) {
      inet_pton(AF_INET, json_object_get_string(obj_item), p->gateway[port]);
    }
  }

  p->num_ports++;

  return 0;
}

static int ufd_parse_json(struct mufd_init_params* init, const char* filename) {
  json_object* root = json_object_from_file(filename);
  if (root == NULL) {
    err("%s, open json file %s fail\n", __func__, filename);
    MUDP_ERR_RET(EIO);
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
  if ((num_interfaces > MTL_PORT_MAX) || (num_interfaces <= 0)) {
    err("%s, invalid interfaces nb %d\n", __func__, num_interfaces);
    ret = -EINVAL;
    goto out;
  }
  for (int i = 0; i < num_interfaces; i++) {
    ret = ufd_parse_interfaces(init, json_object_array_get_idx(interfaces_array, i), i);
    if (ret < 0) goto out;
  }

  struct mtl_init_params* p = &init->mt_params;
  json_object* obj;

  obj = mt_json_object_get(root, "nb_nic_queues");
  if (obj) {
    int nb_nic_queues = json_object_get_int(obj);
    if ((nb_nic_queues < 0) || (nb_nic_queues > 512)) {
      err("%s, invalid nb_nic_queues %d\n", __func__, nb_nic_queues);
      ret = -EINVAL;
      goto out;
    }
    for (int i = 0; i < num_interfaces; i++) {
      p->tx_queues_cnt[i] = nb_nic_queues;
      p->rx_queues_cnt[i] = nb_nic_queues;
    }
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
    init->slots_nb_max = nb_udp_sockets;
    info("%s, nb_udp_sockets %d\n", __func__, nb_udp_sockets);
  }

  obj = mt_json_object_get(root, "nb_tx_desc");
  if (obj) {
    int nb_tx_desc = json_object_get_int(obj);
    if ((nb_tx_desc < 0) || (nb_tx_desc > 4096)) {
      err("%s, invalid nb_tx_desc %d\n", __func__, nb_tx_desc);
      ret = -EINVAL;
      goto out;
    }
    p->nb_tx_desc = nb_tx_desc;
    info("%s, nb_tx_desc %d\n", __func__, nb_tx_desc);
  }

  obj = mt_json_object_get(root, "nb_rx_desc");
  if (obj) {
    int nb_rx_desc = json_object_get_int(obj);
    if ((nb_rx_desc < 0) || (nb_rx_desc > 4096)) {
      err("%s, invalid nb_rx_desc %d\n", __func__, nb_rx_desc);
      ret = -EINVAL;
      goto out;
    }
    p->nb_tx_desc = nb_rx_desc;
    info("%s, nb_rx_desc %d\n", __func__, nb_rx_desc);
  }

  obj = mt_json_object_get(root, "nic_shared_tx_queues");
  if (obj) {
    if (json_object_get_boolean(obj)) {
      info("%s, shared tx queues enabled\n", __func__);
      p->flags |= MTL_FLAG_SHARED_TX_QUEUE;
    }
  }

  obj = mt_json_object_get(root, "nic_shared_rx_queues");
  if (obj) {
    if (json_object_get_boolean(obj)) {
      info("%s, shared rx queues enabled\n", __func__);
      p->flags |= MTL_FLAG_SHARED_RX_QUEUE;
    }
  }

  obj = mt_json_object_get(root, "udp_lcore");
  if (obj) {
    if (json_object_get_boolean(obj)) {
      info("%s, udp lcore enabled\n", __func__);
      p->flags |= MTL_FLAG_UDP_LCORE;
    }
  }

  obj = mt_json_object_get(root, "rss");
  if (obj) {
    if (json_object_get_boolean(obj)) {
      info("%s, rss enabled\n", __func__);
      p->rss_mode = MTL_RSS_MODE_L3_L4;
    }
  }

  obj = mt_json_object_get(root, "log_level");
  if (obj) {
    const char* str = json_object_get_string(obj);
    if (str) {
      if (!strcmp(str, "debug"))
        p->log_level = MTL_LOG_LEVEL_DEBUG;
      else if (!strcmp(str, "info"))
        p->log_level = MTL_LOG_LEVEL_INFO;
      else if (!strcmp(str, "notice"))
        p->log_level = MTL_LOG_LEVEL_NOTICE;
      else if (!strcmp(str, "warning"))
        p->log_level = MTL_LOG_LEVEL_WARNING;
      else if (!strcmp(str, "error"))
        p->log_level = MTL_LOG_LEVEL_ERR;
      else
        err("%s, unknow log level %s\n", __func__, str);
    }
  }

  obj = mt_json_object_get(root, "fd_base");
  if (obj) {
    int fd_base = json_object_get_int(obj);
    int limit = INT_MAX / 2;
    if (fd_base < limit) {
      err("%s, invalid fd_base %d, must bigger than %d\n", __func__, fd_base, limit);
      ret = -EINVAL;
      goto out;
    }
    init->fd_base = fd_base;
    info("%s, fd_base %d\n", __func__, fd_base);
  }

  obj = mt_json_object_get(root, "nic_queue_rate_limit_g");
  if (obj) {
    int rl_bps_g = json_object_get_int(obj);
    if (rl_bps_g < 0) {
      err("%s, invalid rl_bps_g %d\n", __func__, rl_bps_g);
      ret = -EINVAL;
      goto out;
    }
    init->txq_bps = (uint64_t)rl_bps_g * 1000 * 1000 * 1000;
    info("%s, nic_queue_rate_limit_g %d\n", __func__, rl_bps_g);
  }

  obj = mt_json_object_get(root, "rx_ring_count");
  if (obj) {
    int rx_ring_count = json_object_get_int(obj);
    if (rx_ring_count < 0) {
      err("%s, invalid rx_ring_count %d\n", __func__, rx_ring_count);
      ret = -EINVAL;
      goto out;
    }
    init->rx_ring_count = rx_ring_count;
    info("%s, rx_ring_count %d\n", __func__, rx_ring_count);
  }

  obj = mt_json_object_get(root, "wake_thresh_count");
  if (obj) {
    int wake_thresh_count = json_object_get_int(obj);
    if (wake_thresh_count < 0) {
      err("%s, invalid wake_thresh_count %d\n", __func__, wake_thresh_count);
      ret = -EINVAL;
      goto out;
    }
    init->wake_thresh_count = wake_thresh_count;
    info("%s, wake_thresh_count %d\n", __func__, wake_thresh_count);
  }

  obj = mt_json_object_get(root, "wake_timeout_us");
  if (obj) {
    int wake_timeout_us = json_object_get_int(obj);
    if (wake_timeout_us < 0) {
      err("%s, invalid wake_timeout_us %d\n", __func__, wake_timeout_us);
      ret = -EINVAL;
      goto out;
    }
    init->wake_timeout_us = wake_timeout_us;
    info("%s, wake_timeout_us %d\n", __func__, wake_timeout_us);
  }

  obj = mt_json_object_get(root, "rx_poll_sleep_us");
  if (obj) {
    int rx_poll_sleep_us = json_object_get_int(obj);
    if (rx_poll_sleep_us < 0) {
      err("%s, invalid rx_poll_sleep_us %d\n", __func__, rx_poll_sleep_us);
      ret = -EINVAL;
      goto out;
    }
    init->rx_poll_sleep_us = rx_poll_sleep_us;
    info("%s, rx_poll_sleep_us %d\n", __func__, rx_poll_sleep_us);
  }

  obj = mt_json_object_get(root, "bind_address_check");
  if (obj) {
    if (json_object_get_boolean(obj)) {
      info("%s, bind address check enabled\n", __func__);
      init->flags |= MUFD_FLAG_BIND_ADDRESS_CHECK;
    }
  }

  ret = 0;

out:
  json_object_put(root);
  return ret;
}

static int ufd_set_afxdp(struct ufd_mt_ctx* ctx) {
  struct mtl_init_params* p = &ctx->init_params.mt_params;

  for (uint8_t i = 0; i < p->num_ports; i++) {
    p->pmd[i] = mtl_pmd_by_port_name(p->port[i]);
  }

  return 0;
}

static int ufd_config_init(struct ufd_mt_ctx* ctx) {
  const char* cfg_path = getenv(MUFD_CFG_ENV_NAME);
  int ret;

  if (cfg_path) {
    info("%s, env %s: %s\n", __func__, MUFD_CFG_ENV_NAME, cfg_path);
    ret = ufd_parse_json(&ctx->init_params, cfg_path);
  } else {
    /* fallback path */
    ret = ufd_parse_json(&ctx->init_params, "ufd.json");
  }

  return ret;
}

static struct ufd_mt_ctx* ufd_create_mt_ctx(void) {
  struct ufd_mt_ctx* ctx = mt_zmalloc(sizeof(*ctx));
  struct mufd_override_params* rt_para = g_rt_para;
  struct mufd_init_params* init_para = g_init_para;
  int ret;

  if (!ctx) { /* create a new ctx */
    err("%s, malloc ctx mem fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_init(&ctx->slots_lock, NULL);

  /* init mtl context */
  struct mtl_init_params* p = &ctx->init_params.mt_params;
  p->flags |= MTL_FLAG_BIND_NUMA;    /* default bind to numa */
  p->log_level = MTL_LOG_LEVEL_INFO; /* default to info */

  if (init_para) { /* init case selected */
    info("%s, runtime config path\n", __func__);
    rte_memcpy(&ctx->init_params, init_para, sizeof(ctx->init_params));
  } else {
    /* get user config from json */
    int ret = ufd_config_init(ctx);
    if (ret < 0) {
      err("%s, ufd config init fail %d\n", __func__, ret);
      ufd_free_mt_ctx(ctx);
      return NULL;
    }

    /* override config if it runtime config */
    if (rt_para) {
      info("%s, applied override config\n", __func__);
      p->log_level = rt_para->log_level;
      if (rt_para->shared_tx_queue) p->flags |= MTL_FLAG_SHARED_TX_QUEUE;
      if (rt_para->shared_rx_queue) p->flags |= MTL_FLAG_SHARED_RX_QUEUE;
      if (rt_para->rss_mode) p->rss_mode = rt_para->rss_mode;
      if (rt_para->lcore_mode) p->flags |= MTL_FLAG_UDP_LCORE;
    }
  }

  /* assign a default if not set by user */
  if (!ctx->init_params.slots_nb_max) ctx->init_params.slots_nb_max = 1024;
  /* ufd is assigned to top of INT, the bottom fd is used by OS */
  if (!ctx->init_params.fd_base)
    ctx->init_params.fd_base = INT_MAX - ctx->init_params.slots_nb_max * 2;
  if (!ctx->init_params.txq_bps) ctx->init_params.txq_bps = MUDP_DEFAULT_RL_BPS;

  /* udp lcore and shared queue, set tasklets_nb_per_sch to allow max slots */
  if ((p->flags & (MTL_FLAG_SHARED_TX_QUEUE | MTL_FLAG_SHARED_RX_QUEUE)) &&
      (p->flags & MTL_FLAG_UDP_LCORE))
    p->tasklets_nb_per_sch = ctx->init_params.slots_nb_max + 8;

  ufd_set_afxdp(ctx);

  ctx->mt = mtl_init(p);
  if (!ctx->mt) {
    err("%s, mtl init fail\n", __func__);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }
  if (mtl_rss_mode_get(ctx->mt)) {
    ret = mtl_start(ctx->mt);
    if (ret < 0) {
      err("%s, mtl start fail\n", __func__);
      ufd_free_mt_ctx(ctx);
      return NULL;
    }
  }

  ctx->slots = mt_rte_zmalloc_socket(sizeof(*ctx->slots) * ufd_max_slot(ctx),
                                     mt_socket_id(ctx->mt, MTL_PORT_P));
  if (!ctx->slots) {
    err("%s, slots malloc fail\n", __func__);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }

  /* save ctx with dpdk rte memory which can be shared between process */
  struct ufd_mt_ctx* ctx_rte =
      mt_rte_zmalloc_socket(sizeof(*ctx_rte), mt_socket_id(ctx->mt, MTL_PORT_P));
  if (!ctx_rte) {
    err("%s, ctx_rte malloc fail\n", __func__);
    ufd_free_mt_ctx(ctx);
    return NULL;
  }
  mtl_memcpy(ctx_rte, ctx, sizeof(*ctx_rte));
  ctx_rte->alloc_with_rte = true;
  ctx_rte->parent_pid = getpid(); /* save the creator pid */
  mt_free(ctx);

  info("%s, succ, slots_nb_max %d\n", __func__, ufd_max_slot(ctx_rte));
  return ctx_rte;
}

static struct ufd_mt_ctx* ufd_get_mt_ctx(bool create) {
  struct ufd_mt_ctx* ctx = NULL;

  if (create) { /* require lock as get/create the mt ctx */
    ufd_mtl_ctx_lock();
    ctx = g_ufd_mt_ctx;
    if (!ctx) { /* create a new ctx */
      info("%s, start to create mt ctx\n", __func__);
      ctx = ufd_create_mt_ctx();
      g_ufd_mt_ctx = ctx;
    }
    ufd_mtl_ctx_unlock();
  } else {
    /* no lock need for pure global get */
    ctx = g_ufd_mt_ctx;
  }

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
  if (sockfd < ctx->init_params.fd_base) {
    err("%s, invalid sockfd %d, base is %d\n", __func__, sockfd,
        ctx->init_params.fd_base);
    return NULL;
  }
  int idx = ufd_fd2idx(ctx, sockfd);
  struct ufd_slot* slot = ctx->slots[idx];

  if (!slot) err("%s, invalid sockfd %d\n", __func__, sockfd);
  return slot;
}

int mufd_socket_port(int domain, int type, int protocol, enum mtl_port port) {
  int ret;
  struct ufd_mt_ctx* ctx;
  struct ufd_slot* slot = NULL;

  ret = mudp_verify_socket_args(domain, type, protocol);
  if (ret < 0) return ret;
  ctx = ufd_get_mt_ctx(true);
  if (!ctx) {
    err("%s, fail to get ufd mt ctx\n", __func__);
    MUDP_ERR_RET(EIO);
  }
  if (port >= ctx->init_params.mt_params.num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    MUDP_ERR_RET(EINVAL);
  }

  /* find one empty slot */
  mt_pthread_mutex_lock(&ctx->slots_lock);
  for (int i = 0; i < ufd_max_slot(ctx); i++) {
    if (ctx->slots[i]) continue;
    /* create a slot */
    slot = mt_rte_zmalloc_socket(sizeof(*slot), mt_socket_id(ctx->mt, port));
    if (!slot) {
      err("%s, slot malloc fail\n", __func__);
      mt_pthread_mutex_unlock(&ctx->slots_lock);
      MUDP_ERR_RET(ENOMEM);
    }
    slot->idx = i;
    ctx->slots[i] = slot;
    break;
  }
  mt_pthread_mutex_unlock(&ctx->slots_lock);

  if (!slot) {
    err("%s, all slot used, max allowed %d\n", __func__, ufd_max_slot(ctx));
    MUDP_ERR_RET(ENOMEM);
  }

  int idx = slot->idx;
  int fd = ufd_idx2fd(ctx, idx);
  /* update slot last idx */
  ctx->slot_last_idx = idx;

  slot->handle = mudp_socket_port(ctx->mt, domain, type, protocol, port);
  if (!slot->handle) {
    err("%s, socket create fail\n", __func__);
    ufd_free_slot(ctx, slot);
    MUDP_ERR_RET(ENOMEM);
  }

  mudp_set_tx_rate(slot->handle, ctx->init_params.txq_bps);
  if (ctx->init_params.rx_ring_count)
    mudp_set_rx_ring_count(slot->handle, ctx->init_params.rx_ring_count);
  if (ctx->init_params.wake_thresh_count)
    mudp_set_wake_thresh_count(slot->handle, ctx->init_params.wake_thresh_count);
  if (ctx->init_params.wake_timeout_us)
    mudp_set_wake_timeout(slot->handle, ctx->init_params.wake_timeout_us);
  /* allow to set zero to disable sleep */
  mudp_set_rx_poll_sleep(slot->handle, ctx->init_params.rx_poll_sleep_us);
  if (ctx->init_params.flags & MUFD_FLAG_BIND_ADDRESS_CHECK)
    mudp_bind_address_check(slot->handle, true);

  info("%s(%d), succ, fd %d\n", __func__, idx, fd);
  return fd;
}

int mufd_socket(int domain, int type, int protocol) {
  enum mtl_port port = MTL_PORT_P;
  /* port select from env */
  const char* port_u = getenv(MUFD_PORT_ENV_NAME);
  if (port_u) {
    port = atoi(port_u);
    dbg("%s, port_u %s port %d\n", __func__, port_u, port);
  }
  return mufd_socket_port(domain, type, protocol, port);
}

int mufd_close(int sockfd) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  int idx = ufd_fd2idx(ctx, sockfd);
  struct ufd_slot* slot = ufd_fd2slot(sockfd);

  if (!slot) {
    err("%s(%d), null slot for fd %d\n", __func__, idx, sockfd);
    MUDP_ERR_RET(EIO);
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

ssize_t mufd_sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_sendmsg(slot->handle, msg, flags);
}

int mufd_poll_query(struct pollfd* fds, nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv) {
  struct mudp_pollfd mfds[nfds];
  struct ufd_slot* slot;

  for (nfds_t i = 0; i < nfds; i++) {
    dbg("%s, fd %d\n", __func__, fds[i].fd);
    slot = ufd_fd2slot(fds[i].fd);
    mfds[i].fd = slot->handle;
    mfds[i].events = fds[i].events;
  }

  int ret = mudp_poll_query(mfds, nfds, timeout, query, priv);
  for (nfds_t i = 0; i < nfds; i++) {
    fds[i].revents = mfds[i].revents;
  }
  return ret;
}

int mufd_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  return mufd_poll_query(fds, nfds, timeout, NULL, NULL);
}

ssize_t mufd_recvfrom(int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_recvfrom(slot->handle, buf, len, flags, src_addr, addrlen);
}

ssize_t mufd_recvmsg(int sockfd, struct msghdr* msg, int flags) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_recvmsg(slot->handle, msg, flags);
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

int mufd_fcntl(int sockfd, int cmd, va_list args) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  int idx = slot->idx;
  MTL_MAY_UNUSED(args);

#ifdef WINDOWSENV
  err("%s(%d), invalid cmd %d, not support on windows\n", __func__, idx, cmd);
  MUDP_ERR_RET(EINVAL);
#else
  if (cmd != F_SETFD) {
    err("%s(%d), invalid cmd %d\n", __func__, idx, cmd);
    MUDP_ERR_RET(EINVAL);
  }

  dbg("%s(%d), cmd %d\n", __func__, idx, cmd);
  return 0;
#endif
}

int mufd_ioctl(int sockfd, unsigned long cmd, va_list args) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_ioctl(slot->handle, cmd, args);
}

int mufd_cleanup(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (ctx) {
    pid_t pid = getpid();
    if (pid == ctx->parent_pid) {
      ufd_free_mt_ctx(ctx);
    } else {
      info("%s, skip the mt ctx free as it is child process\n", __func__);
    }
    ufd_clear_mt_ctx();
  }

  struct mufd_override_params* rt_para = g_rt_para;
  if (rt_para) {
    mt_free(rt_para);
    g_rt_para = NULL;
  }

  struct mufd_init_params* init_para = g_init_para;
  if (init_para) {
    mt_free(init_para);
    g_init_para = NULL;
  }

  return 0;
}

/* lib constructors to init the resources */
RTE_INIT_PRIO(mufd_init_global, BUS) {
  ufd_init_global();
  dbg("%s, succ\n", __func__);
}

/* lib destructor to cleanup the resources */
RTE_FINI_PRIO(mufd_finish_global, BUS) {
  mufd_cleanup();
  ufd_uinit_global();
  dbg("%s, succ\n", __func__);
}

int mufd_abort(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (ctx) mtl_abort(ctx->mt);
  return 0;
}

int mufd_set_tx_mac(int sockfd, uint8_t mac[MTL_MAC_ADDR_LEN]) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_set_tx_mac(slot->handle, mac);
}

int mufd_set_tx_rate(int sockfd, uint64_t bps) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_set_tx_rate(slot->handle, bps);
}

uint64_t mufd_get_tx_rate(int sockfd) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_get_tx_rate(slot->handle);
}

int mufd_commit_override_params(struct mufd_override_params* p) {
  if (g_rt_para) {
    err("%s, already committed\n", __func__);
    MUDP_ERR_RET(EIO);
  }

  struct mufd_override_params* out = mt_zmalloc(sizeof(*out));
  if (!out) {
    err("%s, malloc out fail\n", __func__);
    MUDP_ERR_RET(ENOMEM);
  }
  rte_memcpy(out, p, sizeof(*p));
  g_rt_para = out;
  info("%s, succ\n", __func__);
  return 0;
}

int mufd_commit_init_params(struct mufd_init_params* p) {
  if (g_init_para) {
    err("%s, already committed\n", __func__);
    MUDP_ERR_RET(EIO);
  }

  struct mufd_init_params* out = mt_zmalloc(sizeof(*out));
  if (!out) {
    err("%s, malloc out fail\n", __func__);
    MUDP_ERR_RET(ENOMEM);
  }
  rte_memcpy(out, p, sizeof(*p));
  g_init_para = out;
  info("%s, succ\n", __func__);
  return 0;
}

int mufd_get_sessions_max_nb(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(true);
  if (!ctx) {
    err("%s, fail to get ufd mt ctx\n", __func__);
    MUDP_ERR_RET(EIO);
  }

  return ufd_max_slot(ctx);
}

int mufd_init_context(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(true);
  if (!ctx) MUDP_ERR_RET(EIO);
  return 0;
}

int mufd_base_fd(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(true);
  if (!ctx) MUDP_ERR_RET(EIO);
  return ctx->init_params.fd_base;
}

enum mtl_log_level mufd_log_level(void) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(true);
  if (!ctx) return MTL_LOG_LEVEL_INFO;
  return ctx->init_params.mt_params.log_level;
}

void* mufd_hp_malloc(size_t size, enum mtl_port port) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (!ctx) {
    err("%s, ctx get fail\n", __func__);
    return NULL;
  }

  return mtl_hp_malloc(ctx->mt, size, port);
}

void* mufd_hp_zmalloc(size_t size, enum mtl_port port) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (!ctx) {
    err("%s, ctx get fail\n", __func__);
    return NULL;
  }

  return mtl_hp_zmalloc(ctx->mt, size, port);
}

void mufd_hp_free(void* ptr) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (!ctx) {
    err("%s, ctx get fail\n", __func__);
    return;
  }

  return mtl_hp_free(ctx->mt, ptr);
}

int mufd_set_opaque(int sockfd, void* pri) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  int idx = slot->idx;

  if (slot->opaque) {
    err("%s(%d), opaque set already\n", __func__, idx);
    MUDP_ERR_RET(EIO);
  }

  slot->opaque = pri;
  return 0;
}

void* mufd_get_opaque(int sockfd) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return slot->opaque;
}

int mufd_get_sip(int sockfd, uint8_t ip[MTL_IP_ADDR_LEN]) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_get_sip(slot->handle, ip);
}

int mufd_tx_valid_ip(int sockfd, uint8_t dip[MTL_IP_ADDR_LEN]) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_tx_valid_ip(slot->handle, dip);
}

int mufd_register_stat_dump_cb(int sockfd, int (*dump)(void* priv), void* priv) {
  struct ufd_slot* slot = ufd_fd2slot(sockfd);
  return mudp_register_stat_dump_cb(slot->handle, dump, priv);
}

int mufd_socket_check(int domain, int type, int protocol) {
  return mudp_verify_socket_args(domain, type, protocol);
}

int mufd_port_ip_info(enum mtl_port port, uint8_t ip[MTL_IP_ADDR_LEN],
                      uint8_t netmask[MTL_IP_ADDR_LEN],
                      uint8_t gateway[MTL_IP_ADDR_LEN]) {
  struct ufd_mt_ctx* ctx = ufd_get_mt_ctx(false);
  if (!ctx) {
    err("%s, ctx get fail\n", __func__);
    return -EIO;
  }
  return mtl_port_ip_info(ctx->mt, port, ip, netmask, gateway);
}
