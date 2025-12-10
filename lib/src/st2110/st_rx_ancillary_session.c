/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_ancillary_session.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_stat.h"
#include "st_ancillary_transmitter.h"

#ifdef MTL_ENABLE_FUZZING_ST40
#define ST40_FUZZ_LOG(...) info(__VA_ARGS__)
#else
#define ST40_FUZZ_LOG(...) \
  do {                     \
  } while (0)
#endif

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
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_get_timeout(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
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

static inline uint16_t rx_ancillary_queue_id(struct st_rx_ancillary_session_impl* s,
                                             enum mtl_session_port s_port) {
  return mt_rxq_queue_id(s->rxq[s_port]);
}

static int rx_ancillary_session_init(struct st_rx_ancillary_sessions_mgr* mgr,
                                     struct st_rx_ancillary_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
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
  struct st40_rfc8331_rtp_hdr* rfc8331 = (struct st40_rfc8331_rtp_hdr*)rtp;
  rfc8331->swapped_first_hdr_chunk = ntohl(rfc8331->swapped_first_hdr_chunk);
  MTL_MAY_UNUSED(s_port);
  uint32_t pkt_len = mbuf->data_len - sizeof(struct st40_rfc8331_rtp_hdr);
  MTL_MAY_UNUSED(pkt_len);
  uint32_t tmstamp = ntohl(rtp->tmstamp);

  if (ops->payload_type && (payload_type != ops->payload_type)) {
#ifdef MTL_ENABLE_FUZZING_ST40
    ST40_FUZZ_LOG("%s(%d,%d), drop payload_type %u expected %u\n", __func__, s->idx,
                  s_port, payload_type, ops->payload_type);
#endif
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->ssrc);
    if (ssrc != ops->ssrc) {
#ifdef MTL_ENABLE_FUZZING_ST40
      ST40_FUZZ_LOG("%s(%d,%d), drop ssrc %u expected %u\n", __func__, s->idx, s_port,
                    ssrc, ops->ssrc);
#endif
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }

  /* Drop if F is 0b01 (invalid: bit 0 set, bit 1 clear) */
  if ((rfc8331->first_hdr_chunk.f & 0x3) == 0x1) {
#ifdef MTL_ENABLE_FUZZING_ST40
    ST40_FUZZ_LOG("%s(%d,%d), drop invalid field bits 0x%x\n", __func__, s->idx, s_port,
                  rfc8331->first_hdr_chunk.f);
#endif
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_interlace_dropped);
    return -EINVAL;
  }
  /* 0b10: first field (bit 1 set, bit 0 clear)
     0b11: second field (bit 1 set, bit 0 set) */
  if (rfc8331->first_hdr_chunk.f & 0x2) {
    if (rfc8331->first_hdr_chunk.f & 0x1) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
    } else {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
    }
  }
  /* 0b00: progressive or not specified, do nothing */

  if (unlikely(s->latest_seq_id[s_port] == -1)) s->latest_seq_id[s_port] = seq_id - 1;
  if (unlikely(s->session_seq_id == -1)) s->session_seq_id = seq_id - 1;
  if (unlikely(s->tmstamp == -1)) s->tmstamp = tmstamp - 1;

  /* not a big deal as long as stream is continous */
  if (seq_id != (uint16_t)(s->latest_seq_id[s_port] + 1)) {
    dbg("%s(%d,%d), non-continuous seq now %u last %d\n", __func__, s->idx, s_port,
        seq_id, s->latest_seq_id[s_port]);
    s->port_user_stats.common.port[s_port].out_of_order_packets++;
    s->stat_pkts_out_of_order_per_port[s_port]++;
  }
  s->latest_seq_id[s_port] = seq_id;

  /* in ancillary we assume packet is redundant when the seq_id is old (it's possible to
  get multiple packets with the same timestamp)) */
  if ((mt_seq32_greater(s->tmstamp, tmstamp)) ||
      !mt_seq16_greater(seq_id, s->session_seq_id)) {
    if (!mt_seq16_greater(seq_id, s->session_seq_id)) {
#ifdef MTL_ENABLE_FUZZING_ST40
      ST40_FUZZ_LOG("%s(%d,%d), redundant seq %u last %d\n", __func__, s->idx, s_port,
                    seq_id, s->session_seq_id);
#endif
      dbg("%s(%d,%d), redundant seq now %u session last %d\n", __func__, s->idx, s_port,
          seq_id, s->session_seq_id);
    } else {
#ifdef MTL_ENABLE_FUZZING_ST40
      ST40_FUZZ_LOG("%s(%d,%d), redundant ts %u last %ld\n", __func__, s->idx, s_port,
                    tmstamp, s->tmstamp);
#endif
      dbg("%s(%d,%d), redundant tmstamp now %u session last %ld\n", __func__, s->idx,
          s_port, tmstamp, s->tmstamp);
    }

    s->redundant_error_cnt[s_port]++;
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant);

    for (int i = 0; i < s->ops.num_port; i++) {
      if (s->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        return -EIO;
      }
    }
    warn(
        "%s(%d), redundant error threshold reached, accept packet seq %u (old seq_id "
        "%d), timestamp %u (old timestamp %ld)\n",
        __func__, s->idx, seq_id, s->session_seq_id, tmstamp, s->tmstamp);
  }
  s->redundant_error_cnt[s_port] = 0;

  /* hole in seq id packets going into the session check if the seq_id of the session is
   * consistent */
  if (seq_id != (uint16_t)(s->session_seq_id + 1)) {
    dbg("%s(%d,%d), session seq_id %u out of order %d\n", __func__, s->idx, s_port,
        seq_id, s->session_seq_id);
    s->stat_pkts_out_of_order++;
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
  }

  /* update seq id */
  s->session_seq_id = seq_id;

  /* enqueue to packet ring to let app to handle */
  int ret = rte_ring_sp_enqueue(s->packet_ring, (void*)mbuf);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring, packet drop, pkt seq %d\n", __func__,
        s->idx, seq_id);
