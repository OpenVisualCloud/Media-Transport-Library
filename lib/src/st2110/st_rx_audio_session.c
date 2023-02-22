/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_audio_session.h"

#include <math.h>

#include "../mt_log.h"

static inline double ra_ebu_pass_rate(struct st_rx_audio_ebu_result* ebu_result,
                                      int pass) {
  return (double)pass * 100 / ebu_result->ebu_result_num;
}

static void rx_audio_session_ebu_result(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_rx_audio_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_audio_ebu_result* ebu_result = &s->ebu_result;

  ebu_result->ebu_result_num -= ebu_info->dropped_results;
  if (ebu_result->ebu_result_num < 0) {
    err("%s, ebu result not enough\n", __func__);
    return;
  }

  critical("st30(%d), [ --- Totla %d ---  Compliance Rate %.2f%% ]\n", idx,
           ebu_result->ebu_result_num,
           ra_ebu_pass_rate(ebu_result, ebu_result->compliance));
  critical(
      "st30(%d), [ Delta Packet vs RTP Pass Rate]\t| Narrow %.2f%% | Wide %.2f%% | Fail "
      "%.2f%% |\n",
      idx, ra_ebu_pass_rate(ebu_result, ebu_result->dpvr_pass_narrow),
      ra_ebu_pass_rate(ebu_result, ebu_result->dpvr_pass_wide),
      ra_ebu_pass_rate(ebu_result, ebu_result->dpvr_fail));
  critical(
      "st30(%d), [ Maximum Timestamped Delay Factor Pass Rate]\t| Pass %.2f%% | Fail "
      "%.2f%% |\n",
      idx, ra_ebu_pass_rate(ebu_result, ebu_result->tsdf_pass),
      ra_ebu_pass_rate(ebu_result, ebu_result->tsdf_fail));
}

static void ra_ebu_clear_result(struct st_rx_audio_ebu_stat* ebu) {
  memset(ebu, 0, sizeof(*ebu));

  ebu->dpvr_max = INT_MIN;
  ebu->dpvr_min = INT_MAX;
  ebu->tsdf_max = INT_MIN;

  ebu->compliant = true;
}

static inline float ra_ebu_calculate_avg(uint32_t cnt, int64_t sum) {
  return cnt ? ((float)sum / cnt) : -1.0f;
}

static char* ra_ebu_dpvr_result(struct st_rx_audio_session_impl* s) {
  struct st_rx_audio_ebu_stat* ebu = &s->ebu;
  struct st_rx_audio_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_audio_ebu_result* ebu_result = &s->ebu_result;

  if (ebu->dpvr_max >= 0 && ebu->dpvr_max < ebu_info->dpvr_max_pass_narrow) {
    ebu_result->dpvr_pass_narrow++;
    return ST_EBU_PASS_NARROW;
  }

  if (ebu->dpvr_max >= 0 && ebu->dpvr_max < ebu_info->dpvr_max_pass_wide &&
      ebu->dpvr_avg >= 0 && ebu->dpvr_avg < ebu_info->dpvr_avg_pass_wide) {
    ebu_result->dpvr_pass_wide++;
    return ST_EBU_PASS_WIDE;
  }

  ebu_result->dpvr_fail++;
  ebu->compliant = false;
  return ST_EBU_FAIL;
}

static char* ra_ebu_tsdf_result(struct st_rx_audio_session_impl* s) {
  struct st_rx_audio_ebu_stat* ebu = &s->ebu;
  struct st_rx_audio_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_audio_ebu_result* ebu_result = &s->ebu_result;

  if (ebu->tsdf_max < ebu_info->tsdf_max_pass) {
    ebu_result->tsdf_pass++;
    return ST_EBU_PASS;
  }
  ebu_result->tsdf_fail++;
  ebu->compliant = false;
  return ST_EBU_FAIL;
}

