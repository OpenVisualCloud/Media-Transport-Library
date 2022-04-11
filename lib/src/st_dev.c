/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "st_dev.h"

#include "st_arp.h"
#include "st_cni.h"
#include "st_dma.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_ptp.h"
#include "st_rx_ancillary_session.h"
#include "st_rx_audio_session.h"
#include "st_rx_video_session.h"
#include "st_sch.h"
#include "st_tx_ancillary_session.h"
#include "st_tx_audio_session.h"
#include "st_tx_video_session.h"
#include "st_util.h"

static int dev_reset_port(struct st_main_impl* impl, enum st_port port);

static void dev_eth_stat(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  uint16_t port_id;
  struct rte_eth_stats stats;

  for (int i = 0; i < num_ports; i++) {
    port_id = st_port_id(impl, i);

    if (!rte_eth_stats_get(port_id, &stats)) {
      rte_eth_stats_reset(port_id);

      uint64_t orate_m = stats.obytes * 8 / ST_DEV_STAT_INTERVAL_S / ST_DEV_STAT_M_UNIT;
      uint64_t irate_m = stats.ibytes * 8 / ST_DEV_STAT_INTERVAL_S / ST_DEV_STAT_M_UNIT;

      info("DEV(%d): Avr rate, tx: %" PRIu64 " Mb/s, rx: %" PRIu64
           " Mb/s, pkts, tx: %" PRIu64 ", rx: %" PRIu64 "\n",
           i, orate_m, irate_m, stats.opackets, stats.ipackets);
      if (stats.imissed || stats.ierrors || stats.oerrors || stats.rx_nombuf) {
        info("DEV(%d): Status: imissed %" PRIu64 " ierrors %" PRIu64 " oerrors %" PRIu64
             " rx_nombuf %" PRIu64 "\n",
             i, stats.imissed, stats.ierrors, stats.oerrors, stats.rx_nombuf);
      }
    }
  }
}

static void dev_stat(struct st_main_impl* impl) {
  struct st_init_params* p = st_get_user_params(impl);

  if (rte_atomic32_read(&impl->dev_in_reset)) return;

  info("* *    S T    D E V   S T A T E   * * \n");
  dev_eth_stat(impl);
  st_ptp_stat(impl);
  st_cni_stat(impl);
  st_tx_video_sessions_stat(impl);
  if (impl->tx_a_init) st_tx_audio_sessions_stat(impl);
  if (impl->tx_anc_init) st_tx_ancillary_sessions_stat(impl);
  st_rx_video_sessions_stat(impl);
  st_dma_stat(impl);
  if (impl->rx_a_init) st_rx_audio_sessions_stat(impl);
  if (impl->rx_anc_init) st_rx_ancillary_sessions_stat(impl);
  if (p->stat_dump_cb_fn) p->stat_dump_cb_fn(p->priv);
  info("* *    E N D    S T A T E   * * \n\n");
}

static void dev_stat_wakeup_thread(struct st_main_impl* impl) {
  pthread_mutex_lock(&impl->stat_wake_mutex);
  pthread_cond_signal(&impl->stat_wake_cond);
  pthread_mutex_unlock(&impl->stat_wake_mutex);
}

static void dev_stat_alarm_handler(void* param) {
  struct st_main_impl* impl = param;

  if (impl->stat_tid)
    dev_stat_wakeup_thread(impl);
  else
    dev_stat(impl);

  rte_eal_alarm_set(ST_DEV_STAT_INTERVAL_US(st_get_user_params(impl)->dump_period_s),
                    dev_stat_alarm_handler, impl);
}

static void* dev_stat_thread(void* arg) {
  struct st_main_impl* impl = arg;

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&impl->stat_stop) == 0) {
    pthread_mutex_lock(&impl->stat_wake_mutex);
    if (!rte_atomic32_read(&impl->stat_stop))
      pthread_cond_wait(&impl->stat_wake_cond, &impl->stat_wake_mutex);
    pthread_mutex_unlock(&impl->stat_wake_mutex);

    if (!rte_atomic32_read(&impl->stat_stop)) dev_stat(impl);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static int dev_eal_init(struct st_init_params* p) {
  char* argv[ST_EAL_MAX_ARGS];
  int argc, ret;
  int num_ports = p->num_ports;
  static bool eal_inited = false; /* eal cann't re-enter in one process */
  char port_params[ST_PORT_MAX][2 * ST_PORT_MAX_LEN];
  char* port_param;

  argc = 0;

  argv[argc] = ST_DPDK_LIB_NAME;
  argc++;
#ifndef WINDOWSENV
  argv[argc] = "--file-prefix";
  argc++;
  argv[argc] = ST_DPDK_LIB_NAME;
  argc++;
  argv[argc] = "--match-allocations";
  argc++;
#endif
  argv[argc] = "--in-memory";
  argc++;
  for (int i = 0; i < num_ports; i++) {
    argv[argc] = "-a";
    argc++;
    port_param = port_params[i];
    memset(port_param, 0, 2 * ST_PORT_MAX_LEN);
    snprintf(port_param, 2 * ST_PORT_MAX_LEN, "%s,max_burst_size=2048", p->port[i]);
    dbg("%s(%d), port_param: %s\n", __func__, i, port_param);
    argv[argc] = port_param;
    argc++;
  }

  /* amend dma dev port */
  dbg("%s, dma dev no %u\n", __func__, p->num_dma_dev_port);
  for (int i = 0; i < p->num_dma_dev_port; i++) {
    argv[argc] = "-a";
    argc++;
    argv[argc] = p->dma_dev_port[i];
    argc++;
  }

  if (p->lcores) {
    argv[argc] = "-l";
    argc++;
    info("%s, lcores: %s\n", __func__, p->lcores);
    argv[argc] = p->lcores;
    argc++;
  }

  argv[argc] = "--log-level";
  argc++;
  if (p->log_level == ST_LOG_LEVEL_DEBUG)
    argv[argc] = "user,debug";
  else if (p->log_level == ST_LOG_LEVEL_INFO)
    argv[argc] = "info";
  else if (p->log_level == ST_LOG_LEVEL_WARNING)
    argv[argc] = "warning";
  else if (p->log_level == ST_LOG_LEVEL_ERROR)
    argv[argc] = "error";
  else
    argv[argc] = "info";
  argc++;

  argv[argc] = "--";
  argc++;

  if (eal_inited) {
    info("%s, eal not support re-init\n", __func__);
    return -EIO;
  }
  ret = rte_eal_init(argc, argv);
  if (ret < 0) return ret;
  eal_inited = true;

  return 0;
}

static int dev_rx_runtime_queue_start(struct st_main_impl* impl, enum st_port port) {
  struct st_interface* inf = st_if(impl, port);
  int ret;
  struct st_rx_queue* rx_queue;

  for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) {
      ret = rte_eth_dev_rx_queue_start(inf->port_id, q);
      if (ret < 0)
        err("%s(%d), start runtime rx queue %d fail %d\n", __func__, port, q, ret);
    }
  }

  return 0;
}

/* flush all the old bufs in the rx queue already */
static int dev_flush_rx_queue(struct st_interface* inf, uint16_t queue) {
  uint16_t port_id = inf->port_id;
  int mbuf_size = 128;
  int loop = inf->nb_rx_desc / mbuf_size;
  struct rte_mbuf* mbuf[mbuf_size];
  uint16_t rv;

  for (int i = 0; i < loop; i++) {
    rv = rte_eth_rx_burst(port_id, queue, &mbuf[0], mbuf_size);
    if (rv) rte_pktmbuf_free_bulk(&mbuf[0], rv);
  }

  return 0;
}

#define ST_SHAPER_PROFILE_ID 1
#define ST_ROOT_NODE_ID 256
#define ST_DEFAULT_NODE_ID 246