#ifdef MTL_ENABLE_FUZZING_ST40
    ST40_FUZZ_LOG("%s(%d,%d), enqueue failure for seq %u len %u\n", __func__, s->idx,
                  s_port, seq_id, pkt_len);
#endif
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_enqueue_fail);
    MT_USDT_ST40_RX_MBUF_ENQUEUE_FAIL(s->mgr->idx, s->idx, mbuf, tmstamp);
    return 0;
  }
  rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */

  if (tmstamp != s->tmstamp) {
    rte_atomic32_inc(&s->stat_frames_received);
    s->port_user_stats.common.port[s_port].frames++;
    s->tmstamp = tmstamp;
  }
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  s->port_user_stats.common.port[s_port].packets++;

  /* get a valid packet */
  uint64_t tsc_start = 0;
  bool time_measure = mt_sessions_time_measure(impl);
  if (time_measure) tsc_start = mt_get_tsc(impl);
  ops->notify_rtp_ready(ops->priv);
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_rtp_us = RTE_MAX(s->stat_max_notify_rtp_us, delta_us);
  }

  MT_USDT_ST40_RX_MBUF_AVAILABLE(s->mgr->idx, s->idx, mbuf, tmstamp, pkt_len);
#ifdef MTL_ENABLE_FUZZING_ST40
  info("%s(%d,%d), fuzz enqueued seq %u len %u\n", __func__, s->idx, s_port, seq_id,
       pkt_len);
#endif
  return 0;
}

#ifdef MTL_ENABLE_FUZZING_ST40
int st_rx_ancillary_session_fuzz_handle_pkt(struct mtl_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s,
                                            struct rte_mbuf* mbuf,
                                            enum mtl_session_port s_port) {
  return rx_ancillary_session_handle_pkt(impl, s, mbuf, s_port);
}

