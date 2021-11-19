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

#include "st_rx_video_session.h"

#include <math.h>

#include "st_dev.h"
#include "st_fmt.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_sch.h"
#include "st_util.h"

static inline bool rv_ebu_enabled(struct st_main_impl* impl) {
  if (st_get_user_params(impl)->flags & ST_FLAG_RX_VIDEO_EBU)
    return true;
  else
    return false;
}

static void rv_ebu_clear_result(struct st_rx_video_ebu_stat* ebu) {
  memset(ebu, 0, sizeof(*ebu));

  ebu->cinst_max = INT_MIN;
  ebu->cinst_min = INT_MAX;
  ebu->vrx_max = INT_MIN;
  ebu->vrx_min = INT_MAX;
  ebu->fpt_max = INT_MIN;
  ebu->fpt_min = INT_MAX;
  ebu->latency_max = INT_MIN;
  ebu->latency_min = INT_MAX;
  ebu->rtp_offset_max = INT_MIN;
  ebu->rtp_offset_min = INT_MAX;
  ebu->rtp_ts_delta_max = INT_MIN;
  ebu->rtp_ts_delta_min = INT_MAX;
  ebu->rtp_ipt_max = INT_MIN;
  ebu->rtp_ipt_min = INT_MAX;
}

static inline float rv_ebu_calculate_avg(uint32_t cnt, int64_t sum) {
  return cnt ? ((float)sum / cnt) : -1.0f;
}

static char* rv_ebu_cinst_result(struct st_rx_video_ebu_stat* ebu,
                                 struct st_rx_video_ebu_info* ebu_info) {
  if (ebu->cinst_max <= ebu_info->c_max_narrow_pass) return ST_EBU_PASS_NARROW;

  if (ebu->cinst_max <= ebu_info->c_max_wide_pass) return ST_EBU_PASS_WIDE;

  if (ebu->cinst_max <= (ebu_info->c_max_wide_pass * 16))
    return ST_EBU_PASS_WIDE_WA; /* WA, the RX time inaccurate */

  return ST_EBU_FAIL;
}

static char* rv_ebu_vrx_result(struct st_rx_video_ebu_stat* ebu,
                               struct st_rx_video_ebu_info* ebu_info) {
  if ((ebu->vrx_min > 0) && (ebu->vrx_max <= ebu_info->vrx_full_narrow_pass))
    return ST_EBU_PASS_NARROW;

  if ((ebu->vrx_min > 0) && (ebu->vrx_max <= ebu_info->vrx_full_wide_pass))
    return ST_EBU_PASS_WIDE;

  if (ebu->vrx_max <= ebu_info->vrx_full_wide_pass)
    return ST_EBU_PASS_WIDE_WA; /* skip vrx_min as no HW RX time */

  return ST_EBU_FAIL;
}

#ifdef DEBUG
static char* rv_ebu_latency_result(struct st_rx_video_ebu_stat* ebu) {
  if ((ebu->latency_min < 0) || (ebu->latency_max > ST_EBU_LATENCY_MAX_NS))
    return ST_EBU_FAIL;

  return ST_EBU_PASS;
}
#endif

static char* rv_ebu_rtp_offset_result(struct st_rx_video_ebu_stat* ebu,
                                      struct st_rx_video_ebu_info* ebu_info) {
  if ((ebu->rtp_offset_min < ST_EBU_RTP_OFFSET_MIN) ||
      (ebu->rtp_offset_max > ebu_info->rtp_offset_max_pass))
    return ST_EBU_FAIL;

  return ST_EBU_PASS;
}

static char* rv_ebu_rtp_ts_delta_result(struct st_rx_video_ebu_stat* ebu,
                                        struct st_rx_video_ebu_info* ebu_info) {
  int32_t rtd = ebu_info->frame_time_sampling;

  if ((ebu->rtp_ts_delta_min < rtd) || (ebu->rtp_ts_delta_max > (rtd + 1)))
    return ST_EBU_FAIL;

  return ST_EBU_PASS;
}

static char* rv_ebu_fpt_result(struct st_rx_video_ebu_stat* ebu, uint32_t tr_offset) {
  if (ebu->fpt_max <= tr_offset) return ST_EBU_PASS;

  if (ebu->fpt_max <= (tr_offset * 2)) /* WA as no HW RX time */
    return ST_EBU_PASS_WIDE_WA;

  return ST_EBU_FAIL;
}

