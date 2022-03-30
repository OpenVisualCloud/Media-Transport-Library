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

#include "st_rx_ancillary_session.h"

#include "st_dev.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_sch.h"
#include "st_util.h"

static inline void rx_ancillary_session_lock(struct st_rx_ancillary_sessions_mgr* mgr,
                                             int sidx) {
  rte_spinlock_lock(&mgr->mutex[sidx]);
}

static inline int rx_ancillary_session_try_lock(struct st_rx_ancillary_sessions_mgr* mgr,
                                                int sidx) {
  return rte_spinlock_trylock(&mgr->mutex[sidx]);
}

static inline void rx_ancillary_session_unlock(struct st_rx_ancillary_sessions_mgr* mgr,
                                               int sidx) {
  rte_spinlock_unlock(&mgr->mutex[sidx]);
}

static int rx_ancillary_session_init(struct st_main_impl* impl,
                                     struct st_rx_ancillary_sessions_mgr* mgr,
                                     struct st_rx_ancillary_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int rx_ancillary_session_uinit(struct st_main_impl* impl,
                                      struct st_rx_ancillary_session_impl* s) {
  dbg("%s(%d), succ\n", __func__, s->idx);
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

static int rx_ancillary_session_handle_pkt(struct st_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum st_session_port s_port) {
  struct st40_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);

  uint16_t seq_id = ntohs(rtp->seq_number);

  /* set first seq_id - 1 */
  if (unlikely(s->st40_seq_id == -1)) s->st40_seq_id = seq_id - 1;
  /* drop old packet */
  if (st_rx_seq_drop(seq_id, s->st40_seq_id, 5)) {
    dbg("%s(%d,%d), drop as pkt seq %d is old\n", __func__, s->idx, s_port, seq_id);
    s->st40_stat_pkts_dropped++;
    rte_pktmbuf_free(mbuf);
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
    rte_pktmbuf_free(mbuf);
    return 0;
  }
  if (rtp->tmstamp != s->tmstamp) {
    rte_atomic32_inc(&s->st40_stat_frames_received);
    s->tmstamp = rtp->tmstamp;
  }
  s->st40_stat_pkts_received++;
  /* get a valid packet */
  if (ops->priv) ops->notify_rtp_ready(ops->priv);

  return 0;
}

static int rx_ancillary_session_tasklet(struct st_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_ANCILLARY_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->queue_active[s_port]) continue;
    rv = rte_eth_rx_burst(s->port_id[s_port], s->queue_id[s_port], &mbuf[0],
                          ST_RX_ANCILLARY_BURTS_SIZE);
    if (rv > 0) {
      for (uint16_t i = 0; i < rv; i++)
        rx_ancillary_session_handle_pkt(impl, s, mbuf[i], s_port);
    }
  }

  return 0;
}

static int rx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_rx_ancillary_session_impl* s;
  int sidx;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    if (rx_ancillary_session_try_lock(mgr, sidx)) {
      if (mgr->active[sidx]) {
        s = &mgr->sessions[sidx];
        rx_ancillary_session_tasklet(impl, s);
      }
      rx_ancillary_session_unlock(mgr, sidx);
    }
  }

  return 0;
}

static int rx_ancillary_session_uinit_hw(struct st_main_impl* impl,
                                         struct st_rx_ancillary_session_impl* s) {
  int num_port = s->ops.num_port;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    if (s->queue_active[i]) {
      st_dev_free_rx_queue(impl, port, s->queue_id[i]);
      s->queue_active[i] = false;
    }
  }

  return 0;
}

static int rx_ancillary_session_init_hw(struct st_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  int ret;
  uint16_t queue;
  struct st_rx_flow flow;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    memset(&flow, 0xff, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.sip_addr[i], ST_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, st_sip_addr(impl, port), ST_IP_ADDR_LEN);
    flow.port_flow = true;
    flow.dst_port = s->st40_dst_port[i];

    ret = st_dev_request_rx_queue(impl, port, &queue, &flow);
    if (ret < 0) {
      rx_ancillary_session_uinit_hw(impl, s);
      return ret;
    }

    s->port_id[i] = st_port_id(impl, port);
    s->queue_id[i] = queue;
    s->queue_active[i] = true;
    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port, queue,
         flow.dst_port);
  }

  return 0;
}

