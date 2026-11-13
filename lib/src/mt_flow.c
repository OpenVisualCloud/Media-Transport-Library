/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_flow.h"

/* iavf VF reset callbacks, rte_pmd_iavf_register_pre/post_reset_cb(): DPDK 26.03 */
#if !defined(WINDOWSENV) && defined(__has_include)
#if __has_include(<rte_pmd_iavf.h>) && RTE_VERSION >= RTE_VERSION_NUM(26, 3, 0, 0)
#define MT_FLOW_IAVF_RESET_CB
#include <dlfcn.h>
#include <link.h>
#include <rte_pmd_iavf.h>
#endif
#endif

#include "mt_log.h"
#include "mt_socket.h"
#include "mt_util.h"

static inline void rx_flow_lock(struct mt_flow_impl* flow) {
  mt_pthread_mutex_lock(&flow->mutex);
}

static inline void rx_flow_unlock(struct mt_flow_impl* flow) {
  mt_pthread_mutex_unlock(&flow->mutex);
}

static struct rte_flow* rte_rx_flow_create_raw(struct mt_interface* inf, uint16_t q,
                                               struct mt_rxq_flow* flow) {
  struct rte_flow_error error;
  struct rte_flow* r_flow;

  struct rte_flow_attr attr = {0};
  struct rte_flow_item pattern[2];
  struct rte_flow_action action[2];
  struct rte_flow_item_raw spec = {0};
  struct rte_flow_item_raw mask = {0};
  struct rte_flow_action_queue to_queue = {0};

  uint16_t port_id = inf->port_id;
  char pkt_buf[] =
      "0000000000010000000000020800450000300000000000110000010101010202020200001B3A001C00"
      "008000000000000000000000000000000000000000";
  char msk_buf[] =
      "000000000000000000000000000000000000000000000000000000000000000000000000FFFF000000"
      "000000000000000000000000000000000000000000";
  MTL_MAY_UNUSED(flow);

  attr.ingress = 1;

  memset(&error, 0, sizeof(error));
  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));

  spec.pattern = (const void*)pkt_buf;
  spec.length = 62;
  mask.pattern = (const void*)msk_buf;
  mask.length = 62;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_RAW;
  pattern[0].spec = &spec;
  pattern[0].mask = &mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  to_queue.index = q;
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &to_queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port_id, q,
        mt_string_safe(error.message));
    return NULL;
  }

  info("%s(%d), queue %u succ\n", __func__, inf->port, q);
  return r_flow;
}