static void ra_ebu_result(struct st_rx_audio_session_impl* s) {
  struct st_rx_audio_ebu_stat* ebu = &s->ebu;
  struct st_rx_audio_ebu_result* ebu_result = &s->ebu_result;
  int idx = s->idx;

  /* Maximum Timestamped Delay Factor */
  int64_t tsdf = (ebu->dpvr_max - ebu->dpvr_first) - (ebu->dpvr_min - ebu->dpvr_first);
  ebu->tsdf_max = RTE_MAX(tsdf, ebu->tsdf_max);
  ebu->dpvr_first = 0;

  ebu->dpvr_avg = ra_ebu_calculate_avg(ebu->dpvr_cnt, ebu->dpvr_sum);
  /* print every 5 results */
  if (ebu_result->ebu_result_num % 5 == 0) {
    info("%s(%d), Delta Packet vs RTP AVG %.2f (us) MIN %" PRId64 " (us) MAX %" PRId64
         " (us) test %s!\n",
         __func__, idx, ebu->dpvr_avg, ebu->dpvr_min, ebu->dpvr_max,
         ra_ebu_dpvr_result(s));
    info("%s(%d), Maximum Timestamped Delay Factor %" PRIu64 " (us) test %s!\n\n",
         __func__, idx, ebu->tsdf_max, ra_ebu_tsdf_result(s));
  } else {
    ra_ebu_dpvr_result(s);
    ra_ebu_tsdf_result(s);
  }

  if (ebu->compliant) ebu_result->compliance++;
}

static void ra_ebu_on_packet(struct st_rx_audio_session_impl* s, uint32_t rtp_tmstamp,
                             uint64_t pkt_tmstamp) {
  struct st_rx_audio_ebu_stat* ebu = &s->ebu;
  struct st_rx_audio_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_audio_ebu_result* ebu_result = &s->ebu_result;

  uint64_t epochs = (double)pkt_tmstamp / ebu_info->frame_time;
  uint64_t epoch_tmstamp = (double)epochs * ebu_info->frame_time;
  double fpt_delta = (double)pkt_tmstamp - epoch_tmstamp;
  uint64_t tmstamp64 = epochs * ebu_info->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;
  double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;
  double diff_rtp_ts_ns =
      diff_rtp_ts * ebu_info->frame_time / ebu_info->frame_time_sampling;
  double latency = fpt_delta - diff_rtp_ts_ns;
  double dpvr = latency / 1000;

  ebu->pkt_num++;

  if (ebu->pkt_num % 1000 == 0) {
    ebu_result->ebu_result_num++;
    /* every second (for 1ms/packet) */
    if (ebu_result->ebu_result_num > ebu_info->dropped_results) ra_ebu_result(s);
    ra_ebu_clear_result(ebu);
  }

  /* calculate Delta Packet vs RTP */
  ebu->dpvr_sum += dpvr;
  ebu->dpvr_min = RTE_MIN(dpvr, ebu->dpvr_min);
  ebu->dpvr_max = RTE_MAX(dpvr, ebu->dpvr_max);
  ebu->dpvr_cnt++;

  if (!ebu->dpvr_first) ebu->dpvr_first = dpvr;
}

static int ra_ebu_init(struct mtl_main_impl* impl, struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_rx_audio_ebu_info* ebu_info = &s->ebu_info;
  struct st30_rx_ops* ops = &s->ops;

  ra_ebu_clear_result(&s->ebu);

  int sampling = (ops->sampling == ST30_SAMPLING_48K) ? 48 : 96;
  ebu_info->frame_time = (double)1000000000.0 * 1 / 1000; /* 1ms, in ns */
  ebu_info->frame_time_sampling = (double)(sampling * 1000) * 1 / 1000;

  ebu_info->dpvr_max_pass_narrow = 3 * ebu_info->frame_time / 1000; /* in us */
  ebu_info->dpvr_max_pass_wide = 20 * ebu_info->frame_time / 1000;  /* in us */
  ebu_info->dpvr_avg_pass_wide = 2.5 * ebu_info->frame_time / 1000; /* in us */
  ebu_info->tsdf_max_pass = 17 * ebu_info->frame_time / 1000;       /* in us */

  ebu_info->dropped_results = 10; /* we drop first 10 results */

  info("%s[%02d], Delta Packet vs RTP Pass Criteria(narrow) min %d (us) max %d (us)\n",
       __func__, idx, 0, ebu_info->dpvr_max_pass_narrow);
  info("%s[%02d], Delta Packet vs RTP Pass Criteria(wide) max %d (us) avg %.2f (us)\n",
       __func__, idx, ebu_info->dpvr_max_pass_wide, ebu_info->dpvr_avg_pass_wide);
  info("%s[%02d], Maximum Timestamped Delay Factor Pass Criteria %d (us)\n", __func__,
       idx, ebu_info->tsdf_max_pass);

  return 0;
}

