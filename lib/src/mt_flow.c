/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mt_flow.h"

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
      rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
    } else {
      rte_memcpy(&ipv4_spec.hdr.src_addr, flow->dip_addr, MTL_IP_ADDR_LEN);
      rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->sip_addr, MTL_IP_ADDR_LEN);
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

  mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);

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
    /* WA to avoid iavf_flow_create fail in 1000+ mudp close at same time */
    if (inf->drv_info.drv_type == MT_DRV_IAVF) mt_sleep_ms(5);
  }

  return rsp;
}

static int rx_flow_free(struct mt_interface* inf, struct mt_rx_flow_rsp* rsp) {
  enum mtl_port port = inf->port;
  struct rte_flow_error error;
  int ret;
  int max_retry = 5;
  int retry = 0;

retry:
  if (rsp->flow_id > 0) {
    mt_socket_remove_flow(inf->parent, port, rsp->flow_id, rsp->dst_port);
    rsp->flow_id = -1;
  }
  if (rsp->flow) {
    mt_pthread_mutex_lock(&inf->vf_cmd_mutex);
    ret = rte_flow_destroy(inf->port_id, rsp->flow, &error);
    mt_pthread_mutex_unlock(&inf->vf_cmd_mutex);
    if (ret < 0) {
      err("%s(%d), flow destroy fail, queue %d, retry %d\n", __func__, port,
          rsp->queue_id, retry);
      retry++;
      if (retry < max_retry) {
        mt_sleep_ms(10); /* WA: to wait pf finish the vf request */
        goto retry;
      }
    }
    rsp->flow = NULL;
  }
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
  /* no lock need */
  return rx_flow_free(inf, rsp);
}

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
    impl->flow[i] = flow;
  }

  return 0;
}