static struct rte_flow* rte_rx_flow_create(struct mt_interface* inf, uint16_t q,
                                           struct mt_rxq_flow* flow) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[4];
  struct rte_flow_action action[2];
  struct rte_flow_action_queue queue;
  struct rte_flow_item_eth eth_spec;
  struct rte_flow_item_eth eth_mask;
  struct rte_flow_item_ipv4 ipv4_spec;
  struct rte_flow_item_ipv4 ipv4_mask;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_item_udp udp_mask;
  struct rte_flow_error error;
  struct rte_flow* r_flow;
  int ret;
  bool has_ip_flow = true;
  bool has_port_flow = true;

  uint16_t port_id = inf->port_id;
  enum mtl_port port = inf->port;

  memset(&error, 0, sizeof(error));

  /* drv not support ip flow */
  if (inf->drv_info.flow_type == MT_FLOW_NO_IP) has_ip_flow = false;
  /* no ip flow requested */
  if (flow->flags & MT_RXQ_FLOW_F_NO_IP) has_ip_flow = false;
  /* no port flow requested */
  if (flow->flags & MT_RXQ_FLOW_F_NO_PORT) has_port_flow = false;

  if (mt_get_user_params(inf->parent)->flags & MTL_FLAG_RX_UDP_PORT_ONLY) {
    if (has_ip_flow) {
      info("%s(%d), no ip flow as MTL_FLAG_RX_UDP_PORT_ONLY is set\n", __func__, port);
      has_ip_flow = false;
    }
  }

  /* only raw flow can be applied on the hdr split queue */
  if (mt_if_hdr_split_pool(inf, q)) {
    return rte_rx_flow_create_raw(inf, q, flow);
  }

  /* queue */
  queue.index = q;

  /* nothing for eth flow */
  memset(&eth_spec, 0, sizeof(eth_spec));
  memset(&eth_mask, 0, sizeof(eth_mask));

  /* ipv4 flow */
  memset(&ipv4_spec, 0, sizeof(ipv4_spec));
  memset(&ipv4_mask, 0, sizeof(ipv4_mask));
  ipv4_spec.hdr.next_proto_id = IPPROTO_UDP;

  if (has_ip_flow) {
    memset(&ipv4_mask.hdr.dst_addr, 0xFF, MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow->dip_addr)) {
      mt_memcpy(&ipv4_spec.hdr.dst_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
    } else {
      mt_memcpy(&ipv4_spec.hdr.src_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
      mt_memcpy(&ipv4_spec.hdr.dst_addr, flow->sip_addr, MTL_IP_ADDR_LEN);
      memset(&ipv4_mask.hdr.src_addr, 0xFF, MTL_IP_ADDR_LEN);
    }
  }

  /* udp port flow */
  if (has_port_flow) {
    memset(&udp_spec, 0, sizeof(udp_spec));
    memset(&udp_mask, 0, sizeof(udp_mask));
    udp_spec.hdr.dst_port = htons(flow->dst_port);
    udp_mask.hdr.dst_port = htons(0xFFFF);
  }

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;

  memset(action, 0, sizeof(action));
  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  memset(pattern, 0, sizeof(pattern));
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[0].spec = has_ip_flow ? &eth_spec : NULL;
  pattern[0].mask = has_ip_flow ? &eth_mask : NULL;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[1].spec = &ipv4_spec;
  pattern[1].mask = &ipv4_mask;
  if (has_port_flow) {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;
  } else {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_END;
  }

  ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
  if (ret < 0) {
    err("%s(%d), rte_flow_validate fail %d for queue %d, %s\n", __func__, port, ret, q,
        mt_string_safe(error.message));
    return NULL;
  }

  const int max_retry = 5;

  for (int retry = 0; retry < max_retry;) {
    mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
    r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
    if (r_flow) break;
    err("%s(%d), rte_flow_create fail for queue %d, retry %d, %s\n", __func__, port, q,
        retry, mt_string_safe(error.message));
    retry++;
    if (retry < max_retry) mt_sleep_ms(10); /* WA: to wait pf finish the vf request */
  }

  /* WA specific for e810 for PF interfaces */
  if (!has_ip_flow && !r_flow) {
    info("%s(%d), Flow creation failed on default group, retrying with group 2\n",
         __func__, port);
    attr.group = 2;

    ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
    if (ret < 0) {
      err("%s(%d), rte_flow_validate fail %d for queue %d, %s\n", __func__, port, ret, q,
          mt_string_safe(error.message));
      return NULL;
    }

    mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
    r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
  }

  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port, q,
        mt_string_safe(error.message));
    return NULL;
  }

  if (has_ip_flow) {
    uint8_t* ip = flow->dip_addr;
    info("%s(%d), queue %u succ, ip %u.%u.%u.%u port %u\n", __func__, port, q, ip[0],
         ip[1], ip[2], ip[3], flow->dst_port);
  } else {
    info("%s(%d), queue %u succ, port %u\n", __func__, port, q, flow->dst_port);
  }
  return r_flow;
}