static void rv_ebu_result(struct st_rx_video_session_impl* s) {
  struct st_rx_video_ebu_stat* ebu = &s->ebu;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  int idx = s->idx;

  ebu->vrx_avg = rv_ebu_calculate_avg(ebu->vrx_cnt, ebu->vrx_sum);
  ebu->cinst_avg = rv_ebu_calculate_avg(ebu->cinst_cnt, ebu->cinst_sum);
  ebu->fpt_avg = rv_ebu_calculate_avg(ebu->fpt_cnt, ebu->fpt_sum);
  ebu->latency_avg = rv_ebu_calculate_avg(ebu->latency_cnt, ebu->latency_sum);
  ebu->rtp_offset_avg = rv_ebu_calculate_avg(ebu->rtp_offset_cnt, ebu->rtp_offset_sum);
  ebu->rtp_ts_delta_avg =
      rv_ebu_calculate_avg(ebu->rtp_ts_delta_cnt, ebu->rtp_ts_delta_sum);
  ebu->rtp_ipt_avg = rv_ebu_calculate_avg(ebu->rtp_ipt_cnt, ebu->rtp_ipt_sum);

  info("%s(%d), Cinst AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx, ebu->cinst_avg,
       ebu->cinst_min, ebu->cinst_max, rv_ebu_cinst_result(ebu, ebu_info));
  info("%s(%d), VRX AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx, ebu->vrx_avg,
       ebu->vrx_min, ebu->vrx_max, rv_ebu_vrx_result(ebu, ebu_info));
  info("%s(%d), TRO %.2f TPRS %.2f FPT AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu_info->tr_offset, ebu_info->trs, ebu->fpt_avg, ebu->fpt_min, ebu->fpt_max,
       rv_ebu_fpt_result(ebu, ebu_info->tr_offset));
#ifdef DEBUG
  info("%s(%d), LATENCY AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->latency_avg, ebu->latency_min, ebu->latency_max, rv_ebu_latency_result(ebu));
#endif
  info("%s(%d), RTP Offset AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->rtp_offset_avg, ebu->rtp_offset_min, ebu->rtp_offset_max,
       rv_ebu_rtp_offset_result(ebu, ebu_info));
  info("%s(%d), RTP TS Delta AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->rtp_ts_delta_avg, ebu->rtp_ts_delta_min, ebu->rtp_ts_delta_max,
       rv_ebu_rtp_ts_delta_result(ebu, ebu_info));
  info("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n\n", __func__, idx,
       ebu->rtp_ipt_avg, ebu->rtp_ipt_min, ebu->rtp_ipt_max);
}

static void rv_ebu_on_frame(struct st_rx_video_session_impl* s, uint32_t rtp_tmstamp,
                            uint64_t pkt_tmstamp) {
  struct st_rx_video_ebu_stat* ebu = &s->ebu;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  uint64_t epochs = (double)pkt_tmstamp / ebu_info->frame_time;
  uint64_t epoch_tmstamp = (double)epochs * ebu_info->frame_time;
  double fpt_delta = (double)pkt_tmstamp - epoch_tmstamp;

  int64_t ticks = round((double)epoch_tmstamp * ebu_info->frame_time_sampling);
  uint32_t rtp_ts_for_epoch = ticks % ST_EBU_RTP_WRAP_AROUND;
  int delta_t_ticks = rtp_ts_for_epoch - rtp_tmstamp;
  double tr = ((double)epochs * ebu_info->frame_time * ebu_info->frame_time_sampling -
               delta_t_ticks) /
              ebu_info->frame_time_sampling;
  double latency = (double)pkt_tmstamp - tr;

  ebu->frame_idx++;
  if (ebu->frame_idx % (60 * 5) == 0) {
    /* every 5 seconds */
    rv_ebu_result(s);
    rv_ebu_clear_result(ebu);
  }

  ebu->cur_epochs = epochs;
  ebu->vrx_drained_prev = 0;
  ebu->vrx_prev = 0;
  ebu->cinst_initial_time = pkt_tmstamp;
  ebu->prev_rtp_ipt_ts = 0;

  /* calculate fpt */
  ebu->fpt_sum += fpt_delta;
  ebu->fpt_min = RTE_MIN(fpt_delta, ebu->fpt_min);
  ebu->fpt_max = RTE_MAX(fpt_delta, ebu->fpt_max);
  ebu->fpt_cnt++;

  ebu->latency_sum += latency;
  ebu->latency_min = RTE_MIN(latency, ebu->latency_min);
  ebu->latency_max = RTE_MAX(latency, ebu->latency_max);
  ebu->latency_cnt++;

  uint64_t tmstamp64 = epochs * ebu_info->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;
  double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;

  /* calculate rtp offset */
  ebu->rtp_offset_sum += diff_rtp_ts;
  ebu->rtp_offset_min = RTE_MIN(diff_rtp_ts, ebu->rtp_offset_min);
  ebu->rtp_offset_max = RTE_MAX(diff_rtp_ts, ebu->rtp_offset_max);
  ebu->rtp_offset_cnt++;

  /* calculate rtp ts dleta */
  if (ebu->prev_rtp_ts) {
    int rtp_ts_delta = rtp_tmstamp - ebu->prev_rtp_ts;

    ebu->rtp_ts_delta_sum += rtp_ts_delta;
    ebu->rtp_ts_delta_min = RTE_MIN(rtp_ts_delta, ebu->rtp_ts_delta_min);
    ebu->rtp_ts_delta_max = RTE_MAX(rtp_ts_delta, ebu->rtp_ts_delta_max);
    ebu->rtp_ts_delta_cnt++;
  }
  ebu->prev_rtp_ts = rtp_tmstamp;
}

static void rv_ebu_on_packet(struct st_rx_video_session_impl* s, uint32_t rtp_tmstamp,
                             uint64_t pkt_tmstamp, int pkt_idx) {
  struct st_rx_video_ebu_stat* ebu = &s->ebu;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  uint64_t epoch_tmstamp;
  double tvd, packet_delta_ns, trs = ebu_info->trs;

  if (!pkt_idx) /* start of new frame */
    rv_ebu_on_frame(s, rtp_tmstamp, pkt_tmstamp);

  epoch_tmstamp = (uint64_t)(ebu->cur_epochs * ebu_info->frame_time);
  tvd = epoch_tmstamp + ebu_info->tr_offset;

  /* Calculate vrx */
  packet_delta_ns = (double)pkt_tmstamp - tvd;
  int32_t drained = (packet_delta_ns + trs) / trs;
  int32_t vrx_cur = ebu->vrx_prev + 1 - (drained - ebu->vrx_drained_prev);

  ebu->vrx_sum += vrx_cur;
  ebu->vrx_min = RTE_MIN(vrx_cur, ebu->vrx_min);
  ebu->vrx_max = RTE_MAX(vrx_cur, ebu->vrx_max);
  ebu->vrx_cnt++;
  ebu->vrx_prev = vrx_cur;
  ebu->vrx_drained_prev = drained;

  /* Calculate C-inst */
  int exp_cin_pkts =
      ((pkt_tmstamp - ebu->cinst_initial_time) / trs) * ST_EBU_CINST_DRAIN_FACTOR;
  int cinst = RTE_MAX(0, pkt_idx - exp_cin_pkts);

  ebu->cinst_sum += cinst;
  ebu->cinst_min = RTE_MIN(cinst, ebu->cinst_min);
  ebu->cinst_max = RTE_MAX(cinst, ebu->cinst_max);
  ebu->cinst_cnt++;

  /* calculate Inter-packet time */
  if (ebu->prev_rtp_ipt_ts) {
    double ipt = (double)pkt_tmstamp - ebu->prev_rtp_ipt_ts;

    ebu->rtp_ipt_sum += ipt;
    ebu->rtp_ipt_min = RTE_MIN(ipt, ebu->rtp_ipt_min);
    ebu->rtp_ipt_max = RTE_MAX(ipt, ebu->rtp_ipt_max);
    ebu->rtp_ipt_cnt++;
  }
  ebu->prev_rtp_ipt_ts = pkt_tmstamp;
}

static int rv_ebu_init(struct st_main_impl* impl, struct st_rx_video_session_impl* s) {
  int idx = s->idx, ret;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  struct st20_rx_ops* ops = &s->ops;
  double frame_time, frame_time_s;
  struct st_fps_timing fps_tm;
  struct st21_timing tm;

  rv_ebu_clear_result(&s->ebu);

  ret = st21_get_timing(ops->width, ops->height, &tm);
  if (ret < 0) {
    err("%s(%d), invalid w %d h %d\n", __func__, idx, ops->width, ops->height);
    return ret;
  }

  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  frame_time_s = (double)fps_tm.den / fps_tm.mul;
  frame_time = (double)1000000000.0 * fps_tm.den / fps_tm.mul;
  /* calculate pkts in line */
  size_t bytes_in_pkt = ST_PKT_MAX_UDP_BYTES - sizeof(struct st_rfc4175_hdr_single);
  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  int st20_pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
  int st20_total_pkts = ops->height * st20_pkts_in_line;
  double st21_ractive = (double)ops->height / tm.total_lines;

  ebu_info->frame_time = frame_time;
  ebu_info->frame_time_sampling =
      (double)(fps_tm.sampling_clock_rate) * fps_tm.den / fps_tm.mul;
  ebu_info->trs = frame_time / (st20_pkts_in_line * tm.total_lines);
  ebu_info->tr_offset = frame_time * tm.tro_lines / tm.total_lines;

  ebu_info->c_max_narrow_pass =
      RTE_MAX(4, (double)st20_total_pkts / (43200 * st21_ractive * frame_time_s));
  ebu_info->c_max_wide_pass =
      RTE_MAX(16, (double)st20_total_pkts / (21600 * frame_time_s));

  ebu_info->vrx_full_narrow_pass = RTE_MAX(8, st20_total_pkts / (27000 * frame_time_s));
  ebu_info->vrx_full_wide_pass = RTE_MAX(720, st20_total_pkts / (300 * frame_time_s));

  ebu_info->rtp_offset_max_pass =
      ceil((ebu_info->tr_offset / NS_PER_S) * fps_tm.sampling_clock_rate) + 1;

  info("%s[%02d], trs %f tr offset %f sampling %f\n", __func__, idx, ebu_info->trs,
       ebu_info->tr_offset, ebu_info->frame_time_sampling);
  info(
      "%s[%02d], cmax_narrow %d cmax_wide %d vrx_full_narrow %d vrx_full_wide %d "
      "rtp_offset_max %d\n",
      __func__, idx, ebu_info->c_max_narrow_pass, ebu_info->c_max_wide_pass,
      ebu_info->vrx_full_narrow_pass, ebu_info->vrx_full_wide_pass,
      ebu_info->rtp_offset_max_pass);
  return 0;
}

static void* rx_video_session_get_frame(struct st_rx_video_session_impl* s) {
  int idx = s->idx;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (0 == rte_atomic32_read(&s->st20_frames_refcnt[i])) {
      dbg("%s(%d), find frame at %d\n", __func__, idx, i);
      rte_atomic32_inc(&s->st20_frames_refcnt[i]);
      return s->st20_frames[i];
    }
  }

  err("%s(%d), no free frame\n", __func__, idx);
  return NULL;
}