static int dev_rl_init_root(struct st_main_impl* impl, enum st_port port,
                            uint32_t shaper_profile_id) {
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);
  int ret;
  struct rte_tm_error error;
  struct rte_tm_node_params np;

  if (inf->tx_rl_root_active) return 0;

  memset(&error, 0, sizeof(error));

  /* root node */
  memset(&np, 0, sizeof(np));
  np.shaper_profile_id = shaper_profile_id;
  np.nonleaf.n_sp_priorities = 1;
  ret = rte_tm_node_add(port_id, ST_ROOT_NODE_ID, -1, 0, 1, 0, &np, &error);
  if (ret < 0) {
    err("%s(%d), root add error: (%d)%s\n", __func__, port, ret, error.message);
    return ret;
  }

  /* nonleaf node based on root */
  ret =
      rte_tm_node_add(port_id, ST_DEFAULT_NODE_ID, ST_ROOT_NODE_ID, 0, 1, 1, &np, &error);
  if (ret < 0) {
    err("%s(%d), node add error: (%d)%s\n", __func__, port, ret, error.message);
    return ret;
  }

  inf->tx_rl_root_active = true;
  return 0;
}

static struct st_rl_shaper* dev_rl_shaper_add(struct st_main_impl* impl,
                                              enum st_port port, uint64_t bps) {
  struct st_rl_shaper* shapers = &st_if(impl, port)->tx_rl_shapers[0];
  uint16_t port_id = st_port_id(impl, port);
  int ret;
  struct rte_tm_error error;
  struct rte_tm_shaper_params sp;
  uint32_t shaper_profile_id;

  memset(&error, 0, sizeof(error));

  for (int i = 0; i < ST_MAX_RL_ITEMS; i++) {
    if (shapers[i].rl_bps) continue;

    shaper_profile_id = ST_SHAPER_PROFILE_ID + i;

    /* shaper profile with bandwidth */
    memset(&sp, 0, sizeof(sp));
    sp.peak.rate = bps;
    ret = rte_tm_shaper_profile_add(port_id, shaper_profile_id, &sp, &error);
    if (ret < 0) {
      err("%s(%d), shaper add error: (%d)%s\n", __func__, port, ret, error.message);
      return NULL;
    }

    ret = dev_rl_init_root(impl, port, shaper_profile_id);
    if (ret < 0) {
      err("%s(%d), root init error %d\n", __func__, port, ret);
      rte_tm_shaper_profile_delete(port_id, shaper_profile_id, &error);
      return NULL;
    }

    info("%s(%d), bps %" PRIu64 " on shaper %d\n", __func__, port, bps,
         shaper_profile_id);
    shapers[i].rl_bps = bps;
    shapers[i].shaper_profile_id = shaper_profile_id;
    shapers[i].idx = i;
    return &shapers[i];
  }

  err("%s(%d), no space\n", __func__, port);
  return NULL;
}

static struct st_rl_shaper* dev_rl_shaper_get(struct st_main_impl* impl,
                                              enum st_port port, uint64_t bps) {
  struct st_rl_shaper* shapers = &st_if(impl, port)->tx_rl_shapers[0];

  for (int i = 0; i < ST_MAX_RL_ITEMS; i++) {
    if (bps == shapers[i].rl_bps) return &shapers[i];
  }

  return dev_rl_shaper_add(impl, port, bps);
}

static int dev_init_ratelimit(struct st_main_impl* impl, enum st_port port) {
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);
  enum st_port_type port_type = st_port_type(impl, port);
  int ret;
  struct rte_tm_error error;
  struct rte_tm_node_params qp;
  struct st_rl_shaper* shaper;

  memset(&error, 0, sizeof(error));

  if (ST_PORT_VF == port_type && inf->tx_rl_root_active) {
    ret = dev_reset_port(impl, port);
    if (ret < 0) {
      err("%s(%d), VF dev_reset_port fail %d\n", __func__, port, ret);
      return ret;
    }
  }

  struct st_tx_queue* tx_queue;
  for (int q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];
    uint64_t bps = tx_queue->active ? tx_queue->bps : 0;

    if (!bps && (ST_PORT_VF == port_type))
      bps = 1024 * 1024 * 1024; /* VF require all q config with RL */
    if (!bps && !q)
      bps = 1024 * 1024 * 1024; /* trigger detect in case all tx not config with rl */

    /* delete old queue node */
    if (tx_queue->rl_shapers_mapping >= 0) {
      ret = rte_tm_node_delete(port_id, q, &error);
      if (ret < 0) {
        err("%s(%d), node %d delete fail %d(%s)\n", __func__, port, q, ret,
            error.message);
        return ret;
      }
      tx_queue->rl_shapers_mapping = -1;
    }

    if (bps) {
      shaper = dev_rl_shaper_get(impl, port, bps);
      if (!shaper) {
        err("%s(%d), rl shaper get fail for q %d\n", __func__, port, q);
        return -EIO;
      }
      memset(&qp, 0, sizeof(qp));
      qp.shaper_profile_id = shaper->shaper_profile_id;
      qp.leaf.cman = RTE_TM_CMAN_TAIL_DROP;
      qp.leaf.wred.wred_profile_id = RTE_TM_WRED_PROFILE_ID_NONE;
      ret = rte_tm_node_add(port_id, q, ST_DEFAULT_NODE_ID, 0, 1, 2, &qp, &error);
      if (ret < 0) {
        err("%s(%d), q %d add fail %d(%s)\n", __func__, port, q, ret, error.message);
        return ret;
      }
      tx_queue->rl_shapers_mapping = shaper->idx;
      info("%s(%d), q %d link to shaper id %d\n", __func__, port, q,
           shaper->shaper_profile_id);
    }
  }

  /* TODO: VF requires to stop the port first */
  if (ST_PORT_VF == port_type) {
    st_cni_stop(impl);
    rte_eth_dev_stop(port_id);
  }

  ret = rte_tm_hierarchy_commit(port_id, 1, &error);
  if (ret < 0) err("%s(%d), commit error (%d)%s\n", __func__, port, ret, error.message);

  if (ST_PORT_VF == port_type) {
    rte_eth_dev_start(port_id);
    /* start runtime rx queue again */
    if (inf->feature & ST_IF_FEATURE_RUNTIME_RX_QUEUE)
      dev_rx_runtime_queue_start(impl, port);
    st_cni_start(impl);
  }

  dbg("%s(%d), succ\n", __func__, port);
  return ret;
}

static struct rte_flow* dev_rx_queue_create_flow(uint16_t port_id, uint16_t q,
                                                 struct st_rx_flow* flow) {
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

  /* queue */
  queue.index = q;

  /* nothing for eth flow */
  memset(&eth_spec, 0, sizeof(eth_spec));
  memset(&eth_mask, 0, sizeof(eth_mask));

  /* ipv4 flow */
  memset(&ipv4_spec, 0, sizeof(ipv4_spec));
  memset(&ipv4_mask, 0, sizeof(ipv4_mask));
  ipv4_spec.hdr.next_proto_id = IPPROTO_UDP;
  memset(&ipv4_mask.hdr.dst_addr, 0xFF, ST_IP_ADDR_LEN);
  if (st_is_multicast_ip(flow->dip_addr)) {
    rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->dip_addr, ST_IP_ADDR_LEN);
  } else {
    rte_memcpy(&ipv4_spec.hdr.src_addr, flow->dip_addr, ST_IP_ADDR_LEN);
    rte_memcpy(&ipv4_spec.hdr.dst_addr, flow->sip_addr, ST_IP_ADDR_LEN);
    memset(&ipv4_mask.hdr.src_addr, 0xFF, ST_IP_ADDR_LEN);
  }

  /* udp flow */
  if (flow->port_flow) {
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
  pattern[0].spec = &eth_spec;
  pattern[0].mask = &eth_mask;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[1].spec = &ipv4_spec;
  pattern[1].mask = &ipv4_mask;
  if (flow->port_flow) {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;
  } else {
    pattern[2].type = RTE_FLOW_ITEM_TYPE_END;
  }

  ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
  if (ret < 0) {
    err("%s(%d), rte_flow_validate fail %d for queue %d, %s\n", __func__, port_id, ret, q,
        error.message);
    return NULL;
  }

  r_flow = rte_flow_create(port_id, &attr, pattern, action, &error);
  if (!r_flow) {
    err("%s(%d), rte_flow_create fail for queue %d, %s\n", __func__, port_id, q,
        error.message);
    return NULL;
  }

  return r_flow;
}