void st_rx_ancillary_session_fuzz_reset(struct st_rx_ancillary_session_impl* s) {
  if (!s) return;

  s->session_seq_id = -1;
  s->tmstamp = -1;
  s->stat_pkts_dropped = 0;
  s->stat_pkts_redundant = 0;
  s->stat_pkts_out_of_order = 0;
  s->stat_pkts_enqueue_fail = 0;
  s->stat_pkts_wrong_pt_dropped = 0;
  s->stat_pkts_wrong_ssrc_dropped = 0;
  s->stat_pkts_received = 0;
  s->stat_last_time = 0;
  s->stat_max_notify_rtp_us = 0;
  s->stat_interlace_first_field = 0;
  s->stat_interlace_second_field = 0;
  s->stat_pkts_wrong_interlace_dropped = 0;
  rte_atomic32_set(&s->stat_frames_received, 0);
  mt_stat_u64_init(&s->stat_time);
  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  memset(s->stat_pkts_out_of_order_per_port, 0,
         sizeof(s->stat_pkts_out_of_order_per_port));

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    s->latest_seq_id[i] = -1;
    s->redundant_error_cnt[i] = 0;
  }
}
#endif

static int rx_ancillary_session_handle_mbuf(void* priv, struct rte_mbuf** mbuf,
                                            uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_ancillary_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_session_port s_port = s_priv->s_port;

  if (!s->attached) {
    dbg("%s(%d,%d), session not ready\n", __func__, s->idx, s_port);
    return -EIO;
  }

  for (uint16_t i = 0; i < nb; i++)
    rx_ancillary_session_handle_pkt(impl, s, mbuf[i], s_port);

  return 0;
}

static int rx_ancillary_session_tasklet(struct st_rx_ancillary_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_ANCILLARY_BURST_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;
  bool done = true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->rxq[s_port]) continue;

    rv = mt_rxq_burst(s->rxq[s_port], &mbuf[0], ST_RX_ANCILLARY_BURST_SIZE);
    if (rv) {
      rx_ancillary_session_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
      rte_pktmbuf_free_bulk(&mbuf[0], rv);
    }

    if (rv) done = false;
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int rx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_rx_ancillary_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_ancillary_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    pending += rx_ancillary_session_tasklet(s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
    rx_ancillary_session_put(mgr, sidx);
  }

  return pending;
}

static int rx_ancillary_session_uinit_hw(struct st_rx_ancillary_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->rxq[i]) {
      mt_rxq_put(s->rxq[i]);
      s->rxq[i] = NULL;
    }
  }

  return 0;
}

static int rx_ancillary_session_init_hw(struct mtl_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  struct mt_rxq_flow flow;
  enum mtl_port port;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].s_port = i;

    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.ip_addr[i], MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow.dip_addr))
      rte_memcpy(flow.sip_addr, s->ops.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    else
      rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st40_dst_port[i];
    if (mt_has_cni_rx(impl, port)) flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;

    /* no flow for data path only */
    if (s->ops.flags & ST40_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), rxq get without flow for port %d as data path only\n", __func__,
           s->idx, i);
      s->rxq[i] = mt_rxq_get(impl, port, NULL);
    } else {
      s->rxq[i] = mt_rxq_get(impl, port, &flow);
    }
    if (!s->rxq[i]) {
      rx_ancillary_session_uinit_hw(s);
      return -EIO;
    }

    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         rx_ancillary_queue_id(s, i), flow.dst_port);
  }

  return 0;
}

static int rx_ancillary_session_uinit_mcast(struct mtl_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!s->mcast_joined[i]) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    mt_mcast_leave(impl, mt_ip_to_u32(ops->ip_addr[i]),
                   mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
  }

  return 0;
}

static int rx_ancillary_session_init_mcast(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  int ret;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!mt_is_multicast_ip(ops->ip_addr[i])) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    if (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), skip mcast join for port %d\n", __func__, s->idx, i);
      return 0;
    }
    ret = mt_mcast_join(impl, mt_ip_to_u32(ops->ip_addr[i]),
                        mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
    if (ret < 0) return ret;
    s->mcast_joined[i] = true;
  }

  return 0;
}