int rx_video_session_put_frame(struct st_rx_video_session_impl* s, void* frame) {
  int idx = s->idx;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (s->st20_frames[i] == frame) {
      dbg("%s(%d), put frame at %d\n", __func__, idx, i);
      rte_atomic32_dec(&s->st20_frames_refcnt[i]);
      return 0;
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, idx, frame);
  return -EIO;
}

static int rx_video_session_free_frames(struct st_rx_video_session_impl* s) {
  if (s->st20_frames) {
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      if (s->st20_frames[i]) {
        st_rte_free(s->st20_frames[i]);
        s->st20_frames[i] = NULL;
      }
    }
    st_rte_free(s->st20_frames);
    s->st20_frames = NULL;
  }
  if (s->st20_frames_refcnt) {
    st_rte_free(s->st20_frames_refcnt);
    s->st20_frames_refcnt = NULL;
  }
  s->st20_frames_cnt = 0;

  /* free slot */
  struct st_rx_video_slot_impl* slot;
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    if (slot->frame_bitmap) {
      st_rte_free(slot->frame_bitmap);
      slot->frame_bitmap = NULL;
    }
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rx_video_session_alloc_frames(struct st_main_impl* impl,
                                         struct st_rx_video_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t size = s->st20_frame_size;
  void* frame;

  s->st20_frames = st_rte_zmalloc_socket(sizeof(void*) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames) {
    err("%s(%d), st20_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  s->st20_frames_refcnt =
      st_rte_zmalloc_socket(sizeof(rte_atomic32_t) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames_refcnt) {
    err("%s(%d), st20_frames_refcnt alloc fail\n", __func__, idx);
    rx_video_session_free_frames(s);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame = st_rte_zmalloc_socket(size, soc_id);
    if (!frame) {
      err("%s(%d), frame malloc %" PRIu64 " fail for %d\n", __func__, idx, size, i);
      rx_video_session_free_frames(s);
      return -ENOMEM;
    }
    s->st20_frames[i] = frame;
    rte_atomic32_set(&s->st20_frames_refcnt[i], 0);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_video_session_free_rtps(struct st_rx_video_session_impl* s) {
  if (s->st20_rtps_ring) {
    st_ring_dequeue_clean(s->st20_rtps_ring);
    rte_ring_free(s->st20_rtps_ring);
    s->st20_rtps_ring = NULL;
  }

  return 0;
}

static int rx_video_session_alloc_rtps(struct st_main_impl* impl,
                                       struct st_rx_video_sessions_mgr* mgr,
                                       struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-VIDEO-RTP-RING-M%d-R%d", mgr_idx, idx);
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
  s->st20_rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_video_session_init(struct st_main_impl* impl,
                                 struct st_rx_video_sessions_mgr* mgr,
                                 struct st_rx_video_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int rx_video_session_uinit(struct st_main_impl* impl,
                                  struct st_rx_video_session_impl* s) {
  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

void rx_video_session_slot_dump(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    info("%s(%d), tmstamp %u recv_size %" PRIu64 "\n", __func__, i, slot->tmstamp,
         slot->frame_recv_size);
  }
}

static int rx_video_session_uinit_slot(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    if (slot->frame_bitmap) {
      st_rte_free(slot->frame_bitmap);
      slot->frame_bitmap = NULL;
    }
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rx_video_session_init_slot(struct st_main_impl* impl,
                                      struct st_rx_video_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t bitmap_size = s->st20_frame_bitmap_size;
  enum st20_type type = s->ops.type;
  struct st_rx_video_slot_impl* slot;
  uint8_t* frame_bitmap;

  /* init slot */
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    slot->idx = i;
    if (ST20_TYPE_FRAME_LEVEL == type) {
      slot->frame = rx_video_session_get_frame(s);
      if (!slot->frame) {
        err("%s(%d), slot frame get fail on %d\n", __func__, idx, i);
        return -ENOMEM;
      }
    }
    slot->frame_recv_size = 0;
    slot->tmstamp = 0;
    slot->seq_id_got = false;
    frame_bitmap = st_rte_zmalloc_socket(bitmap_size, soc_id);
    if (!frame_bitmap) {
      err("%s(%d), bitmap malloc %" PRIu64 " fail\n", __func__, idx, bitmap_size);
      return -ENOMEM;
    }
    slot->frame_bitmap = frame_bitmap;
  }
  s->slot_idx = -1;

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_rx_video_slot_impl* rx_video_frame_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp) {
  int i, ret;
  int slot_idx;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) return slot;
  }
  slot_idx = (s->slot_idx + 1) % ST_VIDEO_RX_REC_NUM_OFO;
  slot = &s->slots[slot_idx];
  // rx_video_session_slot_dump(s);

  if (slot->frame_recv_size) {
    s->st20_stat_frames_dropped++;
    // rx_video_slot_dump(s);
    dbg("%s: slot %d drop, recv size %" PRIu64 " tmstamp new %u old %u\n", __func__,
        slot_idx, slot->frame_recv_size, tmstamp, slot->tmstamp);
    slot->frame_recv_size = 0;
  }
  if (slot->frame) {
    ret = rx_video_session_put_frame(s, slot->frame);
    if (ret < 0) err("%s: slot %d put frame %p fail\n", __func__, slot_idx, slot->frame);
    slot->frame = NULL;
  }
  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  s->slot_idx = slot_idx;

  slot->frame = rx_video_session_get_frame(s);
  if (!slot->frame) {
    err("%s: slot %d get frame fail\n", __func__, slot_idx);
    return NULL;
  }

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);

  dbg("%s: assign slot %d frame %p for tmstamp %u\n", __func__, slot_idx, slot->frame,
      tmstamp);
  return slot;
}

static struct st_rx_video_slot_impl* rx_video_rtp_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp) {
  int i;
  int slot_idx = 0;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) return slot;
  }

  /* replace the oldest slot*/
  slot_idx = (s->slot_idx + 1) % ST_VIDEO_RX_REC_NUM_OFO;
  slot = &s->slots[slot_idx];
  // rx_video_session_slot_dump(s);

  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  s->slot_idx = slot_idx;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);

  dbg("%s: assign slot %d for tmstamp %u\n", __func__, slot_idx, tmstamp);
  return slot;
}