static int dev_free_port(struct st_main_impl* impl, enum st_port port) {
  int ret;
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);

  if (!(inf->status & ST_IF_STAT_PORT_STARTED)) {
    info("%s(%d), port not started\n", __func__, port);
    return 0;
  }

  ret = rte_eth_dev_stop(port_id);
  if (ret < 0) err("%s(%d), rte_eth_dev_stop fail %d\n", __func__, port, ret);

  ret = rte_eth_dev_close(port_id);
  if (ret < 0) err("%s(%d), rte_eth_dev_close fail %d\n", __func__, port, ret);

  inf->status &= ~ST_IF_STAT_PORT_STARTED;
  inf->status &= ~ST_IF_STAT_PORT_CONFIGED;

  info("%s(%d), succ\n", __func__, port);
  return 0;
}

static int dev_detect_link(struct st_main_impl* impl, enum st_port port) {
  /* get link speed for the port */
  struct rte_eth_link eth_link;
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);

  memset(&eth_link, 0, sizeof(eth_link));

  for (int i = 0; i < 100; i++) {
    rte_eth_link_get_nowait(port_id, &eth_link);
    if (eth_link.link_status) {
      inf->link_speed = eth_link.link_speed;
      st_eth_link_dump(port_id);
      return 0;
    }
    st_sleep_ms(100); /* only happen on CVL PF */
  }

  st_eth_link_dump(port_id);
  err("%s(%d), link not connected for %s\n", __func__, port,
      st_get_user_params(impl)->port[port]);
  return -EIO;
}

static int dev_start_timesync(struct st_main_impl* impl, enum st_port port) {
  int ret, i = 0, max_retry = 10;
  uint16_t port_id = st_port_id(impl, port);
  struct timespec spec;

  for (i = 0; i < max_retry; i++) {
    ret = rte_eth_timesync_enable(port_id);
    if (ret < 0) {
      err("%s(%d), rte_eth_timesync_enable fail %d\n", __func__, port, ret);
      return ret;
    }

    memset(&spec, 0, sizeof(spec));
    ret = rte_eth_timesync_read_time(port_id, &spec);
    if (ret < 0) {
      err("%s(%d), rte_eth_timesync_read_time fail %d\n", __func__, port, ret);
      return ret;
    }
    if (spec.tv_sec || spec.tv_nsec) {
      /* read and print time */
      rte_eth_timesync_read_time(port_id, &spec);
      struct tm t;
      char date_time[64];
      localtime_r(&spec.tv_sec, &t);
      strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &t);
      info("%s(%d), init ptp time %s, i %d\n", __func__, port, date_time, i);
      break;
    }
    dbg("%s(%d), tv_sec %" PRIu64 " tv_nsec %" PRIu64 ", i %d\n", __func__, port,
        spec.tv_sec, spec.tv_nsec, i);
    st_sleep_ms(10);
  }
  if (i >= max_retry) {
    err("%s(%d), fail to get read time\n", __func__, port);
    return -EIO;
  }

  return 0;
}

static const struct rte_eth_conf dev_port_conf = {
    .txmode = {.offloads = DEV_TX_OFFLOAD_MULTI_SEGS}};

static const struct rte_eth_txconf dev_tx_port_conf = {.tx_rs_thresh = 1,
                                                       .tx_free_thresh = 1};
static const struct rte_eth_rxconf dev_rx_port_conf = {.rx_free_thresh = 1,
                                                       .rx_deferred_start = 0};

static int dev_config_port(struct st_main_impl* impl, enum st_port port) {
  uint16_t nb_rx_desc = ST_DEV_RX_DESC, nb_tx_desc = ST_DEV_TX_DESC;
  int ret;
  struct st_init_params* p = st_get_user_params(impl);
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);
  uint16_t nb_rx_q = inf->max_rx_queues, nb_tx_q = inf->max_tx_queues;
  struct rte_eth_conf port_conf = dev_port_conf;

  if (inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP)
    port_conf.rxmode.offloads |= DEV_RX_OFFLOAD_TIMESTAMP;

  ret = rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, &port_conf);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_configure fail %d\n", __func__, port, ret);
    return ret;
  }

  /* apply if user has rx_tx_desc config */
  if (p->nb_tx_desc) nb_tx_desc = p->nb_tx_desc;
  if (p->nb_rx_desc) nb_rx_desc = p->nb_rx_desc;

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rx_desc, &nb_tx_desc);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_adjust_nb_rx_tx_desc fail %d\n", __func__, port, ret);
    return ret;
  }
  inf->nb_tx_desc = nb_tx_desc;
  inf->nb_rx_desc = nb_rx_desc;

  /* enable PTYPE for packet classification by NIC */
  uint32_t ptypes[16];
  uint32_t set_ptypes[16];
  uint32_t ptype_mask = RTE_PTYPE_L2_ETHER_TIMESYNC | RTE_PTYPE_L2_ETHER_ARP |
                        RTE_PTYPE_L2_ETHER_VLAN | RTE_PTYPE_L2_ETHER_QINQ |
                        RTE_PTYPE_L4_ICMP | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP |
                        RTE_PTYPE_L4_FRAG;
  int num_ptypes =
      rte_eth_dev_get_supported_ptypes(port_id, ptype_mask, ptypes, RTE_DIM(ptypes));
  for (int i = 0; i < num_ptypes; i += 1) {
    set_ptypes[i] = ptypes[i];
  }
  if (num_ptypes >= 5) {
    ret = rte_eth_dev_set_ptypes(port_id, ptype_mask, set_ptypes, num_ptypes);
    if (ret < 0) {
      err("%s(%d), rte_eth_dev_set_ptypes fail %d\n", __func__, port, ret);
      return ret;
    }
  } else {
    err("%s(%d), failed to setup all ptype, only %d supported\n", __func__, port,
        num_ptypes);
    return -EIO;
  }

  inf->status |= ST_IF_STAT_PORT_CONFIGED;

  info("%s(%d), tx_q(%d with %d desc) rx_q (%d with %d desc)\n", __func__, port, nb_tx_q,
       nb_tx_desc, nb_rx_q, nb_rx_desc);
  return 0;
}

