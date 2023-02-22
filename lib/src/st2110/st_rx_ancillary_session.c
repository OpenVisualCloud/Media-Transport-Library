/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_ancillary_session.h"

#include "../mt_log.h"
#include "st_ancillary_transmitter.h"

/* call rx_ancillary_session_put always if get successfully */
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_get(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_try_get(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline bool rx_ancillary_session_get_empty(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_ancillary_session_put(struct st_rx_ancillary_sessions_mgr* mgr,
                                            int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static int rx_ancillary_session_init(struct mtl_main_impl* impl,
                                     struct st_rx_ancillary_sessions_mgr* mgr,
                                     struct st_rx_ancillary_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int rx_ancillary_sessions_tasklet_start(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_ancillary_sessions_tasklet_stop(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_ancillary_session_handle_pkt(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum mtl_session_port s_port) {
  struct st40_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint8_t payload_type = rtp->payload_type;

  if (payload_type != ops->payload_type) {
    s->st40_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* set first seq_id - 1 */
  if (unlikely(s->st40_seq_id == -1)) s->st40_seq_id = seq_id - 1;
  /* drop old packet */
  if (st_rx_seq_drop(seq_id, s->st40_seq_id, 5)) {
    dbg("%s(%d,%d), drop as pkt seq %d is old\n", __func__, s->idx, s_port, seq_id);
    s->st40_stat_pkts_dropped++;
    return 0;
  }
  /* update seq id */
  s->st40_seq_id = seq_id;

  /* enqueue to packet ring to let app to handle */
  int ret = rte_ring_sp_enqueue(s->packet_ring, (void*)mbuf);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring, packet drop, pkt seq %d\n", __func__,
        s->idx, seq_id);
    s->st40_stat_pkts_dropped++;
    return 0;
  }
  rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */

  if (rtp->tmstamp != s->tmstamp) {
    rte_atomic32_inc(&s->st40_stat_frames_received);
    s->tmstamp = rtp->tmstamp;
  }
  s->st40_stat_pkts_received++;
  /* get a valid packet */
  if (ops->priv) ops->notify_rtp_ready(ops->priv);

  return 0;
}
static int rx_ancillary_session_handle_mbuf(void* priv, struct rte_mbuf** mbuf,
                                            uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_ancillary_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_port s_port = s_priv->port;

  for (uint16_t i = 0; i < nb; i++)
    rx_ancillary_session_handle_pkt(impl, s, mbuf[i], s_port);

  return 0;
}

static int rx_ancillary_session_tasklet(struct mtl_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_ANCILLARY_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;
  bool done = true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (s->rss[s_port]) {
      rv = mt_rss_burst(s->rss[s_port], ST_RX_ANCILLARY_BURTS_SIZE);
    } else if (s->queue[s_port]) {
      rv = mt_dev_rx_burst(s->queue[s_port], &mbuf[0], ST_RX_ANCILLARY_BURTS_SIZE);
      if (rv) {
        rx_ancillary_session_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
        rte_pktmbuf_free_bulk(&mbuf[0], rv);
      }
    } else {
      continue;
    }

    if (rv) done = false;
  }

  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static int rx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parnet;
  struct st_rx_ancillary_session_impl* s;
  int pending = MT_TASKLET_ALL_DONE;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_ancillary_session_try_get(mgr, sidx);
    if (!s) continue;

    pending += rx_ancillary_session_tasklet(impl, s);
    rx_ancillary_session_put(mgr, sidx);
  }

  return pending;
}

static int rx_ancillary_session_uinit_hw(struct mtl_main_impl* impl,
                                         struct st_rx_ancillary_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->queue[i]) {
      mt_dev_put_rx_queue(impl, s->queue[i]);
      s->queue[i] = NULL;
    }
  }

  return 0;
}

static int rx_ancillary_session_init_hw(struct mtl_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  struct mt_rx_flow flow;
  enum mtl_port port;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    s->port_id[i] = mt_port_id(impl, port);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].port = port;

    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.sip_addr[i], MTL_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st40_dst_port[i];

    /* no flow for data path only */
    if (mt_pmd_is_kernel(impl, port) && (s->ops.flags & ST40_RX_FLAG_DATA_PATH_ONLY))
      s->queue[i] = mt_dev_get_rx_queue(impl, port, NULL);
    else if (mt_has_rss(impl, port)) {
      flow.priv = &s->priv[i];
      flow.cb = rx_ancillary_session_handle_mbuf;
      s->rss[i] = mt_rss_get(impl, port, &flow);
    } else
      s->queue[i] = mt_dev_get_rx_queue(impl, port, &flow);
    if (!s->queue[i] && !s->rss[i]) {
      rx_ancillary_session_uinit_hw(impl, s);
      return -EIO;
    }

    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         mt_has_rss(impl, port) ? mt_rss_queue_id(s->rss[i])
                                : mt_dev_rx_queue_id(s->queue[i]),
         flow.dst_port);
  }

  return 0;
}