static int rx_video_session_handle_frame_pkt(struct st_main_impl* impl,
                                             struct st_rx_video_session_impl* s,
                                             struct rte_mbuf* mbuf,
                                             enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st20_rx_ops* ops = &s->ops;
  struct st_interface* inf = st_if(impl, port);
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_hdr_single) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t line1_number = ntohs(rtp->row_number); /* 0 to 1079 for 1080p */
  uint16_t line1_offset = ntohs(rtp->row_offset); /* [0, 480, 960, 1440] for 1080p */
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;
  if (line1_offset & 0x8000) {
    line1_offset &= ~0x8000;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint16_t line1_length = ntohs(rtp->row_length); /* 1200 for 1080p */
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  int pkt_idx = -1;

  /* do we need check if header is valid? */

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_frame_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    st_video_rtp_dump(port, idx, "drop as find slot fail", rtp);
    s->st20_stat_pkts_idx_dropped++;
    return -ENOMEM;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (seq_id >= slot->seq_id_base)
      pkt_idx = seq_id - slot->seq_id_base;
    else
      pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, idx, s_port,
          pkt_idx, slot->seq_id_base);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
    bool is_set = st_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, idx, s_port, pkt_idx);
      s->st20_stat_pkts_redunant_dropped++;
      return -EIO;
    }
  } else {
    if (!line1_number && !line1_offset) { /* first packet */
      slot->seq_id_base = seq_id;
      slot->seq_id_got = true;
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, idx, s_port,
          seq_id, slot->seq_id_base);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (line1_number * ops->width + line1_offset) / s->st20_pg.coverage * s->st20_pg.size;
  if ((offset + line1_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, idx, s_port,
        offset, s->st20_frame_size);
    s->st20_stat_pkts_offset_dropped++;
    return -EIO;
  }
  rte_memcpy(slot->frame + offset, payload, line1_length);
  slot->frame_recv_size += line1_length;
  if (extra_rtp) {
    uint16_t line2_number = ntohs(extra_rtp->row_number);
    uint16_t line2_offset = ntohs(extra_rtp->row_offset);
    uint16_t line2_length = ntohs(extra_rtp->row_length);

    dbg("%s(%d,%d), line2 info %d %d %d\n", __func__, idx, s_port, line2_number,
        line2_offset, line2_length);
    offset = (line2_number * ops->width + line2_offset) / s->st20_pg.coverage *
             s->st20_pg.size;
    if ((offset + line2_length) > s->st20_frame_size) {
      dbg("%s(%d,%d): invalid offset %u len %u, frame size %" PRIu64 " for extra rtp\n",
          __func__, idx, s_port, offset, line2_length, s->st20_frame_size);
      s->st20_stat_pkts_offset_dropped++;
      return -EIO;
    }
    rte_memcpy(slot->frame + offset, payload + line1_length, line2_length);
    slot->frame_recv_size += line2_length;
  }
  s->st20_stat_pkts_received++;

  if (rv_ebu_enabled(impl)) {
    if (inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) /* use hw timestamp */
      rv_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf), pkt_idx);
    else /* use ptp tsync timestamp */
      rv_ebu_on_packet(s, tmstamp, st_mbuf_get_time_stamp(mbuf), pkt_idx);
  }

  if (slot->frame_recv_size >= s->st20_frame_size) {
    dbg("%s(%d,%d): full frame on %p(%" PRIu64 ")\n", __func__, idx, s_port, slot->frame,
        slot->frame_recv_size);
    dbg("%s(%d,%d): tmstamp %u slot %d\n", __func__, idx, s_port, slot->tmstamp,
        slot->idx);
    /* get a full frame */
    ops->notify_frame_ready(ops->priv, slot->frame);
    slot->frame_recv_size = 0;
    slot->frame = NULL; /* frame pass to app */
    s->st20_stat_frames_received++;
  }

  return 0;
}