static int rx_ancillary_session_uinit_mcast(struct st_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (st_is_multicast_ip(ops->sip_addr[i]))
      st_mcast_leave(impl, *(uint32_t*)ops->sip_addr[i],
                     st_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rx_ancillary_session_init_mcast(struct st_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  int ret;

  for (int i = 0; i < ops->num_port; i++) {
    if (!st_is_multicast_ip(ops->sip_addr[i])) continue;
    ret = st_mcast_join(impl, *(uint32_t*)ops->sip_addr[i],
                        st_port_logic2phy(s->port_maps, i));
    if (ret < 0) return ret;
  }

  return 0;
}

static int rx_ancillary_session_init_sw(struct st_main_impl* impl,
                                        struct st_rx_ancillary_sessions_mgr* mgr,
                                        struct st_rx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-ANC-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
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

static int rx_ancillary_session_uinit_sw(struct st_main_impl* impl,
                                         struct st_rx_ancillary_session_impl* s) {
  if (s->packet_ring) {
    st_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  return 0;
}

static int rx_ancillary_session_attach(struct st_main_impl* impl,
                                       struct st_rx_ancillary_sessions_mgr* mgr,
                                       struct st_rx_ancillary_session_impl* s,
                                       struct st40_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st40_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx);
    s->st40_dst_port[i] = s->st40_src_port[i];
  }

  s->st40_seq_id = -1;
  s->st40_stat_pkts_received = 0;
  s->st40_stat_pkts_dropped = 0;
  s->st40_stat_last_time = st_get_monotonic_time();
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
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st40_stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->st40_stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->st40_stat_frames_received, 0);

  info("RX_ANC_SESSION(%d): fps %f, st40 received frames %d, received pkts %d\n", idx,
       framerate, frames_received, s->st40_stat_pkts_received);
  s->st40_stat_pkts_received = 0;
  s->st40_stat_last_time = cur_time_ns;

  if (s->st40_stat_pkts_dropped) {
    info("RX_ANC_SESSION(%d): st40 dropped pkts %d\n", idx, s->st40_stat_pkts_dropped);
    s->st40_stat_pkts_dropped = 0;
  }
}

static int rx_ancillary_session_detach(struct st_main_impl* impl,
                                       struct st_rx_ancillary_session_impl* s) {
  rx_ancillary_session_stat(s);
  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_sw(impl, s);
  rx_ancillary_session_uinit_hw(impl, s);
  return 0;
}

static int rx_ancillary_session_update_src(struct st_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st40_rx_ops* ops = &s->ops;

  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], ST_IP_ADDR_LEN);
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

int st_rx_ancillary_sessions_mgr_update_src(struct st_rx_ancillary_sessions_mgr* mgr,
                                            struct st_rx_ancillary_session_impl* s,
                                            struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, sidx = s->idx;

  if (s != &mgr->sessions[sidx]) {
    err("%s(%d,%d), mismatch session\n", __func__, midx, sidx);
    return -EIO;
  }

  if (!mgr->active[sidx]) {
    err("%s(%d,%d), not active\n", __func__, midx, sidx);
    return -EIO;
  }

  rx_ancillary_session_lock(mgr, sidx);
  ret = rx_ancillary_session_update_src(mgr->parnet, s, src);
  rx_ancillary_session_unlock(mgr, sidx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, sidx, ret);
    return ret;
  }

  return 0;
}

int st_rx_ancillary_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                      struct st_rx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  int ret;
  struct st_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
    ret = rx_ancillary_session_init(impl, mgr, &mgr->sessions[i], i);
    if (ret < 0) {
      err("%s(%d), rx_audio_session_init fail %d for %d\n", __func__, idx, ret, i);
      return ret;
    }
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_anc_sessions_mgr";
  ops.start = rx_ancillary_sessions_tasklet_start;
  ops.stop = rx_ancillary_sessions_tasklet_stop;
  ops.handler = rx_ancillary_sessions_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    err("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_rx_ancillary_sessions_mgr_uinit(struct st_rx_ancillary_sessions_mgr* mgr) {
  int idx = mgr->idx;
  int ret, i;
  struct st_rx_ancillary_session_impl* s;

  for (i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    s = &mgr->sessions[i];

    if (mgr->active[i]) { /* make sure all session are detached*/
      warn("%s(%d), session %d still attached\n", __func__, idx, i);
      ret = st_rx_ancillary_sessions_mgr_detach(mgr, s);
      if (ret < 0) {
        err("%s(%d), st_rx_audio_sessions_mgr_detach fail %d for %d\n", __func__, idx,
            ret, i);
      }
    }

    ret = rx_ancillary_session_uinit(mgr->parnet, s);
    if (ret < 0) {
      err("%s(%d), st_rx_audio_session_uinit fail %d for %d\n", __func__, idx, ret, i);
    }
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

struct st_rx_ancillary_session_impl* st_rx_ancillary_sessions_mgr_attach(
    struct st_rx_ancillary_sessions_mgr* mgr, struct st40_rx_ops* ops) {
  int midx = mgr->idx;
  int i, ret;
  struct st_rx_ancillary_session_impl* s;

  for (i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (!mgr->active[i]) {
      s = &mgr->sessions[i];
      ret = rx_ancillary_session_attach(mgr->parnet, mgr, s, ops);
      if (ret < 0) {
        err("%s(%d), rx_audio_session_attach fail on %d\n", __func__, midx, i);
        return NULL;
      }
      mgr->active[i] = true;
      mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
      return s;
    }
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_rx_ancillary_sessions_mgr_detach(struct st_rx_ancillary_sessions_mgr* mgr,
                                        struct st_rx_ancillary_session_impl* s) {
  int midx = mgr->idx;
  int sidx = s->idx;

  if (s != &mgr->sessions[sidx]) {
    err("%s(%d,%d), mismatch session\n", __func__, midx, sidx);
    return -EIO;
  }

  if (!mgr->active[sidx]) {
    err("%s(%d,%d), not active\n", __func__, midx, sidx);
    return -EIO;
  }

  rx_ancillary_session_lock(mgr, sidx);

  rx_ancillary_session_detach(mgr->parnet, s);

  mgr->active[sidx] = false;

  rx_ancillary_session_unlock(mgr, sidx);

  return 0;
}

int st_rx_ancillary_sessions_mgr_update(struct st_rx_ancillary_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (mgr->active[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_rx_ancillary_sessions_stat(struct st_main_impl* impl) {
  struct st_rx_ancillary_sessions_mgr* mgr = &impl->rx_anc_mgr;
  struct st_rx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    if (mgr->active[j]) {
      s = &mgr->sessions[j];
      rx_ancillary_session_stat(s);
    }
  }
}
