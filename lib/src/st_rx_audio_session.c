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

#include "st_rx_audio_session.h"

#include <math.h>

#include "st_dev.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_sch.h"
#include "st_util.h"

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
    info("%s(%d), Delta Packet vs RTP AVG %.2f (us) MIN %ld (us) MAX %ld (us) test %s!\n",
         __func__, idx, ebu->dpvr_avg, ebu->dpvr_min, ebu->dpvr_max,
         ra_ebu_dpvr_result(s));
    info("%s(%d), Maximum Timestamped Delay Factor %ld (us) test %s!\n\n", __func__, idx,
         ebu->tsdf_max, ra_ebu_tsdf_result(s));
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

static int ra_ebu_init(struct st_main_impl* impl, struct st_rx_audio_session_impl* s) {
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

static inline void rx_audio_session_lock(struct st_rx_audio_sessions_mgr* mgr, int sidx) {
  rte_spinlock_lock(&mgr->mutex[sidx]);
}

static inline int rx_audio_session_try_lock(struct st_rx_audio_sessions_mgr* mgr,
                                            int sidx) {
  return rte_spinlock_trylock(&mgr->mutex[sidx]);
}

static inline void rx_audio_session_unlock(struct st_rx_audio_sessions_mgr* mgr,
                                           int sidx) {
  rte_spinlock_unlock(&mgr->mutex[sidx]);
}

static void* rx_audio_session_get_frame(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    if (0 == rte_atomic32_read(&s->st30_frames_refcnt[i])) {
      dbg("%s(%d), find frame at %d\n", __func__, idx, i);
      rte_atomic32_inc(&s->st30_frames_refcnt[i]);
      return s->st30_frames[i];
    }
  }

  err("%s(%d), no free frame\n", __func__, idx);
  return NULL;
}

int st_rx_audio_session_put_frame(struct st_rx_audio_session_impl* s, void* frame) {
  int idx = s->idx;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    if (s->st30_frames[i] == frame) {
      dbg("%s(%d), put frame at %d\n", __func__, idx, i);
      rte_atomic32_dec(&s->st30_frames_refcnt[i]);
      return 0;
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, idx, frame);
  return -EIO;
}