static int rx_video_session_handle_rtp_pkt(struct st_main_impl* impl,
                                           struct st_rx_video_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st20_rx_ops* ops = &s->ops;
  struct st_interface* inf = st_if(impl, port);
  size_t hdr_offset =
      sizeof(struct st_rfc4175_hdr_single) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  int pkt_idx = -1;

  /* do we need check if header is valid? */

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_rtp_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    st_video_rtp_dump(port, idx, "drop as find slot fail", rtp);
    s->st20_stat_pkts_idx_dropped++;
    return -ENOMEM;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (seq_id >= slot->seq_id_base)
      pkt_idx = seq_id - slot->seq_id_base;
    else
      pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;

    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, idx, s_port,
          pkt_idx, slot->seq_id_base);
      st_video_rtp_dump(port, idx, NULL, rtp);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
    bool is_set = st_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, idx, s_port, pkt_idx);
      s->st20_stat_pkts_redunant_dropped++;
      return -EIO;
    }
  } else {
    if (!slot->seq_id_got) { /* first packet */
      slot->seq_id_base = seq_id;
      slot->seq_id_got = true;
      s->st20_stat_frames_received++;
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, idx, s_port,
          seq_id, slot->seq_id_base);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* enqueue the packet ring to app */
  int ret = rte_ring_sp_enqueue(s->st20_rtps_ring, (void*)mbuf);
  if (ret < 0) {
    dbg("%s(%d,%d), drop as rtps ring full, pkt_idx %d base %u\n", __func__, idx, s_port,
        pkt_idx, slot->seq_id_base);
    s->st20_stat_pkts_rtp_ring_full++;
    return -EIO;
  }

  ops->notify_rtp_ready(ops->priv);
  s->st20_stat_pkts_received++;

  if (rv_ebu_enabled(impl)) {
    if (inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) /* use hw timestamp */
      rv_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf), pkt_idx);
    else /* use ptp tsync timestamp */
      rv_ebu_on_packet(s, tmstamp, st_mbuf_get_time_stamp(mbuf), pkt_idx);
  }

  return 0;
}