static struct mt_rx_flow_rsp* rx_flow_create(struct mt_interface* inf, uint16_t q,
                                             struct mt_rxq_flow* flow) {
  int ret;
  enum mtl_port port = inf->port;
  struct mtl_main_impl* impl = inf->parent;
  uint8_t* ip = flow->dip_addr;

  if (!mt_drv_kernel_based(impl, port) && q >= inf->nb_rx_q) {
    err("%s(%d), invalid q %u\n", __func__, port, q);
    return NULL;
  }

  struct mt_rx_flow_rsp* rsp = mt_rte_zmalloc_socket(sizeof(*rsp), inf->socket_id);
  rsp->flow_id = -1;
  rsp->queue_id = q;
  rsp->dst_port = flow->dst_port;

  /* no flow if MT_DRV_F_RX_NO_FLOW */
  if (inf->drv_info.flags & MT_DRV_F_RX_NO_FLOW) return rsp;

  if (mt_drv_use_kernel_ctl(impl, port)) {
    ret = mt_socket_add_flow(impl, port, q, flow);
    if (ret < 0) {
      err("%s(%d), socket add flow fail for queue %d\n", __func__, port, q);
      mt_rte_free(rsp);
      return NULL;
    }
    rsp->flow_id = ret;
  } else {
    struct rte_flow* r_flow;

    r_flow = rte_rx_flow_create(inf, q, flow);
    if (!r_flow) {
      err("%s(%d), create flow fail for queue %d, ip %u.%u.%u.%u port %u\n", __func__,
          port, q, ip[0], ip[1], ip[2], ip[3], flow->dst_port);
      mt_rte_free(rsp);
      return NULL;
    }

    rsp->flow = r_flow;
    rsp->req = *flow;
    rsp->listed = true;
    MT_TAILQ_INSERT_TAIL(&impl->flow[port]->rte_flows, rsp, next);
    /* WA to avoid iavf_flow_create fail in 1000+ mudp close at same time */
    if (inf->drv_info.drv_type == MT_DRV_IAVF) mt_sleep_ms(5);
  }

  return rsp;
}

static int rx_flow_free(struct mt_interface* inf, struct mt_rx_flow_rsp* rsp) {
  enum mtl_port port = inf->port;
  struct rte_flow_error error;
  int ret;
  const int max_retry = 5;

  if (rsp->flow_id > 0) {
    mt_socket_remove_flow(inf->parent, port, rsp->flow_id, rsp->dst_port);
    rsp->flow_id = -1;
  }
  struct mt_flow_impl* flow_impl = rsp->listed ? inf->parent->flow[port] : NULL;
  if (flow_impl) {
    /* not during an iavf VF reset, which frees and replaces rsp->flow */
    rx_flow_lock(flow_impl);
    MT_TAILQ_REMOVE(&flow_impl->rte_flows, rsp, next);
    rsp->listed = false;
  }
  if (rsp->flow) {
    for (int retry = 0; retry < max_retry;) {
      mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
      ret = rte_flow_destroy(inf->port_id, rsp->flow, &error);
      mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
      if (ret >= 0) break;
      err("%s(%d), flow destroy fail, queue %d, retry %d\n", __func__, port,
          rsp->queue_id, retry);
      retry++;
      if (retry < max_retry) mt_sleep_ms(10); /* WA: to wait pf finish the vf request */
    }
    rsp->flow = NULL;
  }
  if (flow_impl) rx_flow_unlock(flow_impl);
  mt_rte_free(rsp);
  /* WA to avoid iavf_flow_destroy fail in 1000+ mudp close at same time */
  if (inf->drv_info.drv_type == MT_DRV_IAVF) mt_sleep_ms(1);
  return 0;
}

struct mt_rx_flow_rsp* mt_rx_flow_create(struct mtl_main_impl* impl, enum mtl_port port,
                                         uint16_t q, struct mt_rxq_flow* flow) {
  struct mt_interface* inf = mt_if(impl, port);
  struct mt_rx_flow_rsp* rsp;
  struct mt_flow_impl* flow_impl = impl->flow[port];

  if ((!mt_drv_kernel_based(impl, port) && q >= inf->nb_rx_q)) {
    err("%s(%d), invalid q %u max allowed %u\n", __func__, port, q, inf->nb_rx_q);
    return NULL;
  }

  rx_flow_lock(flow_impl);
  rsp = rx_flow_create(inf, q, flow);
  rx_flow_unlock(flow_impl);

  return rsp;
}

int mt_rx_flow_free(struct mtl_main_impl* impl, enum mtl_port port,
                    struct mt_rx_flow_rsp* rsp) {
  struct mt_interface* inf = mt_if(impl, port);
  return rx_flow_free(inf, rsp);
}

#ifdef MT_FLOW_IAVF_RESET_CB
/* iavf refuses a callback change while a VF resets (-EBUSY), which can take ~50 s */
#ifndef MT_FLOW_IAVF_CB_TRIES
#define MT_FLOW_IAVF_CB_TRIES (600)
#endif
#ifndef MT_FLOW_IAVF_CB_RETRY_MS
#define MT_FLOW_IAVF_CB_RETRY_MS (100)
#endif