static int rx_audio_session_free_frames(struct st_rx_audio_session_impl* s) {
  /* free frames */
  if (s->st30_frames) {
    for (int i = 0; i < s->st30_frames_cnt; i++) {
      if (s->st30_frames[i]) {
        st_rte_free(s->st30_frames[i]);
        s->st30_frames[i] = NULL;
      }
    }
    st_rte_free(s->st30_frames);
    s->st30_frames = NULL;
  }
  if (s->st30_frames_refcnt) {
    st_rte_free(s->st30_frames_refcnt);
    s->st30_frames_refcnt = NULL;
  }
  s->st30_frames_cnt = 0;

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rx_audio_session_alloc_frames(struct st_main_impl* impl,
                                         struct st_rx_audio_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t size = s->st30_frame_size;
  void* frame;

  s->st30_frames = st_rte_zmalloc_socket(sizeof(void*) * s->st30_frames_cnt, soc_id);
  if (!s->st30_frames) {
    err("%s(%d), st21_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  s->st30_frames_refcnt =
      st_rte_zmalloc_socket(sizeof(rte_atomic32_t) * s->st30_frames_cnt, soc_id);
  if (!s->st30_frames_refcnt) {
    err("%s(%d), st30_frames_refcnt alloc fail\n", __func__, idx);
    rx_audio_session_free_frames(s);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame = st_rte_zmalloc_socket(size, soc_id);
    if (!frame) {
      err("%s(%d), frame malloc %" PRIu64 " fail\n", __func__, idx, size);
      rx_audio_session_free_frames(s);
      return -ENOMEM;
    }
    s->st30_frames[i] = frame;
    rte_atomic32_set(&s->st30_frames_refcnt[i], 0);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_free_rtps(struct st_rx_audio_session_impl* s) {
  if (s->st30_rtps_ring) {
    st_ring_dequeue_clean(s->st30_rtps_ring);
    rte_ring_free(s->st30_rtps_ring);
    s->st30_rtps_ring = NULL;
  }

  return 0;
}

static int rx_audio_session_alloc_rtps(struct st_main_impl* impl,
                                       struct st_rx_audio_sessions_mgr* mgr,
                                       struct st_rx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-AUDIO-RTP-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  if (count <= 0) {
    err("%s(%d,%d), invalid rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
    return -ENOMEM;
  }
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->st30_rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_audio_session_init(struct st_main_impl* impl,
                                 struct st_rx_audio_sessions_mgr* mgr,
                                 struct st_rx_audio_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int rx_audio_session_uinit(struct st_main_impl* impl,
                                  struct st_rx_audio_session_impl* s) {
  dbg("%s(%d), succ\n", __func__, s->idx);
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

static int rx_audio_session_handle_frame_pkt(struct st_main_impl* impl,
                                             struct st_rx_audio_session_impl* s,
                                             struct rte_mbuf* mbuf,
                                             enum st_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st_interface* inf = st_if(impl, port);
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);

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
  if (!s->st30_frame) {
    s->st30_frame = rx_audio_session_get_frame(s);
    if (!s->st30_frame) {
      dbg("%s(%d,%d), seq %d drop as frame run out\n", __func__, s->idx, s_port, seq_id);
      s->st30_stat_pkts_dropped++;
      return -EIO;
    }
  }
  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  rte_memcpy(s->st30_frame + offset, payload, s->pkt_len);
  s->frame_recv_size += s->pkt_len;
  s->st30_stat_pkts_received++;
  s->st30_pkt_idx++;

  if (st_has_ebu(impl) && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    ra_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf));
  }

  // notify frame done
  if (s->frame_recv_size >= s->st30_frame_size) {
    struct st30_frame_meta* meta = &s->meta;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = tmstamp;
    meta->fmt = ops->fmt;
    meta->sampling = ops->sampling;
    meta->channel = ops->channel;

    /* get a full frame */
    int ret = -EIO;
    if (ops->notify_frame_ready)
      ret = ops->notify_frame_ready(ops->priv, s->st30_frame, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      st_rx_audio_session_put_frame(s, s->st30_frame);
    }
    s->frame_recv_size = 0;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->st30_stat_frames_received);
    s->st30_frame = NULL;
    dbg("%s: full frame on %p\n", __func__, s->st30_frame);
  }
  return 0;
}

static int rx_audio_session_handle_rtp_pkt(struct st_main_impl* impl,
                                           struct st_rx_audio_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum st_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st_interface* inf = st_if(impl, port);
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);

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

  ops->notify_rtp_ready(ops->priv);
  s->st30_stat_pkts_received++;

  if (st_has_ebu(impl) && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    ra_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf));
  }

  return 0;
}

static int rx_audio_session_tasklet(struct st_main_impl* impl,
                                    struct st_rx_audio_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_AUDIO_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port, ret;
  enum st30_type st30_type = s->ops.type;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->queue_active[s_port]) continue;
    rv = rte_eth_rx_burst(s->port_id[s_port], s->queue_id[s_port], &mbuf[0],
                          ST_RX_AUDIO_BURTS_SIZE);
    if (rv > 0) {
      if (ST30_TYPE_FRAME_LEVEL == st30_type) {
        for (uint16_t i = 0; i < rv; i++)
          rx_audio_session_handle_frame_pkt(impl, s, mbuf[i], s_port);
        rte_pktmbuf_free_bulk(&mbuf[0], rv);
      } else {
        struct rte_mbuf* free_mbuf[ST_RX_AUDIO_BURTS_SIZE];
        int free_mbuf_cnt = 0;

        for (uint16_t i = 0; i < rv; i++) {
          ret = rx_audio_session_handle_rtp_pkt(impl, s, mbuf[i], s_port);
          if (ret < 0) { /* set to free if it is dropped pkt */
            free_mbuf[free_mbuf_cnt] = mbuf[i];
            free_mbuf_cnt++;
          }
        }
        rte_pktmbuf_free_bulk(&free_mbuf[0], free_mbuf_cnt);
      }
    }
  }

  return 0;
}

static int rx_audio_sessions_tasklet_handler(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_rx_audio_session_impl* s;
  int sidx;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    if (rx_audio_session_try_lock(mgr, sidx)) {
      if (mgr->active[sidx]) {
        s = &mgr->sessions[sidx];
        rx_audio_session_tasklet(impl, s);
      }
      rx_audio_session_unlock(mgr, sidx);
    }
  }

  return 0;
}