static int dev_start_port(struct st_main_impl* impl, enum st_port port) {
  int ret;
  uint16_t port_id = st_port_id(impl, port);
  int socket_id = rte_eth_dev_socket_id(port_id);
  struct st_interface* inf = st_if(impl, port);
  uint16_t nb_rx_q = inf->max_rx_queues, nb_tx_q = inf->max_tx_queues;
  uint16_t nb_rx_desc = inf->nb_rx_desc, nb_tx_desc = inf->nb_tx_desc;
  uint8_t rx_deferred_start = 0;
  struct rte_eth_txconf tx_port_conf = dev_tx_port_conf;
  struct rte_eth_rxconf rx_port_conf = dev_rx_port_conf;

  if (inf->feature & ST_IF_FEATURE_RUNTIME_RX_QUEUE) rx_deferred_start = 1;
  rx_port_conf.rx_deferred_start = rx_deferred_start;

  struct rte_mempool* mbuf_pool;
  for (uint16_t q = 0; q < nb_rx_q; q++) {
    mbuf_pool = inf->rx_queues[q].mbuf_pool ? inf->rx_queues[q].mbuf_pool
                                            : st_get_mempool(impl, port);
    ret = rte_eth_rx_queue_setup(port_id, q, nb_rx_desc, socket_id, &rx_port_conf,
                                 mbuf_pool);
    if (ret < 0) {
      err("%s(%d), rte_eth_rx_queue_setup fail %d for queue %d\n", __func__, port, ret,
          q);
      return ret;
    }
  }

  for (uint16_t q = 0; q < nb_tx_q; q++) {
    ret = rte_eth_tx_queue_setup(port_id, q, nb_tx_desc, socket_id, &tx_port_conf);
    if (ret < 0) {
      err("%s(%d), rte_eth_tx_queue_setup fail %d for queue %d\n", __func__, port, ret,
          q);
      return ret;
    }
  }

  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    err("%s(%d), rte_eth_dev_start fail %d\n", __func__, port, ret);
    return ret;
  }
  inf->status |= ST_IF_STAT_PORT_STARTED;

  if (st_get_user_params(impl)->flags & ST_FLAG_NIC_RX_PROMISCUOUS) {
    /* Enable RX in promiscuous mode if it's required. */
    err("%s(%d), enable promiscuous\n", __func__, port);
    rte_eth_promiscuous_enable(port_id);
  }
  rte_eth_stats_reset(port_id); /* reset stats */

  info("%s(%d), rx_defer %d\n", __func__, port, rx_deferred_start);
  return 0;
}

static int dev_reset_port(struct st_main_impl* impl, enum st_port port) {
  int ret;
  uint16_t port_id = st_port_id(impl, port);
  struct st_interface* inf = st_if(impl, port);
  struct rte_flow* flow;

  if (rte_atomic32_read(&impl->started)) {
    err("%s, only allowed when dev is in stop state\n", __func__);
    return -EIO;
  }

  rte_atomic32_set(&impl->dev_in_reset, 1);

  st_cni_stop(impl);

  rte_eth_dev_reset(port_id);

  ret = dev_config_port(impl, port);
  if (ret < 0) {
    err("%s(%d), dev_config_port fail %d\n", __func__, port, ret);
    rte_atomic32_set(&impl->dev_in_reset, 0);
    return ret;
  }

  ret = dev_start_port(impl, port);
  if (ret < 0) {
    err("%s(%d), dev_start_port fail %d\n", __func__, port, ret);
    rte_atomic32_set(&impl->dev_in_reset, 0);
    return ret;
  }

  st_cni_start(impl);

  /* clear rl status */
  for (int q = 0; q < inf->max_tx_queues; q++) {
    inf->tx_queues[q].rl_shapers_mapping = -1; /* init to invalid */
  }
  inf->tx_rl_root_active = false;
  struct st_rl_shaper* shapers = &inf->tx_rl_shapers[0];
  memset(shapers, 0, sizeof(*shapers) * ST_MAX_RL_ITEMS);

  /* restore rte flow */
  struct st_rx_queue* rx_queue;
  for (uint16_t rx_q = 0; rx_q < inf->max_rx_queues; rx_q++) {
    rx_queue = &inf->rx_queues[rx_q];
    flow = rx_queue->flow;
    if (flow) {
      flow = dev_rx_queue_create_flow(port_id, rx_q, &rx_queue->st_flow);
      if (!flow) {
        err("%s(%d), dev_rx_queue_create_flow fail for q %d\n", __func__, port, rx_q);
        rte_atomic32_set(&impl->dev_in_reset, 0);
        return -EIO;
      }
      rx_queue->flow = flow;
    }
  }
  /* restore mcast */
  st_mcast_restore(impl, port);

  return 0;
}

static int dev_filelock_lock(struct st_main_impl* impl) {
  int fd = -1;
  if (access(ST_FLOCK_PATH, F_OK) < 0) {
    fd = open(ST_FLOCK_PATH, O_RDONLY | O_CREAT, 0666);
  } else {
    fd = open(ST_FLOCK_PATH, O_RDONLY);
  }

  if (fd < 0) {
    err("%s, failed to open %s, %s\n", __func__, ST_FLOCK_PATH, strerror(errno));
    return -EIO;
  }
  impl->lcore_lock_fd = fd;
  /* wait until locked */
  if (flock(fd, LOCK_EX) != 0) {
    err("%s, can not lock file\n", __func__);
    close(fd);
    impl->lcore_lock_fd = -1;
    return -EIO;
  }

  return 0;
}

static int dev_filelock_unlock(struct st_main_impl* impl) {
  int fd = impl->lcore_lock_fd;

  if (fd < 0) {
    err("%s, wrong lock file fd %d\n", __func__, fd);
    return -EIO;
  }

  if (flock(fd, LOCK_UN) != 0) {
    err("%s, can not unlock file\n", __func__);
    return -EIO;
  }
  close(fd);
  impl->lcore_lock_fd = -1;
  return 0;
}

int st_dev_get_lcore(struct st_main_impl* impl, unsigned int* lcore) {
  unsigned int cur_lcore = 0;
  int ret;
  struct st_lcore_shm* lcore_shm = impl->lcore_shm;

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  do {
    cur_lcore = rte_get_next_lcore(cur_lcore, 1, 0);

    if ((cur_lcore < RTE_MAX_LCORE) &&
        rte_lcore_to_socket_id(cur_lcore) == st_socket_id(impl, ST_PORT_P)) {
      if (!lcore_shm->lcores_active[cur_lcore]) {
        *lcore = cur_lcore;
        lcore_shm->lcores_active[cur_lcore] = true;
        lcore_shm->used++;
        rte_atomic32_inc(&impl->lcore_cnt);
        impl->local_lcores_active[cur_lcore] = true;
        ret = dev_filelock_unlock(impl);
        info("%s, available lcore %d\n", __func__, cur_lcore);
        if (ret < 0) {
          err("%s, dev_filelock_unlock fail\n", __func__);
          return ret;
        }
        return 0;
      }
    }
  } while (cur_lcore < RTE_MAX_LCORE);

  dev_filelock_unlock(impl);
  err("%s, fail to find lcore\n", __func__);
  return -EIO;
}

int st_dev_put_lcore(struct st_main_impl* impl, unsigned int lcore) {
  int ret;
  struct st_lcore_shm* lcore_shm = impl->lcore_shm;

  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EIO;
  }
  if (!lcore_shm) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }
  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }
  if (!lcore_shm->lcores_active[lcore]) {
    err("%s, lcore %d not active\n", __func__, lcore);
    ret = -EIO;
    goto err_unlock;
  }

  lcore_shm->lcores_active[lcore] = false;
  lcore_shm->used--;
  rte_atomic32_dec(&impl->lcore_cnt);
  impl->local_lcores_active[lcore] = false;
  ret = dev_filelock_unlock(impl);
  info("%s, lcore %d\n", __func__, lcore);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

bool st_dev_lcore_valid(struct st_main_impl* impl, unsigned int lcore) {
  struct st_lcore_shm* lcore_shm = impl->lcore_shm;

  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EIO;
  }
  if (!lcore_shm) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }

  return lcore_shm->lcores_active[lcore];
}