static int rx_ancillary_session_uinit_mcast(struct mtl_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (mt_is_multicast_ip(ops->sip_addr[i]))
      mt_mcast_leave(impl, mt_ip_to_u32(ops->sip_addr[i]),
                     mt_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rx_ancillary_session_init_mcast(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  int ret;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!mt_is_multicast_ip(ops->sip_addr[i])) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    if (mt_pmd_is_kernel(impl, port) && (ops->flags & ST30_RX_FLAG_DATA_PATH_ONLY)) {
      info("%s(%d), skip mcast join for port %d\n", __func__, s->idx, i);
      return 0;
    }
    ret = mt_mcast_join(impl, mt_ip_to_u32(ops->sip_addr[i]),
                        mt_port_logic2phy(s->port_maps, i));
    if (ret < 0) return ret;
  }

  return 0;
}

static int rx_ancillary_session_init_sw(struct mtl_main_impl* impl,
                                        struct st_rx_ancillary_sessions_mgr* mgr,
                                        struct st_rx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-ANC-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (count <= 0) {
    err("%s(%d,%d), invalid rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
    return -ENOMEM;
  }
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_ancillary_session_uinit_sw(struct mtl_main_impl* impl,
                                         struct st_rx_ancillary_session_impl* s) {
  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  return 0;
}

static int rx_ancillary_session_attach(struct mtl_main_impl* impl,
                                       struct st_rx_ancillary_sessions_mgr* mgr,
                                       struct st_rx_ancillary_session_impl* s,
                                       struct st40_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st40_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx);
    s->st40_dst_port[i] = s->st40_src_port[i];
  }

  s->st40_seq_id = -1;
  s->st40_stat_pkts_received = 0;
  s->st40_stat_pkts_dropped = 0;
  s->st40_stat_pkts_wrong_hdr_dropped = 0;
  s->st40_stat_last_time = mt_get_monotonic_time();
  rte_atomic32_set(&s->st40_stat_frames_received, 0);

  ret = rx_ancillary_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  ret = rx_ancillary_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_rtps fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit_hw(impl, s);
    return -EIO;
  }

  ret = rx_ancillary_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit_sw(impl, s);
    rx_ancillary_session_uinit_hw(impl, s);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static void rx_ancillary_session_stat(struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st40_stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->st40_stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->st40_stat_frames_received, 0);

  notice("RX_ANC_SESSION(%d:%s): fps %f, st40 received frames %d, received pkts %d\n",
         idx, s->ops_name, framerate, frames_received, s->st40_stat_pkts_received);
  s->st40_stat_pkts_received = 0;
  s->st40_stat_last_time = cur_time_ns;

  if (s->st40_stat_pkts_dropped) {
    notice("RX_ANC_SESSION(%d): st40 dropped pkts %d\n", idx, s->st40_stat_pkts_dropped);
    s->st40_stat_pkts_dropped = 0;
  }
  if (s->st40_stat_pkts_wrong_hdr_dropped) {
    notice("RX_AUDIO_SESSION(%d): wrong hdr dropped pkts %d\n", idx,
           s->st40_stat_pkts_wrong_hdr_dropped);
    s->st40_stat_pkts_wrong_hdr_dropped = 0;
  }
}

static int rx_ancillary_session_detach(struct mtl_main_impl* impl,
                                       struct st_rx_ancillary_session_impl* s) {
  rx_ancillary_session_stat(s);
  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_sw(impl, s);
  rx_ancillary_session_uinit_hw(impl, s);
  return 0;
}