static int rx_ancillary_session_init_sw(struct st_rx_ancillary_sessions_mgr* mgr,
                                        struct st_rx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_RX_ANCILLARY_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
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

static int rx_ancillary_session_uinit_sw(struct st_rx_ancillary_session_impl* s) {
  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  return 0;
}

static int rx_ancillary_session_uinit(struct mtl_main_impl* impl,
                                      struct st_rx_ancillary_session_impl* s) {
  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_sw(s);
  rx_ancillary_session_uinit_hw(s);
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

  s->mgr = mgr;
  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "RX_ANC_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
  }

  s->session_seq_id = -1;
  s->latest_seq_id[MTL_SESSION_PORT_P] = -1;
  s->latest_seq_id[MTL_SESSION_PORT_R] = -1;
  s->tmstamp = -1;
  s->stat_pkts_received = 0;
  s->stat_pkts_dropped = 0;
  s->stat_last_time = mt_get_monotonic_time();
  rte_atomic32_set(&s->stat_frames_received, 0);
  mt_stat_u64_init(&s->stat_time);

  ret = rx_ancillary_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return ret;
  }

  ret = rx_ancillary_session_init_sw(mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_rtps fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return ret;
  }

  ret = rx_ancillary_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return -EIO;
  }

  s->attached = true;
  info("%s(%d), flags 0x%x pt %u, %s\n", __func__, idx, ops->flags, ops->payload_type,
       ops->interlaced ? "interlace" : "progressive");
  return 0;
}