static int dev_uinit_lcores(struct st_main_impl* impl) {
  int ret;
  int shm_id = impl->lcore_shm_id;
  struct st_lcore_shm* lcore_shm = impl->lcore_shm;
  if (!lcore_shm || shm_id == -1) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }

  for (unsigned int lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
    if (impl->local_lcores_active[lcore]) {
      warn("%s, lcore %d still active\n", __func__, lcore);
      st_dev_put_lcore(impl, lcore);
    }
  }

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  ret = shmdt(impl->lcore_shm);
  if (ret == -1) {
    err("%s, shared memory detach failed, %s\n", __func__, strerror(errno));
    goto err_unlock;
  }

  struct shmid_ds shmds;
  ret = shmctl(shm_id, IPC_STAT, &shmds);
  if (ret < 0) {
    err("%s, can not stat shared memory, %s\n", __func__, strerror(errno));
    goto err_unlock;
  }
  if (shmds.shm_nattch == 0) { /* remove ipc if we are the last user */
    ret = shmctl(shm_id, IPC_RMID, NULL);
    if (ret < 0) {
      warn("%s, can not remove shared memory, %s\n", __func__, strerror(errno));
      goto err_unlock;
    }
  }

  impl->lcore_shm_id = -1;
  impl->lcore_shm = NULL;
  ret = dev_filelock_unlock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

static int dev_init_lcores(struct st_main_impl* impl) {
  int ret;
  struct st_lcore_shm* lcore_shm;

  if (impl->lcore_shm) {
    err("%s, lcore_shm attached\n", __func__);
    return -EIO;
  }

  ret = dev_filelock_lock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_lock fail\n", __func__);
    return ret;
  }

  key_t key = ftok("/dev/null", 21);
  if (key < 0) {
    err("%s, ftok error: %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }
  int shm_id = shmget(key, sizeof(*lcore_shm), 0666 | IPC_CREAT);
  if (shm_id < 0) {
    err("%s, can not get shared memory for lcore, %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }
  impl->lcore_shm_id = shm_id;

  lcore_shm = shmat(shm_id, NULL, 0);
  if (lcore_shm == (void*)-1) {
    err("%s, can not attach shared memory for lcore, %s\n", __func__, strerror(errno));
    ret = -EIO;
    goto err_unlock;
  }

  struct shmid_ds stat;
  ret = shmctl(shm_id, IPC_STAT, &stat);
  if (ret < 0) {
    err("%s, shmctl fail\n", __func__);
    shmdt(lcore_shm);
    goto err_unlock;
  }
  if (stat.shm_nattch == 1) /* clear shm as we are the first user */
    memset(lcore_shm, 0, sizeof(*lcore_shm));

  impl->lcore_shm = lcore_shm;
  info("%s, shared memory attached at %p nattch %" PRIu64 "\n", __func__, impl->lcore_shm,
       stat.shm_nattch);
  ret = dev_filelock_unlock(impl);
  if (ret < 0) {
    err("%s, dev_filelock_unlock fail\n", __func__);
    return ret;
  }
  return 0;

err_unlock:
  dev_filelock_unlock(impl);
  return ret;
}

static int dev_if_free_rx_queue_flow(struct st_interface* inf,
                                     struct st_rx_queue* rx_queue) {
  int ret;
  struct rte_flow* flow;
  struct rte_flow_error error;

  flow = rx_queue->flow;
  if (flow) {
    ret = rte_flow_destroy(inf->port_id, flow, &error);
    if (ret < 0)
      err("%s(%d), rte_flow_destroy fail for queue %d\n", __func__, inf->port,
          rx_queue->queue_id);
    rx_queue->flow = NULL;
  }

  return 0;
}

static int dev_if_uinit_rx_queues(struct st_interface* inf) {
  enum st_port port = inf->port;
  struct st_rx_queue* rx_queue;
  struct rte_flow_error error;

  if (!inf->rx_queues) return 0;

  for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) {
      warn("%s(%d), rx queue %d still active\n", __func__, port, q);
    }
    if (rx_queue->flow) {
      dbg("%s(%d), rte flow %d still active\n", __func__, port, q);
      rte_flow_destroy(inf->port_id, rx_queue->flow, &error);
      rx_queue->flow = NULL;
    }
    if (rx_queue->mbuf_pool) {
      st_mempool_free(rx_queue->mbuf_pool);
      rx_queue->mbuf_pool = NULL;
    }
  }

  st_rte_free(inf->rx_queues);
  inf->rx_queues = NULL;

  return 0;
}

static int dev_if_init_rx_queues(struct st_main_impl* impl, struct st_interface* inf) {
  struct st_rx_queue* rx_queues =
      st_rte_zmalloc_socket(sizeof(*rx_queues) * inf->max_rx_queues, inf->socket_id);
  if (!rx_queues) {
    err("%s(%d), rx_queues not alloc\n", __func__, inf->port);
    return -ENOMEM;
  }

  struct st_init_params* p = st_get_user_params(impl);
  if (!(p->flags & ST_FLAG_RX_QUEUE_MONO_POOL)) {
    for (uint16_t q = 0; q < inf->max_rx_queues; q++) {
      rx_queues[q].queue_id = q;
      /* Creates mempool to hold the rx queue mbufs. */
      unsigned int mbuf_elements = inf->nb_rx_desc + 1024;
      char pool_name[ST_MAX_NAME_LEN];
      snprintf(pool_name, ST_MAX_NAME_LEN, "ST%d_RX%d_MBUF_POOL", inf->port, q);
      struct rte_mempool* mbuf_pool = NULL;
      if (q == 0)
        mbuf_pool = st_mempool_create_common(impl, inf->port, pool_name, mbuf_elements);
      else
        mbuf_pool = st_mempool_create(impl, inf->port, pool_name, mbuf_elements,
                                      ST_MBUF_CACHE_SIZE, sizeof(struct st_muf_priv_data),
                                      ST_PKT_MAX_ETHER_BYTES);
      if (!mbuf_pool) {
        dev_if_uinit_rx_queues(inf);
        return -ENOMEM;
      }
      rx_queues[q].mbuf_pool = mbuf_pool;
    }
  }
  inf->rx_queues = rx_queues;
  pthread_mutex_init(&inf->rx_queues_mutex, NULL);

  return 0;
}

static int dev_if_uinit_tx_queues(struct st_interface* inf) {
  enum st_port port = inf->port;
  struct st_tx_queue* tx_queue;

  if (!inf->tx_queues) return 0;

  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];
    if (tx_queue->active) {
      warn("%s(%d), tx_queuequeue %d still active\n", __func__, port, q);
    }
  }

  st_rte_free(inf->tx_queues);
  inf->tx_queues = NULL;

  return 0;
}

static int dev_if_init_tx_queues(struct st_main_impl* impl, struct st_interface* inf) {
  struct st_tx_queue* tx_queues =
      st_rte_zmalloc_socket(sizeof(*tx_queues) * inf->max_tx_queues, inf->socket_id);
  if (!tx_queues) {
    err("%s(%d), tx_queues not alloc\n", __func__, inf->port);
    return -ENOMEM;
  }

  for (uint16_t q = 0; q < inf->max_tx_queues; q++) {
    tx_queues[q].queue_id = q;
    tx_queues[q].rl_shapers_mapping = -1;
  }
  inf->tx_queues = tx_queues;
  pthread_mutex_init(&inf->tx_queues_mutex, NULL);

  return 0;
}

static uint64_t ptp_from_real_time(struct st_main_impl* impl, enum st_port port) {
  struct timespec spec;

  clock_gettime(CLOCK_REALTIME, &spec);
  return st_timespec_to_ns(&spec);
}

static uint64_t ptp_from_user(struct st_main_impl* impl, enum st_port port) {
  struct st_init_params* p = st_get_user_params(impl);

  return p->ptp_get_time_fn(p->priv);
}

static enum st_port_type _get_type_from_driver(const char* driver) {
  if (!strncmp(driver, "net_iavf", 10)) {
    return ST_PORT_VF;
  }