static int rx_ancillary_session_update_src(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st40_rx_ops* ops = &s->ops;

  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st40_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx);
    s->st40_dst_port[i] = s->st40_src_port[i];
  }
  /* reset seq id */
  s->st40_seq_id = -1;

  ret = rx_ancillary_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rx_ancillary_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_ancillary_sessions_mgr_update_src(struct st_rx_ancillary_sessions_mgr* mgr,
                                                struct st_rx_ancillary_session_impl* s,
                                                struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rx_ancillary_session_update_src(mgr->parnet, s, src);
  rx_ancillary_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_ancillary_sessions_mgr_init(struct mtl_main_impl* impl,
                                          struct mt_sch_impl* sch,
                                          struct st_rx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mt_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_anc_sessions_mgr";
  ops.start = rx_ancillary_sessions_tasklet_start;
  ops.stop = rx_ancillary_sessions_tasklet_stop;
  ops.handler = rx_ancillary_sessions_tasklet_handler;

  mgr->tasklet = mt_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mt_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_rx_ancillary_session_impl* rx_ancillary_sessions_mgr_attach(
    struct st_rx_ancillary_sessions_mgr* mgr, struct st40_rx_ops* ops) {
  int midx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parnet;
  int ret;
  struct st_rx_ancillary_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (!rx_ancillary_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, MTL_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      return NULL;
    }
    ret = rx_ancillary_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rx_ancillary_session_attach(mgr->parnet, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_ancillary_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int rx_ancillary_sessions_mgr_detach(struct st_rx_ancillary_sessions_mgr* mgr,
                                            struct st_rx_ancillary_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rx_ancillary_session_detach(mgr->parnet, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  rx_ancillary_session_put(mgr, idx);

  return 0;
}

static int rx_ancillary_sessions_mgr_update(struct st_rx_ancillary_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_rx_ancillary_sessions_stat(struct mtl_main_impl* impl) {
  struct st_rx_ancillary_sessions_mgr* mgr = &impl->rx_anc_mgr;
  struct st_rx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_ancillary_session_get(mgr, j);
    if (!s) continue;
    rx_ancillary_session_stat(s);
    rx_ancillary_session_put(mgr, j);
  }
}

int st_rx_ancillary_sessions_mgr_uinit(struct st_rx_ancillary_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_ancillary_session_impl* s;

  if (mgr->tasklet) {
    mt_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    s = rx_ancillary_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rx_ancillary_sessions_mgr_detach(mgr, s);
    rx_ancillary_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int rx_ancillary_ops_check(struct st40_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > MTL_SESSION_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->sip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->sip_addr[0], ops->sip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->rtp_ring_size <= 0) {
    err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
    return -EINVAL;
  }

  if (!ops->notify_rtp_ready) {
    err("%s, pls set notify_rtp_ready\n", __func__);
    return -EINVAL;
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_anc_init(struct mtl_main_impl* impl) {
  int ret;

  if (impl->rx_anc_init) return 0;

  /* create rx ancillary context */
  ret = rx_ancillary_sessions_mgr_init(impl, impl->main_sch, &impl->rx_anc_mgr);
  if (ret < 0) {
    err("%s, rx_ancillary_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  impl->rx_anc_init = true;
  return 0;
}

st40_rx_handle st40_rx_create(mtl_handle mt, struct st40_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st_rx_ancillary_session_handle_impl* s_impl;
  struct st_rx_ancillary_session_impl* s;
  int ret;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  ret = st_rx_anc_init(impl);
  mt_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), mt_socket_id(impl, MTL_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  s = rx_ancillary_sessions_mgr_attach(&impl->rx_anc_mgr, ops);
  mt_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
  if (!s) {
    err("%s, rx_ancillary_sessions_mgr_attach fail\n", __func__);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = MT_HANDLE_RX_ANC;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st40_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, s->idx);
  return s_impl;
}

int st40_rx_update_source(st40_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rx_ancillary_sessions_mgr_update_src(&impl->rx_anc_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st40_rx_free(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  int ret, idx;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = rx_ancillary_sessions_mgr_detach(&impl->rx_anc_mgr, s);
  if (ret < 0) err("%s(%d), rx_ancillary_sessions_mgr_detach fail\n", __func__, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  rx_ancillary_sessions_mgr_update(&impl->rx_anc_mgr);
  mt_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

void* st40_rx_get_mbuf(st40_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_rx_ancillary_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(packet_ring, (void**)&pkt);
  if (ret == 0) {
    int header_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                     sizeof(struct rte_udp_hdr);
    *len = pkt->data_len - header_len;
    *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, header_len);
    return (void*)pkt;
  }

  return NULL;
}

void st40_rx_put_mbuf(st40_rx_handle handle, void* mbuf) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_HANDLE_RX_ANC)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st40_rx_get_queue_meta(st40_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;
  struct mtl_main_impl* impl;
  enum mtl_port port;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  impl = s_impl->parnet;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    if (mt_pmd_type(impl, port) == MTL_PMD_DPDK_AF_XDP) {
      /* af_xdp pmd */
      meta->start_queue[i] = mt_start_queue(impl, port);
    }
    meta->queue_id[i] = mt_dev_rx_queue_id(s->queue[i]);
  }

  return 0;
}