/* call rx_audio_session_put always if get successfully */
static inline struct st_rx_audio_session_impl* rx_audio_session_get(
    struct st_rx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_audio_session_put always if get successfully */
static inline struct st_rx_audio_session_impl* rx_audio_session_try_get(
    struct st_rx_audio_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_audio_session_put always if get successfully */
static inline bool rx_audio_session_get_empty(struct st_rx_audio_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_audio_session_put(struct st_rx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static void* rx_audio_session_get_frame(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame_info = &s->st30_frames[i];

    if (0 == rte_atomic32_read(&frame_info->refcnt)) {
      dbg("%s(%d), find frame at %d\n", __func__, idx, i);
      rte_atomic32_inc(&frame_info->refcnt);
      return frame_info->addr;
    }
  }

  err("%s(%d), no free frame\n", __func__, idx);
  return NULL;
}

static int rx_audio_session_put_frame(struct st_rx_audio_session_impl* s, void* frame) {
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame_info = &s->st30_frames[i];
    if (frame_info->addr == frame) {
      dbg("%s(%d), put frame at %d\n", __func__, idx, i);
      rte_atomic32_dec(&frame_info->refcnt);
      return 0;
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, idx, frame);
  return -EIO;
}

static int rx_audio_session_free_frames(struct st_rx_audio_session_impl* s) {
  /* free frames */
  if (s->st30_frames) {
    if (s->st30_cur_frame) {
      rx_audio_session_put_frame(s, s->st30_cur_frame);
      s->st30_cur_frame = NULL;
    }
    struct st_frame_trans* frame;
    for (int i = 0; i < s->st30_frames_cnt; i++) {
      frame = &s->st30_frames[i];
      st_frame_trans_uinit(frame);
    }
    mt_rte_free(s->st30_frames);
    s->st30_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rx_audio_session_alloc_frames(struct mtl_main_impl* impl,
                                         struct st_rx_audio_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx;
  size_t size = s->st30_frame_size;
  struct st_frame_trans* st30_frame;
  void* frame;

  s->st30_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st30_frames) * s->st30_frames_cnt, soc_id);
  if (!s->st30_frames) {
    err("%s(%d), st30_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    st30_frame = &s->st30_frames[i];
    rte_atomic32_set(&st30_frame->refcnt, 0);
    st30_frame->idx = i;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    st30_frame = &s->st30_frames[i];

    frame = mt_rte_zmalloc_socket(size, soc_id);
    if (!frame) {
      err("%s(%d), frame malloc %" PRIu64 " fail for %d\n", __func__, idx, size, i);
      rx_audio_session_free_frames(s);
      return -ENOMEM;
    }
    st30_frame->flags = ST_FT_FLAG_RTE_MALLOC;
    st30_frame->addr = frame;
    st30_frame->iova = rte_malloc_virt2iova(frame);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_free_rtps(struct st_rx_audio_session_impl* s) {
  if (s->st30_rtps_ring) {
    mt_ring_dequeue_clean(s->st30_rtps_ring);
    rte_ring_free(s->st30_rtps_ring);
    s->st30_rtps_ring = NULL;
  }

  return 0;
}

static int rx_audio_session_alloc_rtps(struct mtl_main_impl* impl,
                                       struct st_rx_audio_sessions_mgr* mgr,
                                       struct st_rx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-AUDIO-RTP-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  if (count <= 0) {
    err("%s(%d,%d), invalid rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
    return -ENOMEM;
  }
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->st30_rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_audio_sessions_tasklet_start(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_sessions_tasklet_stop(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_handle_frame_pkt(struct mtl_main_impl* impl,
                                             struct st_rx_audio_session_impl* s,
                                             struct rte_mbuf* mbuf,
                                             enum mtl_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint8_t payload_type = rtp->payload_type;

  if (payload_type != ops->payload_type) {
    s->st30_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* set first seq_id - 1 */
  if (unlikely(s->st30_seq_id == -1)) s->st30_seq_id = seq_id - 1;
  /* drop old packet */
  if (st_rx_seq_drop(seq_id, s->st30_seq_id, 5)) {
    dbg("%s(%d,%d), drop as pkt seq %d is old\n", __func__, s->idx, s_port, seq_id);
    s->st30_stat_pkts_dropped++;
    return -EIO;
  }
  /* update seq id */
  s->st30_seq_id = seq_id;

  // copy frame
  if (!s->st30_cur_frame) {
    s->st30_cur_frame = rx_audio_session_get_frame(s);
    if (!s->st30_cur_frame) {
      dbg("%s(%d,%d), seq %d drop as frame run out\n", __func__, s->idx, s_port, seq_id);
      s->st30_stat_pkts_dropped++;
      return -EIO;
    }
  }
  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  if ((offset + s->pkt_len) > s->st30_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st30_frame_size);
    s->st30_stat_pkts_dropped++;
    return -EIO;
  }
  rte_memcpy(s->st30_cur_frame + offset, payload, s->pkt_len);
  s->frame_recv_size += s->pkt_len;
  s->st30_stat_pkts_received++;
  s->st30_pkt_idx++;

  if (mt_has_ebu(impl) && inf->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    ra_ebu_on_packet(s, tmstamp, mt_mbuf_hw_time_stamp(impl, mbuf, port));
  }

  // notify frame done
  if (s->frame_recv_size >= s->st30_frame_size) {
    struct st30_rx_frame_meta* meta = &s->meta;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = tmstamp;
    meta->fmt = ops->fmt;
    meta->sampling = ops->sampling;
    meta->channel = ops->channel;

    /* get a full frame */
    int ret = -EIO;
    if (ops->notify_frame_ready)
      ret = ops->notify_frame_ready(ops->priv, s->st30_cur_frame, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      rx_audio_session_put_frame(s, s->st30_cur_frame);
    }
    s->frame_recv_size = 0;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->st30_stat_frames_received);
    s->st30_cur_frame = NULL;
    dbg("%s: full frame on %p\n", __func__, s->st30_frame);
  }
  return 0;
}

static int rx_audio_session_handle_rtp_pkt(struct mtl_main_impl* impl,
                                           struct st_rx_audio_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum mtl_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint8_t payload_type = rtp->payload_type;

  if (payload_type != ops->payload_type) {
    s->st30_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* set first seq_id - 1 */
  if (unlikely(s->st30_seq_id == -1)) s->st30_seq_id = seq_id - 1;
  /* drop old packet */
  if (st_rx_seq_drop(seq_id, s->st30_seq_id, 5)) {
    dbg("%s(%d,%d), drop as pkt seq %d is old\n", __func__, s->idx, s_port, seq_id);
    s->st30_stat_pkts_dropped++;
    return -EIO;
  }
  /* update seq id */
  s->st30_seq_id = seq_id;

  /* enqueue the packet ring to app */
  int ret = rte_ring_sp_enqueue(s->st30_rtps_ring, (void*)mbuf);
  if (ret < 0) {
    dbg("%s(%d,%d), drop as rtps ring full, seq id %d\n", __func__, seq_id, s_port);
    s->st30_stat_pkts_rtp_ring_full++;
    return -EIO;
  }
  rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */

  ops->notify_rtp_ready(ops->priv);
  s->st30_stat_pkts_received++;

  if (mt_has_ebu(impl) && inf->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    ra_ebu_on_packet(s, tmstamp, mt_mbuf_hw_time_stamp(impl, mbuf, port));
  }

  return 0;
}

static int rx_audio_session_handle_mbuf(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_audio_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_port s_port = s_priv->port;
  enum st30_type st30_type = s->ops.type;

  if (ST30_TYPE_FRAME_LEVEL == st30_type) {
    for (uint16_t i = 0; i < nb; i++)
      rx_audio_session_handle_frame_pkt(impl, s, mbuf[i], s_port);
  } else {
    for (uint16_t i = 0; i < nb; i++) {
      rx_audio_session_handle_rtp_pkt(impl, s, mbuf[i], s_port);
    }
  }

  return 0;
}

static int rx_audio_session_tasklet(struct mtl_main_impl* impl,
                                    struct st_rx_audio_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_AUDIO_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;

  bool done = true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (s->rss[s_port]) {
      rv = mt_rss_burst(s->rss[s_port], ST_RX_AUDIO_BURTS_SIZE);
    } else if (s->queue[s_port]) {
      rv = mt_dev_rx_burst(s->queue[s_port], &mbuf[0], ST_RX_AUDIO_BURTS_SIZE);
      if (rv) {
        rx_audio_session_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
        rte_pktmbuf_free_bulk(&mbuf[0], rv);
      }
    } else {
      continue;
    }

    if (rv) done = false;
  }

  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static int rx_audio_sessions_tasklet_handler(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parnet;
  struct st_rx_audio_session_impl* s;
  int pending = MT_TASKLET_ALL_DONE;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_audio_session_try_get(mgr, sidx);
    if (!s) continue;

    pending += rx_audio_session_tasklet(impl, s);
    rx_audio_session_put(mgr, sidx);
  }

  return pending;
}

static int rx_audio_session_uinit_hw(struct mtl_main_impl* impl,
                                     struct st_rx_audio_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->queue[i]) {
      mt_dev_put_rx_queue(impl, s->queue[i]);
      s->queue[i] = NULL;
    }
  }

  return 0;
}

static int rx_audio_session_init_hw(struct mtl_main_impl* impl,
                                    struct st_rx_audio_session_impl* s) {
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
    flow.dst_port = s->st30_dst_port[i];

    /* no flow for data path only */
    if (mt_pmd_is_kernel(impl, port) && (s->ops.flags & ST30_RX_FLAG_DATA_PATH_ONLY))
      s->queue[i] = mt_dev_get_rx_queue(impl, port, NULL);
    else if (mt_has_rss(impl, port)) {
      flow.priv = &s->priv[i];
      flow.cb = rx_audio_session_handle_mbuf;
      s->rss[i] = mt_rss_get(impl, port, &flow);
    } else
      s->queue[i] = mt_dev_get_rx_queue(impl, port, &flow);
    if (!s->queue[i] && !s->rss[i]) {
      rx_audio_session_uinit_hw(impl, s);
      return -EIO;
    }

    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         mt_has_rss(impl, port) ? mt_rss_queue_id(s->rss[i])
                                : mt_dev_rx_queue_id(s->queue[i]),
         flow.dst_port);
  }

  return 0;
}

static int rx_audio_session_uinit_mcast(struct mtl_main_impl* impl,
                                        struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (mt_is_multicast_ip(ops->sip_addr[i]))
      mt_mcast_leave(impl, mt_ip_to_u32(ops->sip_addr[i]),
                     mt_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rx_audio_session_init_mcast(struct mtl_main_impl* impl,
                                       struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;
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

static int rx_audio_session_uinit_sw(struct mtl_main_impl* impl,
                                     struct st_rx_audio_session_impl* s) {
  rx_audio_session_free_frames(s);
  rx_audio_session_free_rtps(s);
  return 0;
}

static int rx_audio_session_init_sw(struct mtl_main_impl* impl,
                                    struct st_rx_audio_sessions_mgr* mgr,
                                    struct st_rx_audio_session_impl* s) {
  enum st30_type st30_type = s->ops.type;
  int idx = s->idx;
  int ret;

  switch (st30_type) {
    case ST30_TYPE_FRAME_LEVEL:
      ret = rx_audio_session_alloc_frames(impl, s);
      break;
    case ST30_TYPE_RTP_LEVEL:
      ret = rx_audio_session_alloc_rtps(impl, mgr, s);
      break;
    default:
      err("%s(%d), error st30_type %d\n", __func__, idx, st30_type);
      return -EIO;
  }
  if (ret < 0) return ret;

  return 0;
}

static int rx_audio_session_attach(struct mtl_main_impl* impl,
                                   struct st_rx_audio_sessions_mgr* mgr,
                                   struct st_rx_audio_session_impl* s,
                                   struct st30_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx);
    s->st30_dst_port[i] = s->st30_src_port[i];
  }

  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr);
  s->pkt_len = ops->sample_size * ops->sample_num * ops->channel;
  s->st30_pkt_size = s->pkt_len + sizeof(struct st_rfc3550_audio_hdr);
  if (s->pkt_len > bytes_in_pkt) {
    err("%s(%d), invalid pkt_len %d\n", __func__, idx, s->pkt_len);
    return -EIO;
  }

  s->st30_frames_cnt = ops->framebuff_cnt;
  s->st30_total_pkts = ops->framebuff_size / s->pkt_len;
  if (ops->framebuff_size % s->pkt_len) {
    /* todo: add the support? */
    err("%s(%d), framebuff_size %d not multiple pkt_len %d\n", __func__, idx, s->pkt_len,
        ops->framebuff_size);
    return -EIO;
  }
  s->st30_pkt_idx = 0;
  s->st30_frame_size = ops->framebuff_size;

  s->st30_seq_id = -1;
  s->st30_stat_pkts_received = 0;
  s->st30_stat_pkts_dropped = 0;
  s->st30_stat_pkts_wrong_hdr_dropped = 0;
  s->st30_stat_frames_dropped = 0;
  rte_atomic32_set(&s->st30_stat_frames_received, 0);
  s->st30_stat_last_time = mt_get_monotonic_time();

  if (mt_has_ebu(impl)) {
    ret = ra_ebu_init(impl, s);
    if (ret < 0) {
      err("%s(%d), ra_ebu_init fail %d\n", __func__, idx, ret);
      return -EIO;
    }
  }

  ret = rx_audio_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  ret = rx_audio_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_sw fail %d\n", __func__, idx, ret);
    rx_audio_session_uinit_hw(impl, s);
    return -EIO;
  }

  ret = rx_audio_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_audio_session_uinit_sw(impl, s);
    rx_audio_session_uinit_hw(impl, s);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static void rx_audio_session_stat(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st30_stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->st30_stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->st30_stat_frames_received, 0);

  notice("RX_AUDIO_SESSION(%d:%s): fps %f, st30 received frames %d, received pkts %d\n",
         idx, s->ops_name, framerate, frames_received, s->st30_stat_pkts_received);
  s->st30_stat_pkts_received = 0;
  s->st30_stat_last_time = cur_time_ns;

  if (s->st30_stat_frames_dropped || s->st30_stat_pkts_dropped) {
    notice("RX_AUDIO_SESSION(%d): st30 dropped frames %d, dropped pkts %d\n", idx,
           s->st30_stat_frames_dropped, s->st30_stat_pkts_dropped);
    s->st30_stat_frames_dropped = 0;
    s->st30_stat_pkts_dropped = 0;
  }
  if (s->st30_stat_pkts_wrong_hdr_dropped) {
    notice("RX_AUDIO_SESSION(%d): wrong hdr dropped pkts %d\n", idx,
           s->st30_stat_pkts_wrong_hdr_dropped);
    s->st30_stat_pkts_wrong_hdr_dropped = 0;
  }
}

static int rx_audio_session_detach(struct mtl_main_impl* impl,
                                   struct st_rx_audio_session_impl* s) {
  if (mt_has_ebu(impl)) rx_audio_session_ebu_result(s);
  rx_audio_session_stat(s);
  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_sw(impl, s);
  rx_audio_session_uinit_hw(impl, s);
  return 0;
}

static int rx_audio_session_update_src(struct mtl_main_impl* impl,
                                       struct st_rx_audio_session_impl* s,
                                       struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st30_rx_ops* ops = &s->ops;

  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st30_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx);
    s->st30_dst_port[i] = s->st30_src_port[i];
  }
  /* reset seq id */
  s->st30_seq_id = -1;

  ret = rx_audio_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rx_audio_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_audio_sessions_mgr_update_src(struct st_rx_audio_sessions_mgr* mgr,
                                            struct st_rx_audio_session_impl* s,
                                            struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rx_audio_session_update_src(mgr->parnet, s, src);
  rx_audio_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_audio_sessions_mgr_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch,
                                      struct st_rx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mt_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_audio_sessions_mgr";
  ops.start = rx_audio_sessions_tasklet_start;
  ops.stop = rx_audio_sessions_tasklet_stop;
  ops.handler = rx_audio_sessions_tasklet_handler;

  mgr->tasklet = mt_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mt_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_init(struct mtl_main_impl* impl,
                                 struct st_rx_audio_sessions_mgr* mgr,
                                 struct st_rx_audio_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static struct st_rx_audio_session_impl* rx_audio_sessions_mgr_attach(
    struct st_rx_audio_sessions_mgr* mgr, struct st30_rx_ops* ops) {
  int midx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parnet;
  int ret;
  struct st_rx_audio_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    if (!rx_audio_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, MTL_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      return NULL;
    }
    ret = rx_audio_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rx_audio_session_attach(mgr->parnet, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_audio_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int rx_audio_sessions_mgr_detach(struct st_rx_audio_sessions_mgr* mgr,
                                        struct st_rx_audio_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rx_audio_session_detach(mgr->parnet, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  rx_audio_session_put(mgr, idx);

  return 0;
}

static int rx_audio_sessions_mgr_update(struct st_rx_audio_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

int st_rx_audio_sessions_mgr_uinit(struct st_rx_audio_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_audio_session_impl* s;

  if (mgr->tasklet) {
    mt_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    s = rx_audio_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rx_audio_sessions_mgr_detach(mgr, s);
    rx_audio_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

void st_rx_audio_sessions_stat(struct mtl_main_impl* impl) {
  struct st_rx_audio_sessions_mgr* mgr = &impl->rx_a_mgr;
  struct st_rx_audio_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_audio_session_get(mgr, j);
    if (!s) continue;
    rx_audio_session_stat(s);
    rx_audio_session_put(mgr, j);
  }
}

static int rx_audio_ops_check(struct st30_rx_ops* ops) {
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

  if (ops->type == ST30_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST30_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (ops->sample_size > MTL_PKT_MAX_RTP_BYTES) {
      err("%s, invalid sample_size %d\n", __func__, ops->sample_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_audio_init(struct mtl_main_impl* impl) {
  int ret;

  if (impl->rx_a_init) return 0;

  /* create rx audio context */
  ret = rx_audio_sessions_mgr_init(impl, impl->main_sch, &impl->rx_a_mgr);
  if (ret < 0) {
    err("%s, rx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  impl->rx_a_init = true;
  return 0;
}

st30_rx_handle st30_rx_create(mtl_handle mt, struct st30_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mt_sch_impl* sch = impl->main_sch;
  struct st_rx_audio_session_handle_impl* s_impl;
  struct st_rx_audio_session_impl* s;
  int ret;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  ret = st_rx_audio_init(impl);
  mt_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), mt_socket_id(impl, MTL_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  s = rx_audio_sessions_mgr_attach(&impl->rx_a_mgr, ops);
  mt_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
  if (!s) {
    err("%s(%d), rx_audio_sessions_mgr_attach fail\n", __func__, sch->idx);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = MT_HANDLE_RX_AUDIO;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st30_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st30_rx_update_source(st30_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_audio_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rx_audio_sessions_mgr_update_src(&impl->rx_a_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st30_rx_free(st30_rx_handle handle) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_audio_session_impl* s;
  int ret, idx;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = rx_audio_sessions_mgr_detach(&impl->rx_a_mgr, s);
  if (ret < 0) err("%s(%d), st_rx_audio_sessions_mgr_deattach fail\n", __func__, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  rx_audio_sessions_mgr_update(&impl->rx_a_mgr);
  mt_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st30_rx_put_framebuff(st30_rx_handle handle, void* frame) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return rx_audio_session_put_frame(s, frame);
}

void* st30_rx_get_mbuf(st30_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct rte_mbuf* pkt;
  struct rte_ring* rtps_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->st30_rtps_ring;
  if (!rtps_ring) {
    err("%s(%d), rtp ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(rtps_ring, (void**)&pkt);
  if (ret < 0) {
    dbg("%s(%d), rtp ring is empty\n", __func__, idx);
    return NULL;
  }

  size_t hdr_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                   sizeof(struct rte_udp_hdr);
  *len = pkt->data_len - hdr_len;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  return pkt;
}

void st30_rx_put_mbuf(st30_rx_handle handle, void* mbuf) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_HANDLE_RX_AUDIO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st30_rx_get_queue_meta(st30_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct mtl_main_impl* impl;
  enum mtl_port port;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
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