  if (!strncmp(driver, "net_ice", 10)) { /* cvl pf */
    return ST_PORT_PF;
  }

  if (!strncmp(driver, "net_i40e", 10)) { /* flv pf */
    return ST_PORT_PF;
  }

  err("%s, fail to parse for driver %s\n", __func__, driver);
  return ST_PORT_ERR;
}

int st_dev_request_tx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, uint64_t bytes_per_sec) {
  struct st_interface* inf = st_if(impl, port);
  uint16_t q;
  struct st_tx_queue* tx_queue;

  pthread_mutex_lock(&inf->tx_queues_mutex);
  for (q = 0; q < inf->max_tx_queues; q++) {
    tx_queue = &inf->tx_queues[q];
    if (!tx_queue->active) {
      tx_queue->active = true;
      tx_queue->bps = bytes_per_sec;
      *queue_id = q;
      pthread_mutex_unlock(&inf->tx_queues_mutex);
      info("%s(%d), q %d with speed %" PRIu64 "\n", __func__, port, q, bytes_per_sec);
      return 0;
    }
  }
  pthread_mutex_unlock(&inf->tx_queues_mutex);

  err("%s(%d), fail to find free tx queue\n", __func__, port);
  return -ENOMEM;
}

int st_dev_request_rx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, struct st_rx_flow* flow) {
  struct st_interface* inf = st_if(impl, port);
  uint16_t port_id = inf->port_id;
  uint16_t q;
  int ret;
  struct st_rx_queue* rx_queue;

  pthread_mutex_lock(&inf->rx_queues_mutex);
  for (q = 0; q < inf->max_rx_queues; q++) {
    rx_queue = &inf->rx_queues[q];
    if (rx_queue->active) continue;

    /* free the dummmy flow if any */
    dev_if_free_rx_queue_flow(inf, rx_queue);

    if (flow) {
      struct rte_flow* r_flow;

      r_flow = dev_rx_queue_create_flow(port_id, q, flow);
      if (!r_flow) {
        err("%s(%d), create flow fail for queue %d\n", __func__, port, q);
        pthread_mutex_unlock(&inf->rx_queues_mutex);
        return -EIO;
      }

      rx_queue->flow = r_flow;
      rx_queue->st_flow = *flow;
    }

    if (inf->feature & ST_IF_FEATURE_RUNTIME_RX_QUEUE) {
      ret = rte_eth_dev_rx_queue_start(inf->port_id, q);
      if (ret < 0) {
        err("%s(%d), start runtime rx queue %d fail %d\n", __func__, port, q, ret);
        pthread_mutex_unlock(&inf->rx_queues_mutex);
        return -EIO;
      }
    }
    rx_queue->active = true;
    pthread_mutex_unlock(&inf->rx_queues_mutex);

    dev_flush_rx_queue(inf, q);
    *queue_id = q;
    info("%s(%d), q %d\n", __func__, port, q);
    return 0;
  }
  pthread_mutex_unlock(&inf->rx_queues_mutex);

  err("%s(%d), fail to find free rx queue\n", __func__, port);
  return -ENOMEM;
}

int st_dev_flush_tx_queue(struct st_main_impl* impl, enum st_port port,
                          uint16_t queue_id) {
  struct st_interface* inf = st_if(impl, port);
  struct st_tx_queue* tx_queue;

  if (queue_id >= inf->max_tx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, queue_id);
    return -EIO;
  }

  tx_queue = &inf->tx_queues[queue_id];
  if (!tx_queue->active) {
    err("%s(%d), queue %d is not allocated\n", __func__, port, queue_id);
    return -EIO;
  }

  int burst_pkts = inf->nb_tx_desc;
  struct rte_mbuf* pads[1];
  pads[0] = inf->pad;

  for (int i = 0; i < burst_pkts; i++) {
    rte_mbuf_refcnt_update(inf->pad, 1);
    st_tx_burst_busy(inf->port_id, queue_id, &pads[0], 1);
  }
  return 0;
}

int st_dev_free_tx_queue(struct st_main_impl* impl, enum st_port port,
                         uint16_t queue_id) {
  struct st_interface* inf = st_if(impl, port);
  struct st_tx_queue* tx_queue;

  if (queue_id >= inf->max_tx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, queue_id);
    return -EIO;
  }

  tx_queue = &inf->tx_queues[queue_id];
  if (!tx_queue->active) {
    err("%s(%d), queue %d is not allocated\n", __func__, port, queue_id);
    return -EIO;
  }

  tx_queue->active = false;
  info("%s(%d), q %d\n", __func__, port, queue_id);
  return 0;
}

int st_dev_free_rx_queue(struct st_main_impl* impl, enum st_port port,
                         uint16_t queue_id) {
  struct st_interface* inf = st_if(impl, port);
  int ret;
  struct st_rx_queue* rx_queue;

  if (queue_id >= inf->max_rx_queues) {
    err("%s(%d), invalid queue %d\n", __func__, port, queue_id);
    return -EIO;
  }

  rx_queue = &inf->rx_queues[queue_id];
  if (!rx_queue->active) {
    err("%s(%d), queue %d is not allocated\n", __func__, port, queue_id);
    return -EIO;
  }

  dev_if_free_rx_queue_flow(inf, rx_queue);

  if (inf->feature & ST_IF_FEATURE_RUNTIME_RX_QUEUE) {
    ret = rte_eth_dev_rx_queue_stop(inf->port_id, queue_id);
    if (ret < 0)
      err("%s(%d), stop runtime rx queue %d fail %d\n", __func__, port, queue_id, ret);
  }

  rx_queue->active = false;
  info("%s(%d), q %d\n", __func__, port, queue_id);
  return 0;
}

int st_dev_put_sch(struct st_sch_impl* sch, int quota_mbs) {
  int sidx = sch->idx, ret;
  struct st_main_impl* impl = sch->parnet;

  st_sch_free_quota(sch, quota_mbs);

  if (rte_atomic32_dec_and_test(&sch->ref_cnt)) {
    info("%s(%d), ref_cnt now zero\n", __func__, sidx);
    if (sch->data_quota_mbs_total)
      err("%s(%d), still has %d data_quota_mbs_total\n", __func__, sidx,
          sch->data_quota_mbs_total);
    /* stop and free sch */
    ret = st_sch_stop(sch);
    if (ret < 0) {
      err("%s(%d), st_sch_stop fail %d\n", __func__, sidx, ret);
    } else {
      /* put the sch lcore if dev is started */
      if (rte_atomic32_read(&impl->started)) {
        rte_eal_wait_lcore(sch->lcore);
        st_dev_put_lcore(impl, sch->lcore);
      }
    }
    st_sch_free(sch);
  }

  return 0;
}

int st_dev_start_sch(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int sidx = sch->idx;
  int ret;
  unsigned int lcore;

  ret = st_dev_get_lcore(impl, &lcore);
  if (ret < 0) {
    err("%s(%d), dev_get_lcore fail %d\n", __func__, sidx, ret);
    return ret;
  }
  ret = st_sch_start(sch, lcore);
  if (ret < 0) {
    err("%s(%d), st_sch_start fail %d\n", __func__, sidx, ret);
    return ret;
  }

  return 0;
}