typedef int (*flow_iavf_reg_pre_fn)(uint16_t port, iavf_pre_reset_cb_t cb, void* arg);
typedef int (*flow_iavf_reg_post_fn)(uint16_t port, iavf_post_reset_cb_t cb, void* arg);
/* the PMD's register functions, the same for every port */
static flow_iavf_reg_pre_fn flow_iavf_reg_pre;
static flow_iavf_reg_post_fn flow_iavf_reg_post;

static int flow_iavf_find(struct dl_phdr_info* info, size_t size, void* data) {
  MTL_MAY_UNUSED(size);
  if (!info->dlpi_name || !strstr(info->dlpi_name, "librte_net_iavf.so")) return 0;
  *(const char**)data = info->dlpi_name; /* valid while the plugin is loaded */
  return 1;
}

/* the iavf PMD is a DPDK plugin that MTL does not link: look in the loaded object */
static bool flow_iavf_resolve(void) {
  const char* path = NULL;
  void *pre, *post, *handle;

  if (flow_iavf_reg_pre && flow_iavf_reg_post) return true;
  pre = dlsym(RTLD_DEFAULT, "rte_pmd_iavf_register_pre_reset_cb");
  post = dlsym(RTLD_DEFAULT, "rte_pmd_iavf_register_post_reset_cb");
  if (!pre || !post) {
    /* dlopen() not in the callback: dl_iterate_phdr() holds a loader lock */
    dl_iterate_phdr(flow_iavf_find, &path);
    handle = path ? dlopen(path, RTLD_NOW | RTLD_NOLOAD) : NULL;
    if (!handle) return false;
    pre = dlsym(handle, "rte_pmd_iavf_register_pre_reset_cb");
    post = dlsym(handle, "rte_pmd_iavf_register_post_reset_cb");
    dlclose(handle); /* drops the reference RTLD_NOLOAD took */
  }
  if (!pre || !post) return false;
  flow_iavf_reg_pre = (flow_iavf_reg_pre_fn)pre;
  flow_iavf_reg_post = (flow_iavf_reg_post_fn)post;
  return true;
}

static int flow_iavf_set_pre(uint16_t port_id, iavf_pre_reset_cb_t cb, void* arg) {
  int ret = flow_iavf_reg_pre(port_id, cb, arg);

  if (ret == -EBUSY)
    warn("%s(%u), VF resetting, waiting to change the pre reset cb\n", __func__, port_id);
  for (int i = 1; i < MT_FLOW_IAVF_CB_TRIES && ret == -EBUSY; i++) {
    mt_sleep_ms(MT_FLOW_IAVF_CB_RETRY_MS);
    ret = flow_iavf_reg_pre(port_id, cb, arg);
  }
  return ret;
}

static int flow_iavf_set_post(uint16_t port_id, iavf_post_reset_cb_t cb, void* arg) {
  int ret = flow_iavf_reg_post(port_id, cb, arg);

  if (ret == -EBUSY)
    warn("%s(%u), VF resetting, waiting to change the post reset cb\n", __func__,
         port_id);
  for (int i = 1; i < MT_FLOW_IAVF_CB_TRIES && ret == -EBUSY; i++) {
    mt_sleep_ms(MT_FLOW_IAVF_CB_RETRY_MS);
    ret = flow_iavf_reg_post(port_id, cb, arg);
  }
  return ret;
}

/* on the iavf event thread, before the reset frees the port's rte_flows */
static void flow_iavf_pre_reset(uint16_t port_id, void* arg) {
  struct mt_flow_impl* flow_impl = arg;

  MTL_MAY_UNUSED(port_id);
  if (!flow_impl) return; /* iavf sets the cb and its arg without a lock */
  /* held until flow_iavf_post_reset(): no flow create or destroy during the reset */
  rx_flow_lock(flow_impl);
  flow_impl->in_reset = true;
  warn("%s(%d), VF reset, RX flows lost until it is done\n", __func__, flow_impl->port);
}

/* on the same thread after the reset, also without a pre call (registered after this one,
 * unregistered before it) */