static void rx_ancillary_session_stat(struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->stat_frames_received, 0);

  if (s->stat_pkts_redundant) {
    notice("RX_ANC_SESSION(%d:%s): fps %f frames %d pkts %d (redundant %d)\n", idx,
           s->ops_name, framerate, frames_received, s->stat_pkts_received,
           s->stat_pkts_redundant);
    s->stat_pkts_redundant = 0;
  } else {
    notice("RX_ANC_SESSION(%d:%s): fps %f frames %d pkts %d\n", idx, s->ops_name,
           framerate, frames_received, s->stat_pkts_received);
  }
  s->stat_pkts_received = 0;
  s->stat_last_time = cur_time_ns;

  if (s->stat_pkts_dropped) {
    notice("RX_ANC_SESSION(%d): dropped pkts %d\n", idx, s->stat_pkts_dropped);
    s->stat_pkts_dropped = 0;
  }
  if (s->stat_pkts_out_of_order) {
    warn("RX_ANC_SESSION(%d): out of order pkts %d (%d:%d)\n", idx,
         s->stat_pkts_out_of_order,
         s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_P],
         s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_R]);
    s->stat_pkts_out_of_order = 0;
    s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_P] = 0;
    s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_R] = 0;
  }

  if (s->stat_pkts_wrong_pt_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr payload_type dropped pkts %d\n", idx,
           s->stat_pkts_wrong_pt_dropped);
    s->stat_pkts_wrong_pt_dropped = 0;
  }
  if (s->stat_pkts_wrong_pt_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr ssrc dropped pkts %d\n", idx,
           s->stat_pkts_wrong_pt_dropped);
    s->stat_pkts_wrong_pt_dropped = 0;
  }
  if (s->stat_pkts_wrong_interlace_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr interlace dropped pkts %d\n", idx,
           s->stat_pkts_wrong_interlace_dropped);
    s->stat_pkts_wrong_interlace_dropped = 0;
  }
  if (s->stat_pkts_enqueue_fail) {
    notice("RX_ANC_SESSION(%d): enqueue failed pkts %d\n", idx,
           s->stat_pkts_enqueue_fail);
    s->stat_pkts_enqueue_fail = 0;
  }
  if (s->ops.interlaced) {
    notice("RX_ANC_SESSION(%d): interlace first field %u second field %u\n", idx,
           s->stat_interlace_first_field, s->stat_interlace_second_field);
    s->stat_interlace_first_field = 0;
    s->stat_interlace_second_field = 0;
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("RX_ANC_SESSION(%d): tasklet time avg %.2fus max %.2fus min %.2fus\n", idx,
           (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_notify_rtp_us > 8) {
    notice("RX_ANC_SESSION(%d): notify rtp max %uus\n", idx, s->stat_max_notify_rtp_us);
  }
  s->stat_max_notify_rtp_us = 0;
}

static int rx_ancillary_session_detach(struct mtl_main_impl* impl,
                                       struct st_rx_ancillary_session_impl* s) {
  s->attached = false;
  rx_ancillary_session_stat(s);
  rx_ancillary_session_uinit(impl, s);
  return 0;
}

static int rx_ancillary_session_update_src(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st40_rx_ops* ops = &s->ops;

  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_hw(s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->ip_addr[i], src->ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops->mcast_sip_addr[i], src->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
  }
  /* reset seq id */

  s->session_seq_id = -1;
  s->latest_seq_id[MTL_SESSION_PORT_P] = -1;
  s->latest_seq_id[MTL_SESSION_PORT_R] = -1;
  s->tmstamp = -1;

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

  ret = rx_ancillary_session_update_src(mgr->parent, s, src);
  rx_ancillary_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int st_rx_ancillary_sessions_stat(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct st_rx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_ancillary_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    rx_ancillary_session_stat(s);
    rx_ancillary_session_put(mgr, j);
  }

  return 0;
}

static int rx_ancillary_sessions_mgr_init(struct mtl_main_impl* impl,
                                          struct mtl_sch_impl* sch,
                                          struct st_rx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  mgr->parent = impl;
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

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_rx_ancillary_sessions_stat, mgr, "rx_anc");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_rx_ancillary_session_impl* rx_ancillary_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st40_rx_ops* ops) {
  struct st_rx_ancillary_sessions_mgr* mgr = &sch->rx_anc_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_rx_ancillary_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (!rx_ancillary_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = rx_ancillary_session_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rx_ancillary_session_attach(mgr->parent, mgr, s, ops);
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

  rx_ancillary_session_detach(mgr->parent, s);
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

static int rx_ancillary_sessions_mgr_uinit(struct st_rx_ancillary_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_ancillary_session_impl* s;

  mt_stat_unregister(mgr->parent, st_rx_ancillary_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
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
  uint8_t* ip = NULL;

  if ((num_ports > MTL_SESSION_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->ip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->ip_addr[0], ops->ip_addr[1], MTL_IP_ADDR_LEN)) {
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

  /* Zero means disable the payload_type check */
  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_anc_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->rx_anc_init) return 0;

  /* create rx ancillary context */
  ret = rx_ancillary_sessions_mgr_init(impl, sch, &sch->rx_anc_mgr);
  if (ret < 0) {
    err("%s, rx_ancillary_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  sch->rx_anc_init = true;
  return 0;
}

int st_rx_ancillary_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->rx_anc_init) return 0;

  rx_ancillary_sessions_mgr_uinit(&sch->rx_anc_mgr);

  sch->rx_anc_init = false;
  return 0;
}

st40_rx_handle st40_rx_create(mtl_handle mt, struct st40_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st_rx_ancillary_session_handle_impl* s_impl;
  struct st_rx_ancillary_session_impl* s;
  int ret;
  int quota_mbs;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  quota_mbs = 0;
  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  ret = st_rx_anc_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_anc_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  s = rx_ancillary_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (!s) {
    err("%s, rx_ancillary_sessions_mgr_attach fail\n", __func__);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_RX_ANC;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;
  s_impl->impl = s;
  s->st40_handle = s_impl;

  rte_atomic32_inc(&impl->st40_rx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st40_rx_update_source(st40_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rx_ancillary_sessions_mgr_update_src(&sch->rx_anc_mgr, s, src);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st40_rx_free(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  ret = rx_ancillary_sessions_mgr_detach(&sch->rx_anc_mgr, s);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (ret < 0) err("%s(%d, %d), mgr detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  rx_ancillary_sessions_mgr_update(&sch->rx_anc_mgr);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_rx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
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
  struct st_rx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return;
  }

  s = s_impl->impl;
  MTL_MAY_UNUSED(s);

  if (pkt) rte_pktmbuf_free(pkt);
  MT_USDT_ST40_RX_MBUF_PUT(s->mgr->idx, s->idx, mbuf);
}

int st40_rx_get_queue_meta(st40_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    meta->queue_id[i] = rx_ancillary_queue_id(s, i);
  }

  return 0;
}

int st40_rx_get_session_stats(st40_rx_handle handle, struct st40_rx_user_stats* stats) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_ancillary_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st40_rx_reset_session_stats(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_ancillary_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}