struct st_sch_impl* st_dev_get_sch(struct st_main_impl* impl, int quota_mbs,
                                   enum st_sch_type type) {
  int ret, idx;
  struct st_sch_impl* sch;

  for (idx = 0; idx < ST_MAX_SCH_NUM; idx++) {
    sch = st_get_sch(impl, idx);
    if (sch->type != type) continue;
    ret = st_sch_add_quota(sch, quota_mbs);
    if (ret >= 0) {
      /* find one sch capable with quota */
      info("%s(%d), succ with quota_mbs %d\n", __func__, idx, quota_mbs);
      rte_atomic32_inc(&sch->ref_cnt);
      return sch;
    }
  }

  /* no quota, try to create one */
  sch = st_sch_request(impl, type);
  if (!sch) {
    err("%s, no free sch\n", __func__);
    return NULL;
  }
  idx = sch->idx;
  ret = st_sch_add_quota(sch, quota_mbs);
  if (ret < 0) {
    err("%s(%d), st_sch_add_quota fail %d\n", __func__, idx, ret);
    st_sch_free(sch);
    return NULL;
  }

  /* start the sch if dev is started */
  if (rte_atomic32_read(&impl->started)) {
    ret = st_dev_start_sch(impl, sch);
    if (ret < 0) {
      err("%s(%d), start sch fail %d\n", __func__, idx, ret);
      st_sch_free(sch);
      return NULL;
    }
  }

  rte_atomic32_inc(&sch->ref_cnt);
  return sch;
}

int st_dev_create(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_init_params* p = st_get_user_params(impl);
  int ret;
  struct st_interface* inf;

  ret = dev_init_lcores(impl);
  if (ret < 0) return ret;

  for (int i = 0; i < num_ports; i++) {
    inf = st_if(impl, i);

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
    /* DPDK 21.11 support start time sync before rte_eth_dev_start */
    if ((st_has_ptp_service(impl) || st_has_ebu(impl)) &&
        (st_port_type(impl, i) == ST_PORT_PF)) {
      ret = dev_start_timesync(impl, i);
      if (ret >= 0) inf->feature |= ST_IF_FEATURE_TIMESYNC;
    }
#endif

    ret = dev_start_port(impl, i);
    if (ret < 0) {
      err("%s(%d), dev_start_port fail %d\n", __func__, i, ret);
      goto err_exit;
    }
    ret = dev_detect_link(impl, i); /* some port can only detect link after start */
    if (ret < 0) {
      err("%s(%d), dev_detect_link fail %d\n", __func__, i, ret);
      goto err_exit;
    }
    /* try to start time sync after rte_eth_dev_start */
    if ((st_has_ptp_service(impl) || st_has_ebu(impl)) &&
        (st_port_type(impl, i) == ST_PORT_PF) &&
        !(inf->feature & ST_IF_FEATURE_TIMESYNC)) {
      ret = dev_start_timesync(impl, i);
      if (ret >= 0) inf->feature |= ST_IF_FEATURE_TIMESYNC;
    }
    info("%s(%d), feature 0x%x\n", __func__, i, inf->feature);
  }

  /* init sch with one lcore scheduler */
  int data_quota_mbs_per_sch;
  if (st_has_user_quota(impl)) {
    data_quota_mbs_per_sch = st_get_user_params(impl)->data_quota_mbs_per_sch;
  } else {
    /* default: max ST_QUOTA_TX1080P_PER_SCH sessions 1080p@60fps for tx */
    data_quota_mbs_per_sch =
        ST_QUOTA_TX1080P_PER_SCH * st20_1080p59_yuv422_10bit_bandwidth_mps();
  }
  ret = st_sch_init(impl, data_quota_mbs_per_sch);
  if (ret < 0) {
    err("%s, st_sch_init fail %d\n", __func__, ret);
    goto err_exit;
  }

  /* create system sch */
  impl->main_sch = st_dev_get_sch(impl, 0, ST_SCH_TYPE_DEFAULT);
  if (ret < 0) {
    err("%s, get sch fail\n", __func__);
    goto err_exit;
  }

  if (p->flags & ST_FLAG_TSC_PACING)
    impl->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
  else
    impl->tx_pacing_way = ST21_TX_PACING_WAY_AUTO;

  /* rte_eth_stats_get fail in alram context for VF, move it to thread */
  pthread_mutex_init(&impl->stat_wake_mutex, NULL);
  pthread_cond_init(&impl->stat_wake_cond, NULL);
  rte_atomic32_set(&impl->stat_stop, 0);
  rte_ctrl_thread_create(&impl->stat_tid, "st_dev_stat", NULL, dev_stat_thread, impl);
  if (!p->dump_period_s) p->dump_period_s = ST_DEV_STAT_INTERVAL_S;
  rte_eal_alarm_set(ST_DEV_STAT_INTERVAL_US(p->dump_period_s), dev_stat_alarm_handler,
                    impl);

  info("%s, succ, stat period %ds\n", __func__, p->dump_period_s);
  return 0;

err_exit:
  if (impl->main_sch) st_dev_put_sch(impl->main_sch, 0);
  for (int i = num_ports - 1; i >= 0; i--) dev_free_port(impl, i);
  return ret;
}

int st_dev_free(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  int ret, i;

  ret = rte_eal_alarm_cancel(dev_stat_alarm_handler, impl);
  if (ret < 0) err("%s, dev_stat_alarm_handler cancel fail %d\n", __func__, ret);
  if (impl->stat_tid) {
    rte_atomic32_set(&impl->stat_stop, 1);
    dev_stat_wakeup_thread(impl);
    pthread_join(impl->stat_tid, NULL);
    impl->stat_tid = 0;
  }
  pthread_mutex_destroy(&impl->stat_wake_mutex);
  pthread_cond_destroy(&impl->stat_wake_cond);

  st_dev_put_sch(impl->main_sch, 0);

  dev_uinit_lcores(impl);

  for (i = 0; i < num_ports; i++) {
    dev_free_port(impl, i);
  }

  info("%s, succ\n", __func__);
  return 0;
}

int st_dev_start(struct st_main_impl* impl) {
  int ret = 0;
  int num_ports = st_num_ports(impl);
  struct st_sch_impl* sch;

  for (int i = 0; i < num_ports; i++) {
    if ((impl->tx_pacing_way == ST21_TX_PACING_WAY_AUTO) ||
        (impl->tx_pacing_way == ST21_TX_PACING_WAY_RL)) {
      ret = dev_init_ratelimit(impl, i);
      if (ret < 0) break;
    }
  }
  if (impl->tx_pacing_way == ST21_TX_PACING_WAY_AUTO) {
    if (ret < 0) /* fall back to tsc pacing */
      impl->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
    else
      impl->tx_pacing_way = ST21_TX_PACING_WAY_RL;
    ret = 0;
    info("%s, detected pacing way %d\n", __func__, impl->tx_pacing_way);
  }
  if (ret < 0) {
    err("%s, dev_init_ratelimit fail %d\n", __func__, ret);
    return ret;
  }

  /* start active sch */
  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    if (st_sch_is_active(sch)) {
      ret = st_dev_start_sch(impl, sch);
      if (ret < 0) {
        err("%s(%d), st_dev_start_sch fail %d\n", __func__, sch_idx, ret);
        st_dev_stop(impl);
        return ret;
      }
    }
  }

  info("%s, succ\n", __func__);
  return 0;
}

int st_dev_stop(struct st_main_impl* impl) {
  int ret;
  struct st_sch_impl* sch;

  /* stop active sch */
  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    if (st_sch_is_active(sch)) {
      ret = st_sch_stop(sch);
      if (ret < 0) {
        err("%s(%d), st_sch_stop fail %d\n", __func__, sch_idx, ret);
      } else {
        rte_eal_wait_lcore(sch->lcore);
        st_dev_put_lcore(impl, sch->lcore);
      }
    }
  }

  info("%s, succ\n", __func__);
  return 0;
}

int st_dev_get_socket(const char* port) {
  uint16_t port_id = 0;
  int ret = rte_eth_dev_get_port_by_name(port, &port_id);
  if (ret < 0) {
    err("%s, failed to locate %s. Please run nicctl.sh\n", __func__, port);
    return ret;
  }
  return rte_eth_dev_socket_id(port_id);
}