static int rx_audio_session_uinit_hw(struct st_main_impl* impl,
                                     struct st_rx_audio_session_impl* s) {
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

static int rx_audio_session_init_hw(struct st_main_impl* impl,
                                    struct st_rx_audio_session_impl* s) {
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
    flow.dst_port = s->st30_dst_port[i];

    ret = st_dev_request_rx_queue(impl, port, &queue, &flow);
    if (ret < 0) {
      rx_audio_session_uinit_hw(impl, s);
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

static int rx_audio_session_uinit_mcast(struct st_main_impl* impl,
                                        struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (st_is_multicast_ip(ops->sip_addr[i]))
      st_mcast_leave(impl, *(uint32_t*)ops->sip_addr[i],
                     st_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rx_audio_session_init_mcast(struct st_main_impl* impl,
                                       struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;
  int ret;

  for (int i = 0; i < ops->num_port; i++) {
    if (!st_is_multicast_ip(ops->sip_addr[i])) continue;
    ret = st_mcast_join(impl, *(uint32_t*)ops->sip_addr[i],
                        st_port_logic2phy(s->port_maps, i));
    if (ret < 0) return ret;
  }

  return 0;
}

static int rx_audio_session_uinit_sw(struct st_main_impl* impl,
                                     struct st_rx_audio_session_impl* s) {
  rx_audio_session_free_frames(s);
  rx_audio_session_free_rtps(s);
  return 0;
}

static int rx_audio_session_init_sw(struct st_main_impl* impl,
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

static int rx_audio_session_attach(struct st_main_impl* impl,
                                   struct st_rx_audio_sessions_mgr* mgr,
                                   struct st_rx_audio_session_impl* s,
                                   struct st30_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx);
    s->st30_dst_port[i] = s->st30_src_port[i];
  }

  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr);
  s->pkt_len = s->ops.sample_size;
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
  s->st30_stat_frames_dropped = 0;
  rte_atomic32_set(&s->st30_stat_frames_received, 0);
  s->st30_stat_last_time = st_get_monotonic_time();

  if (st_has_ebu(impl)) {
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
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st30_stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->st30_stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->st30_stat_frames_received, 0);

  info("RX_AUDIO_SESSION(%d): fps %f, st30 received frames %d, received pkts %d\n", idx,
       framerate, frames_received, s->st30_stat_pkts_received);
  s->st30_stat_pkts_received = 0;
  s->st30_stat_last_time = cur_time_ns;

  if (s->st30_stat_frames_dropped || s->st30_stat_pkts_dropped) {
    info("RX_AUDIO_SESSION(%d): st30 dropped frames %d, dropped pkts %d\n", idx,
         s->st30_stat_frames_dropped, s->st30_stat_pkts_dropped);
    s->st30_stat_frames_dropped = 0;
    s->st30_stat_pkts_dropped = 0;
  }
}

static int rx_audio_session_detach(struct st_main_impl* impl,
                                   struct st_rx_audio_session_impl* s) {
  if (st_has_ebu(impl)) rx_audio_session_ebu_result(s);
  rx_audio_session_stat(s);
  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_sw(impl, s);
  rx_audio_session_uinit_hw(impl, s);
  return 0;
}

static int rx_audio_session_update_src(struct st_main_impl* impl,
                                       struct st_rx_audio_session_impl* s,
                                       struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st30_rx_ops* ops = &s->ops;

  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], ST_IP_ADDR_LEN);
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

int st_rx_audio_sessions_mgr_update_src(struct st_rx_audio_sessions_mgr* mgr,
                                        struct st_rx_audio_session_impl* s,
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

  rx_audio_session_lock(mgr, sidx);
  ret = rx_audio_session_update_src(mgr->parnet, s, src);
  rx_audio_session_unlock(mgr, sidx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, sidx, ret);
    return ret;
  }

  return 0;
}

int st_rx_audio_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_rx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  int ret;
  struct st_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
    ret = rx_audio_session_init(impl, mgr, &mgr->sessions[i], i);
    if (ret < 0) {
      err("%s(%d), rx_audio_session_init fail %d for %d\n", __func__, idx, ret, i);
      return ret;
    }
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_audio_sessions_mgr";
  ops.start = rx_audio_sessions_tasklet_start;
  ops.stop = rx_audio_sessions_tasklet_stop;
  ops.handler = rx_audio_sessions_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    err("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_rx_audio_sessions_mgr_uinit(struct st_rx_audio_sessions_mgr* mgr) {
  int idx = mgr->idx;
  int ret, i;
  struct st_rx_audio_session_impl* s;

  for (i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    s = &mgr->sessions[i];

    if (mgr->active[i]) { /* make sure all session are detached*/
      warn("%s(%d), session %d still attached\n", __func__, idx, i);
      ret = st_rx_audio_sessions_mgr_detach(mgr, s);
      if (ret < 0) {
        err("%s(%d), st_rx_audio_sessions_mgr_detach fail %d for %d\n", __func__, idx,
            ret, i);
      }
    }

    ret = rx_audio_session_uinit(mgr->parnet, s);
    if (ret < 0) {
      err("%s(%d), st_rx_audio_session_uinit fail %d for %d\n", __func__, idx, ret, i);
    }
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

struct st_rx_audio_session_impl* st_rx_audio_sessions_mgr_attach(
    struct st_rx_audio_sessions_mgr* mgr, struct st30_rx_ops* ops) {
  int midx = mgr->idx;
  int i, ret;
  struct st_rx_audio_session_impl* s;

  for (i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    if (!mgr->active[i]) {
      s = &mgr->sessions[i];
      ret = rx_audio_session_attach(mgr->parnet, mgr, s, ops);
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

int st_rx_audio_sessions_mgr_detach(struct st_rx_audio_sessions_mgr* mgr,
                                    struct st_rx_audio_session_impl* s) {
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

  rx_audio_session_lock(mgr, sidx);

  rx_audio_session_detach(mgr->parnet, s);

  mgr->active[sidx] = false;

  rx_audio_session_unlock(mgr, sidx);

  return 0;
}

int st_rx_audio_sessions_mgr_update(struct st_rx_audio_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_RX_AUDIO_SESSIONS; i++) {
    if (mgr->active[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_rx_audio_sessions_stat(struct st_main_impl* impl) {
  struct st_rx_audio_sessions_mgr* mgr = &impl->rx_a_mgr;
  struct st_rx_audio_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    if (mgr->active[j]) {
      s = &mgr->sessions[j];
      rx_audio_session_stat(s);
    }
  }
}