static int rx_video_session_tasklet(struct st_main_impl* impl,
                                    struct st_rx_video_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_VIDEO_BURTS_SIZE];
  uint16_t rv;
  struct st_interface* inf;
  int num_port = s->ops.num_port, ret;
  enum st20_type type = s->ops.type;
  bool ebu = rv_ebu_enabled(impl);
  static uint64_t rv_ebu_tsc_base_ns;
  static uint64_t rv_ebu_ptp_base_ns;

  if (ebu) {
    if (!rv_ebu_tsc_base_ns) rv_ebu_tsc_base_ns = st_get_tsc(impl);
    /* always use ST_PORT_P for ptp now */
    if (!rv_ebu_ptp_base_ns) rv_ebu_ptp_base_ns = st_get_ptp_time(impl, ST_PORT_P);
  }

  for (int s_port = 0; s_port < num_port; s_port++) {
    inf = st_if(impl, st_port_logic2phy(s->port_maps, s_port));
    rv = rte_eth_rx_burst(s->port_id[s_port], s->queue_id[s_port], &mbuf[0],
                          ST_RX_VIDEO_BURTS_SIZE);
    if (rv > 0) {
      if (ebu && !(inf->feature &
                   ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP)) { /* soft time for rv ebu */
        uint64_t cur_tsc_ns = st_get_tsc(impl);
        uint64_t tsc_delta = cur_tsc_ns - rv_ebu_tsc_base_ns;
        uint64_t ptp_time;

        if (tsc_delta > ST_RV_EBU_TSC_SYNC_NS) {
          rv_ebu_tsc_base_ns = st_get_tsc(impl);
          /* always use ST_PORT_P for ptp now */
          rv_ebu_ptp_base_ns = st_get_ptp_time(impl, ST_PORT_P);
          ptp_time = rv_ebu_ptp_base_ns;
        } else {
          ptp_time = rv_ebu_ptp_base_ns + tsc_delta;
        }

        for (uint16_t i = 0; i < rv; i++) st_mbuf_set_time_stamp(mbuf[i], ptp_time);
      }

      if (ST20_TYPE_FRAME_LEVEL == type) {
        for (uint16_t i = 0; i < rv; i++)
          rx_video_session_handle_frame_pkt(impl, s, mbuf[i], s_port);
        rte_pktmbuf_free_bulk(&mbuf[0], rv);
      } else {
        struct rte_mbuf* free_mbuf[ST_RX_VIDEO_BURTS_SIZE];
        int free_mbuf_cnt = 0;

        for (uint16_t i = 0; i < rv; i++) {
          ret = rx_video_session_handle_rtp_pkt(impl, s, mbuf[i], s_port);
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

static int rx_video_session_uinit_hw(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s) {
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

static int rx_video_session_init_hw(struct st_main_impl* impl,
                                    struct st_rx_video_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  int ret;
  uint16_t queue;
  struct st_dev_flow flow;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    memset(&flow, 0xff, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.sip_addr[i], ST_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, st_sip_addr(impl, port), ST_IP_ADDR_LEN);
    flow.port_flow = true;
    flow.src_port = s->st20_src_port[i];
    flow.dst_port = s->st20_dst_port[i];

    ret = st_dev_request_rx_queue(impl, port, &queue, &flow);
    if (ret < 0) {
      rx_video_session_uinit_hw(impl, s);
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

static int rx_video_session_uinit_sw(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s) {
  rx_video_session_free_frames(s);
  rx_video_session_free_rtps(s);
  rx_video_session_uinit_slot(s);
  return 0;
}

static int rx_video_session_init_sw(struct st_main_impl* impl,
                                    struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s) {
  enum st20_type type = s->ops.type;
  int idx = s->idx;
  int ret;

  switch (type) {
    case ST20_TYPE_FRAME_LEVEL:
      ret = rx_video_session_alloc_frames(impl, s);
      break;
    case ST20_TYPE_RTP_LEVEL:
      ret = rx_video_session_alloc_rtps(impl, mgr, s);
      break;
    default:
      err("%s(%d), error type %d\n", __func__, idx, type);
      return -EIO;
  }
  if (ret < 0) return ret;

  ret = rx_video_session_init_slot(impl, s);
  if (ret < 0) {
    rx_video_session_uinit_sw(impl, s);
    return ret;
  }

  return 0;
}

static int rx_video_session_attach(struct st_main_impl* impl,
                                   struct st_rx_video_sessions_mgr* mgr,
                                   struct st_rx_video_session_impl* s,
                                   struct st20_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  ret = st20_get_pgroup(ops->fmt, &s->st20_pg);
  if (ret < 0) {
    err("%s(%d), st20_get_pgroup fail %d\n", __func__, idx, ret);
    return ret;
  }

  s->st20_frames_cnt = ops->framebuff_cnt;
  s->st20_frame_size = ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
  /* at least 1000 byte for each packet */
  s->st20_frame_bitmap_size = s->st20_frame_size / 1000 / 8;
  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
  }

  s->st20_stat_pkts_idx_dropped = 0;
  s->st20_stat_pkts_offset_dropped = 0;
  s->st20_stat_pkts_redunant_dropped = 0;
  s->st20_stat_pkts_received = 0;
  s->st20_stat_pkts_rtp_ring_full = 0;
  s->st20_stat_frames_dropped = 0;
  s->st20_stat_frames_received = 0;
  s->st20_stat_last_time = st_get_monotonic_time();

  if (rv_ebu_enabled(impl)) {
    ret = rv_ebu_init(impl, s);
    if (ret < 0) {
      err("%s(%d), rv_ebu_init fail %d\n", __func__, idx, ret);
      return -EIO;
    }
  }

  ret = rx_video_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_video_session_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  ret = rx_video_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_video_session_init_sw fail %d\n", __func__, idx, ret);
    rx_video_session_uinit_hw(impl, s);
    return -EIO;
  }

  for (int i = 0; i < num_port; i++) {
    if (!st_is_multicast_ip(ops->sip_addr[i])) continue;
    ret = st_mcast_join(impl, *(uint32_t*)ops->sip_addr[i],
                        st_port_logic2phy(s->port_maps, i));
    if (ret < 0) {
      err("%s(%d), st_mcast_join fail %d\n", __func__, idx, ret);
      rx_video_session_uinit_sw(impl, s);
      rx_video_session_uinit_hw(impl, s);
      return -EIO;
    }
  }

  info("%s(%d), %d frames with size %" PRIu64 " %" PRIu64 ", type %d\n", __func__, idx,
       s->st20_frames_cnt, s->st20_frame_size, s->st20_frame_bitmap_size, ops->type);
  return 0;
}

static int rx_video_session_detach(struct st_main_impl* impl,
                                   struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (st_is_multicast_ip(ops->sip_addr[i]))
      st_mcast_leave(impl, *(uint32_t*)ops->sip_addr[i],
                     st_port_logic2phy(s->port_maps, i));
  }

  rx_video_session_uinit_sw(impl, s);
  rx_video_session_uinit_hw(impl, s);
  return 0;
}

static int rx_video_sessions_tasklet_handler(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  int i;

  for (i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (mgr->active[i]) {
      rx_video_session_tasklet(impl, &mgr->sessions[i]);
    }
  }

  return 0;
}

static int rx_video_sessions_tasklet_start(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_video_sessions_tasklet_stop(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_rx_video_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_rx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  int ret;
  struct st_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    ret = rx_video_session_init(impl, mgr, &mgr->sessions[i], i);
    if (ret < 0) {
      err("%s(%d), rx_video_session_init fail %d for %d\n", __func__, idx, ret, i);
      return ret;
    }
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_video_sessions_mgr";
  ops.start = rx_video_sessions_tasklet_start;
  ops.stop = rx_video_sessions_tasklet_stop;
  ops.handler = rx_video_sessions_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    err("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_rx_video_sessions_mgr_uinit(struct st_rx_video_sessions_mgr* mgr) {
  int idx = mgr->idx;
  int ret, i;
  struct st_rx_video_session_impl* s;

  for (i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = &mgr->sessions[i];

    if (mgr->active[i]) { /* make sure all session are detached*/
      warn("%s(%d), session %d still attached\n", __func__, idx, i);
      ret = st_rx_video_sessions_mgr_detach(mgr, s);
      if (ret < 0) {
        err("%s(%d), st_rx_video_sessions_mgr_detach fail %d for %d\n", __func__, idx,
            ret, i);
      }
    }

    ret = rx_video_session_uinit(mgr->parnet, s);
    if (ret < 0) {
      err("%s(%d), st_rx_video_session_uinit fail %d for %d\n", __func__, idx, ret, i);
    }
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

struct st_rx_video_session_impl* st_rx_video_sessions_mgr_attach(
    struct st_rx_video_sessions_mgr* mgr, struct st20_rx_ops* ops) {
  int midx = mgr->idx;
  int i, ret;
  struct st_rx_video_session_impl* s;

  for (i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (!mgr->active[i]) {
      s = &mgr->sessions[i];
      ret = rx_video_session_attach(mgr->parnet, mgr, s, ops);
      if (ret < 0) {
        err("%s(%d), rx_video_session_attach fail on %d\n", __func__, midx, i);
        return NULL;
      }
      mgr->active[i] = true;
      return s;
    }
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_rx_video_sessions_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s) {
  int midx = mgr->idx;
  int sidx = s->idx;

  if (s != &mgr->sessions[sidx]) {
    err("%s(%d,%d), mismatch session\n", __func__, midx, sidx);
    return -EIO;
  }

  rx_video_session_detach(mgr->parnet, s);

  mgr->active[sidx] = false;

  return 0;
}

void st_rx_video_sessions_stat(struct st_main_impl* impl) {
  struct st_sch_impl* sch;
  struct st_rx_video_sessions_mgr* mgr;
  struct st_rx_video_session_impl* s;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    if (!st_sch_is_active(sch)) continue;
    mgr = &sch->rx_video_mgr;
    for (int j = 0; j < ST_SCH_MAX_RX_VIDEO_SESSIONS; j++) {
      if (mgr->active[j]) {
        s = &mgr->sessions[j];

        uint64_t cur_time_ns = st_get_monotonic_time();
        double time_sec = (double)(cur_time_ns - s->st20_stat_last_time) / NS_PER_S;
        double framerate = s->st20_stat_frames_received / time_sec;

        info("RX_VIDEO_SESSION(%d,%d): fps %f, received frames %d, pkts %d\n", sch_idx, j,
             framerate, s->st20_stat_frames_received, s->st20_stat_pkts_received);
        s->st20_stat_frames_received = 0;
        s->st20_stat_pkts_received = 0;
        s->st20_stat_last_time = cur_time_ns;

        if (s->st20_stat_frames_dropped || s->st20_stat_pkts_idx_dropped ||
            s->st20_stat_pkts_offset_dropped) {
          info(
              "RX_VIDEO_SESSION(%d,%d): dropped frames %d, pkts (idx error: %d, offset "
              "error: %d)\n",
              sch_idx, j, s->st20_stat_frames_dropped, s->st20_stat_pkts_idx_dropped,
              s->st20_stat_pkts_offset_dropped);
          s->st20_stat_frames_dropped = 0;
          s->st20_stat_pkts_idx_dropped = 0;
        }
        if (s->st20_stat_pkts_rtp_ring_full) {
          info("RX_VIDEO_SESSION(%d,%d): rtp dropped pkts %d as ring full\n", sch_idx, j,
               s->st20_stat_pkts_rtp_ring_full);
          s->st20_stat_pkts_rtp_ring_full = 0;
        }
        if (s->st20_stat_pkts_redunant_dropped) {
          info("RX_VIDEO_SESSION(%d,%d): redunant dropped pkts %d\n", sch_idx, j,
               s->st20_stat_pkts_redunant_dropped);
          s->st20_stat_pkts_redunant_dropped = 0;
        }
      }
    }
  }
}