static void flow_iavf_post_reset(uint16_t port_id, int reset_state, void* arg) {
  struct mt_flow_impl* flow_impl = arg;
  struct mt_interface* inf;
  struct mt_rx_flow_rsp* rsp;
  int created = 0, lost = 0;

  MTL_MAY_UNUSED(port_id);
  if (!flow_impl) return; /* iavf sets the cb and its arg without a lock */
  inf = mt_if(flow_impl->parent, flow_impl->port);
  if (!flow_impl->in_reset) rx_flow_lock(flow_impl);
  flow_impl->in_reset = false;
  MT_TAILQ_FOREACH(rsp, &flow_impl->rte_flows, next) {
    /* freed by the PMD; if the reset failed before that, by the port close */
    rsp->flow = NULL;
    if (!reset_state) {
      rsp->flow = rte_rx_flow_create(inf, rsp->queue_id, &rsp->req);
      /* WA to avoid iavf_flow_create fail, as rx_flow_create() */
      mt_sleep_ms(5);
    }
    if (rsp->flow)
      created++;
    else
      lost++;
  }
  rx_flow_unlock(flow_impl);
  if (lost)
    err("%s(%d), VF reset state %d, %d RX flows created again, %d lost\n", __func__,
        flow_impl->port, reset_state, created, lost);
  else
    warn("%s(%d), VF reset done, %d RX flows created again\n", __func__, flow_impl->port,
         created);
}

void mt_flow_reset_cb_register(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_flow_impl* flow_impl = impl->flow[port];
  uint16_t port_id = mt_if(impl, port)->port_id;
  int ret;

  if (!flow_iavf_resolve()) {
    warn("%s(%d), no iavf reset callbacks, a VF reset loses the RX flows\n", __func__,
         port);
    return;
  }
  /* post first: it is safe alone, pre alone would keep the flow mutex */
  ret = flow_iavf_set_post(port_id, flow_iavf_post_reset, flow_impl);
  if (ret < 0) {
    err("%s(%d), post reset cb fail %d, a VF reset loses the RX flows\n", __func__, port,
        ret);
    return;
  }
  flow_impl->reset_cb = true;
  ret = flow_iavf_set_pre(port_id, flow_iavf_pre_reset, flow_impl);
  if (ret < 0)
    err("%s(%d), pre reset cb fail %d, RX flow calls may race a VF reset\n", __func__,
        port, ret);
  info("%s(%d), RX flows are created again after a VF reset\n", __func__, port);
}

void mt_flow_reset_cb_unregister(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_flow_impl* flow_impl = impl->flow[port];
  uint16_t port_id = mt_if(impl, port)->port_id;
  int ret_pre, ret_post;

  if (!flow_impl || !flow_impl->reset_cb) return;
  /* pre first, as register does in reverse; each waits out a reset in progress */
  ret_pre = flow_iavf_set_pre(port_id, NULL, NULL);
  ret_post = flow_iavf_set_post(port_id, NULL, NULL);
  if (ret_pre < 0 || ret_post < 0)
    err("%s(%d), reset cb unregister fail %d %d, a VF reset now may use freed state\n",
        __func__, port, ret_pre, ret_post);
  flow_impl->reset_cb = false;
}
#else
void mt_flow_reset_cb_register(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
}

void mt_flow_reset_cb_unregister(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
}
#endif

int mt_flow_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_flow_impl* flow = impl->flow[i];
    if (!flow) continue;

    mt_pthread_mutex_destroy(&flow->mutex);
    mt_rte_free(flow);
    impl->flow[i] = NULL;
  }

  return 0;
}

int mt_flow_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_flow_impl* flow;

  for (int i = 0; i < num_ports; i++) {
    flow = mt_rte_zmalloc_socket(sizeof(*flow), mt_socket_id(impl, i));
    if (!flow) {
      err("%s(%d), flow malloc fail\n", __func__, i);
      mt_flow_uinit(impl);
      return -ENOMEM;
    }
    mt_pthread_mutex_init(&flow->mutex, NULL);
    flow->parent = impl;
    flow->port = i;
    MT_TAILQ_INIT(&flow->rte_flows);
    impl->flow[i] = flow;
  }

  return 0;
}