int st_dev_init(struct st_init_params* p) {
  int ret;

  ret = dev_eal_init(p);
  if (ret < 0) {
    err("%s, dev_eal_init fail %d\n", __func__, ret);
    return ret;
  }

  return 0;
}

int st_dev_uinit(struct st_init_params* p) {
  rte_eal_cleanup();

  info("%s, succ\n", __func__);
  return 0;
}

int st_dev_dst_ip_mac(struct st_main_impl* impl, uint8_t dip[ST_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum st_port port) {
  int ret;
  struct st_init_params* p = st_get_user_params(impl);

  if ((port == ST_PORT_P) && (p->flags & ST_FLAG_USER_P_TX_MAC)) {
    rte_memcpy(&ea->addr_bytes[0], &p->tx_dst_mac[port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((port == ST_PORT_R) && (p->flags & ST_FLAG_USER_R_TX_MAC)) {
    rte_memcpy(&ea->addr_bytes[0], &p->tx_dst_mac[port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_R_TX_MAC\n", __func__);
  } else if (st_is_multicast_ip(dip)) {
    st_mcast_ip_to_mac(dip, ea);
  } else {
    bool arp_kni = true;

    info("%s(%d), start to get mac for ip %d.%d.%d.%d\n", __func__, port, dip[0], dip[1],
         dip[2], dip[3]);
    if (!arp_kni) {
      ret = st_arp_socket_get_mac(impl, ea, dip, "plcaeholder");
      if (ret < 0) {
        err("%s(%d), failed to get mac from socket %d\n", __func__, port, ret);
        return ret;
      }
    } else {
      st_reset_arp(impl, port);
      uint32_t dip32;
      memcpy(&dip32, dip, ST_IP_ADDR_LEN);
      ret = st_arp_cni_get_mac(impl, ea, port, dip32);
      if (ret < 0) {
        err("%s(%d), failed to get mac from cni %d\n", __func__, port, ret);
        return ret;
      }
    }
  }

  info("%s(%d), ip: %d.%d.%d.%d, mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
       __func__, port, dip[0], dip[1], dip[2], dip[3], ea->addr_bytes[0],
       ea->addr_bytes[1], ea->addr_bytes[2], ea->addr_bytes[3], ea->addr_bytes[4],
       ea->addr_bytes[5]);
  return 0;
}

int st_dev_if_uinit(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl), ret;
  struct st_interface* inf;

  for (int i = 0; i < num_ports; i++) {
    inf = st_if(impl, i);

    if (inf->pad) {
      rte_pktmbuf_free(inf->pad);
      inf->pad = NULL;
    }

    dev_if_uinit_tx_queues(inf);
    dev_if_uinit_rx_queues(inf);

    if (inf->mcast_mac_lists) {
      warn("%s(%d), mcast_mac_lists still active\n", __func__, i);
      free(inf->mcast_mac_lists);
      inf->mcast_mac_lists = NULL;
    }

    if (inf->mbuf_pool) {
      ret = st_mempool_free(inf->mbuf_pool);
      if (ret >= 0) inf->mbuf_pool = NULL;
    }
  }

  return 0;
}

int st_dev_if_init(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_init_params* p = st_get_user_params(impl);
  uint16_t port_id;
  enum st_port_type port_type;
  char* port;
  struct rte_eth_dev_info dev_info;
  struct st_interface* inf;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    port = p->port[i];
    ret = rte_eth_dev_get_port_by_name(port, &port_id);
    if (ret < 0) {
      err("%s, failed to locate %s. Please run nicctl.sh\n", __func__, port);
      st_dev_if_uinit(impl);
      return ret;
    }
    rte_eth_dev_info_get(port_id, &dev_info);
    if (ret < 0) {
      err("%s, rte_eth_dev_info_get fail for %s\n", __func__, port);
      st_dev_if_uinit(impl);
      return ret;
    }
    port_type = _get_type_from_driver(dev_info.driver_name);

    inf = st_if(impl, i);
    inf->port = i;
    inf->port_id = port_id;
    inf->port_type = port_type;

    if (st_has_user_ptp(impl)) /* user provide the ptp source */
      inf->ptp_get_time_fn = ptp_from_user;
    else
      inf->ptp_get_time_fn = ptp_from_real_time;

    /* set max tx/rx queues */
    inf->max_tx_queues = impl->tx_sessions_cnt_max + 2; /* arp, mcast */
    inf->max_rx_queues = impl->rx_sessions_cnt_max + 1; /* cni */
    if (st_has_ptp_service(impl)) {
      inf->max_tx_queues++;
      inf->max_rx_queues++;
    }
#ifdef ST_HAS_KNI
    inf->max_tx_queues++; /* kni tx queue */
#endif

    /* when using VF, num_queue_pairs will be set as the max of tx/rx */
    if (port_type == ST_PORT_VF) {
      inf->max_tx_queues = RTE_MAX(inf->max_rx_queues, inf->max_tx_queues);
      inf->max_rx_queues = inf->max_tx_queues;
    }

    if (dev_info.dev_capa & RTE_ETH_DEV_CAPA_RUNTIME_RX_QUEUE_SETUP)
      inf->feature |= ST_IF_FEATURE_RUNTIME_RX_QUEUE;

    if (st_has_ebu(impl) && (dev_info.rx_offload_capa & DEV_RX_OFFLOAD_TIMESTAMP)) {
      if (!impl->dynfield_offset) {
        ret = rte_mbuf_dyn_rx_timestamp_register(&impl->dynfield_offset, NULL);
        if (ret < 0) {
          err("%s, rte_mbuf_dyn_rx_timestamp_register fail\n", __func__);
          return ret;
        }
        info("%s, rte_mbuf_dyn_rx_timestamp_register: mbuf dynfield offset: %d\n",
             __func__, impl->dynfield_offset);
      }
      inf->feature |= ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;
    }

    ret = dev_config_port(impl, i);
    if (ret < 0) {
      err("%s(%d), dev_config_port fail %d\n", __func__, i, ret);
      st_dev_if_uinit(impl);
      return -EIO;
    }

    /* Creates a default mempool in memory to hold the system mbufs. */
    unsigned int mbuf_elements = 1024;
    if (p->flags & ST_FLAG_RX_QUEUE_MONO_POOL) {
      /* append rx sessions */
      mbuf_elements += inf->max_rx_queues * inf->nb_rx_desc;
    }
    char pool_name[ST_MAX_NAME_LEN];
    snprintf(pool_name, ST_MAX_NAME_LEN, "ST%d_SYS_MBUF_POOL", i);
    struct rte_mempool* mbuf_pool =
        st_mempool_create_common(impl, i, pool_name, mbuf_elements);
    if (!mbuf_pool) {
      st_dev_if_uinit(impl);
      return -ENOMEM;
    }
    inf->mbuf_pool = mbuf_pool;

    ret = dev_if_init_tx_queues(impl, inf);
    if (ret < 0) {
      st_dev_if_uinit(impl);
      return -ENOMEM;
    }
    ret = dev_if_init_rx_queues(impl, inf);
    if (ret < 0) {
      st_dev_if_uinit(impl);
      return -ENOMEM;
    }

    inf->pad = st_build_pad(impl, i, port_id, RTE_ETHER_TYPE_IPV4, 1024);
    if (!inf->pad) {
      err("%s(%d), pad alloc fail\n", __func__, i);
      st_dev_if_uinit(impl);
      return -ENOMEM;
    }

    info("%s(%d), port_id %d port_type %d driver %s\n", __func__, i, port_id, port_type,
         dev_info.driver_name);
    uint8_t* ip = p->sip_addr[i];
    info("%s(%d), sip: %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
  }

  return 0;
}
