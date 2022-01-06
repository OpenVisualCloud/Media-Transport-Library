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

static inline double rv_ebu_pass_rate(struct st_rx_video_ebu_result* ebu_result,
                                      int pass) {
  return (double)pass * 100 / ebu_result->ebu_result_num;
}

static void rx_video_session_ebu_result(struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_video_ebu_result* ebu_result = &s->ebu_result;

  ebu_result->ebu_result_num -= ebu_info->dropped_results;
  if (ebu_result->ebu_result_num < 0) {
    err("%s, ebu result not enough\n", __func__);
    return;
  }

  critical("st20(%d), [ --- Totla %d ---  Compliance Rate Narrow %.2f%%  Wide %.2f%% ]\n",
           idx, ebu_result->ebu_result_num,
           rv_ebu_pass_rate(ebu_result, ebu_result->compliance_narrow),
           rv_ebu_pass_rate(ebu_result,
                            ebu_result->compliance - ebu_result->compliance_narrow));
  critical("st20(%d), [ Cinst ]\t| Narrow %.2f%% | Wide %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->cinst_pass_narrow),
           rv_ebu_pass_rate(ebu_result, ebu_result->cinst_pass_wide),
           rv_ebu_pass_rate(ebu_result, ebu_result->cinst_fail));
  critical("st20(%d), [ VRX ]\t| Narrow %.2f%% | Wide %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->vrx_pass_narrow),
           rv_ebu_pass_rate(ebu_result, ebu_result->vrx_pass_wide),
           rv_ebu_pass_rate(ebu_result, ebu_result->vrx_fail));
  critical("st20(%d), [ FPT ]\t| Pass %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->fpt_pass),
           rv_ebu_pass_rate(ebu_result, ebu_result->fpt_fail));
  critical("st20(%d), [ Latency ]\t| Pass %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->latency_pass),
           rv_ebu_pass_rate(ebu_result, ebu_result->latency_fail));
  critical("st20(%d), [ RTP Offset ]\t| Pass %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->rtp_offset_pass),
           rv_ebu_pass_rate(ebu_result, ebu_result->rtp_offset_fail));
  critical("st20(%d), [ RTP TS Delta ]\t| Pass %.2f%% | Fail %.2f%% |\n", idx,
           rv_ebu_pass_rate(ebu_result, ebu_result->rtp_ts_delta_pass),
           rv_ebu_pass_rate(ebu_result, ebu_result->rtp_ts_delta_fail));
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

  ebu->compliant = true;
  ebu->compliant_narrow = true;
}

static inline float rv_ebu_calculate_avg(uint32_t cnt, int64_t sum) {
  return cnt ? ((float)sum / cnt) : -1.0f;
}

static char* rv_ebu_cinst_result(struct st_rx_video_ebu_stat* ebu,
                                 struct st_rx_video_ebu_info* ebu_info,
                                 struct st_rx_video_ebu_result* ebu_result) {
  if (ebu->cinst_max <= ebu_info->c_max_narrow_pass) {
    ebu_result->cinst_pass_narrow++;
    return ST_EBU_PASS_NARROW;
  }

  if (ebu->cinst_max <= ebu_info->c_max_wide_pass) {
    ebu_result->cinst_pass_wide++;
    ebu->compliant_narrow = false;
    return ST_EBU_PASS_WIDE;
  }

  if (ebu->cinst_max <= (ebu_info->c_max_wide_pass * 16)) {
    ebu_result->cinst_pass_wide++;
    ebu->compliant_narrow = false;
    return ST_EBU_PASS_WIDE_WA; /* WA, the RX time inaccurate */
  }

  ebu_result->cinst_fail++;
  ebu->compliant = false;
  return ST_EBU_FAIL;
}

static char* rv_ebu_vrx_result(struct st_rx_video_ebu_stat* ebu,
                               struct st_rx_video_ebu_info* ebu_info,
                               struct st_rx_video_ebu_result* ebu_result) {
  if ((ebu->vrx_min > 0) && (ebu->vrx_max <= ebu_info->vrx_full_narrow_pass)) {
    ebu_result->vrx_pass_narrow++;
    return ST_EBU_PASS_NARROW;
  }

  if ((ebu->vrx_min > 0) && (ebu->vrx_max <= ebu_info->vrx_full_wide_pass)) {
    ebu_result->vrx_pass_wide++;
    ebu->compliant_narrow = false;
    return ST_EBU_PASS_WIDE;
  }

  if (ebu->vrx_max <= ebu_info->vrx_full_wide_pass) {
    ebu_result->vrx_pass_wide++;
    ebu->compliant_narrow = false;
    return ST_EBU_PASS_WIDE_WA; /* skip vrx_min as no HW RX time */
  }

  ebu_result->vrx_fail++;
  ebu->compliant = false;
  return ST_EBU_FAIL;
}

static char* rv_ebu_latency_result(struct st_rx_video_ebu_stat* ebu,
                                   struct st_rx_video_ebu_result* ebu_result) {
  if ((ebu->latency_min < 0) || (ebu->latency_max > ST_EBU_LATENCY_MAX_NS)) {
    ebu_result->latency_fail++;
    ebu->compliant = false;
    return ST_EBU_FAIL;
  }

  ebu_result->latency_pass++;
  return ST_EBU_PASS;
}

static char* rv_ebu_rtp_offset_result(struct st_rx_video_ebu_stat* ebu,
                                      struct st_rx_video_ebu_info* ebu_info,
                                      struct st_rx_video_ebu_result* ebu_result) {
  if ((ebu->rtp_offset_min < ST_EBU_RTP_OFFSET_MIN) ||
      (ebu->rtp_offset_max > ebu_info->rtp_offset_max_pass)) {
    ebu_result->rtp_offset_fail++;
    ebu->compliant = false;
    return ST_EBU_FAIL;
  }

  ebu_result->rtp_offset_pass++;
  return ST_EBU_PASS;
}

static char* rv_ebu_rtp_ts_delta_result(struct st_rx_video_ebu_stat* ebu,
                                        struct st_rx_video_ebu_info* ebu_info,
                                        struct st_rx_video_ebu_result* ebu_result) {
  int32_t rtd = ebu_info->frame_time_sampling;

  if ((ebu->rtp_ts_delta_min < rtd) || (ebu->rtp_ts_delta_max > (rtd + 1))) {
    ebu_result->rtp_ts_delta_fail++;
    ebu->compliant = false;
    return ST_EBU_FAIL;
  }

  ebu_result->rtp_ts_delta_pass++;
  return ST_EBU_PASS;
}

static char* rv_ebu_fpt_result(struct st_rx_video_ebu_stat* ebu, uint32_t tr_offset,
                               struct st_rx_video_ebu_result* ebu_result) {
  if (ebu->fpt_max <= tr_offset) {
    ebu_result->fpt_pass++;
    return ST_EBU_PASS;
  }

  if (ebu->fpt_max <= (tr_offset * 2)) { /* WA as no HW RX time */
    ebu_result->fpt_pass++;
    return ST_EBU_PASS_WIDE_WA;
  }

  ebu_result->fpt_fail++;
  ebu->compliant = false;
  return ST_EBU_FAIL;
}

static void rv_ebu_result(struct st_rx_video_session_impl* s) {
  struct st_rx_video_ebu_stat* ebu = &s->ebu;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_video_ebu_result* ebu_result = &s->ebu_result;
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
       ebu->cinst_min, ebu->cinst_max, rv_ebu_cinst_result(ebu, ebu_info, ebu_result));
  info("%s(%d), VRX AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx, ebu->vrx_avg,
       ebu->vrx_min, ebu->vrx_max, rv_ebu_vrx_result(ebu, ebu_info, ebu_result));
  info("%s(%d), TRO %.2f TPRS %.2f FPT AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu_info->tr_offset, ebu_info->trs, ebu->fpt_avg, ebu->fpt_min, ebu->fpt_max,
       rv_ebu_fpt_result(ebu, ebu_info->tr_offset, ebu_result));
  info("%s(%d), LATENCY AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->latency_avg, ebu->latency_min, ebu->latency_max,
       rv_ebu_latency_result(ebu, ebu_result));
  info("%s(%d), RTP Offset AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->rtp_offset_avg, ebu->rtp_offset_min, ebu->rtp_offset_max,
       rv_ebu_rtp_offset_result(ebu, ebu_info, ebu_result));
  info("%s(%d), RTP TS Delta AVG %.2f MIN %d MAX %d test %s!\n", __func__, idx,
       ebu->rtp_ts_delta_avg, ebu->rtp_ts_delta_min, ebu->rtp_ts_delta_max,
       rv_ebu_rtp_ts_delta_result(ebu, ebu_info, ebu_result));
  info("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n\n", __func__, idx,
       ebu->rtp_ipt_avg, ebu->rtp_ipt_min, ebu->rtp_ipt_max);

  if (ebu->compliant) {
    ebu_result->compliance++;
    if (ebu->compliant_narrow) ebu_result->compliance_narrow++;
  }
}

static void rv_ebu_on_frame(struct st_rx_video_session_impl* s, uint32_t rtp_tmstamp,
                            uint64_t pkt_tmstamp) {
  struct st_rx_video_ebu_stat* ebu = &s->ebu;
  struct st_rx_video_ebu_info* ebu_info = &s->ebu_info;
  struct st_rx_video_ebu_result* ebu_result = &s->ebu_result;
  uint64_t epochs = (double)pkt_tmstamp / ebu_info->frame_time;
  uint64_t epoch_tmstamp = (double)epochs * ebu_info->frame_time;
  double fpt_delta = (double)pkt_tmstamp - epoch_tmstamp;

  ebu->frame_idx++;
  if (ebu->frame_idx % (60 * 5) == 0) {
    ebu_result->ebu_result_num++;
    /* every 5(60fps)/10(30fps) seconds */
    if (ebu_result->ebu_result_num > ebu_info->dropped_results) rv_ebu_result(s);
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

  uint64_t tmstamp64 = epochs * ebu_info->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;
  double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;
  double diff_rtp_ts_ns =
      diff_rtp_ts * ebu_info->frame_time / ebu_info->frame_time_sampling;
  double latency = fpt_delta - diff_rtp_ts_ns;

  /* calculate latency */
  ebu->latency_sum += latency;
  ebu->latency_min = RTE_MIN(latency, ebu->latency_min);
  ebu->latency_max = RTE_MAX(latency, ebu->latency_max);
  ebu->latency_cnt++;

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

  rv_ebu_clear_result(&s->ebu);

  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  frame_time_s = (double)fps_tm.den / fps_tm.mul;
  frame_time = (double)1000000000.0 * fps_tm.den / fps_tm.mul;
  /* calculate pkts in line */
  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc4175_video_hdr);
  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  int st20_pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
  int st20_total_pkts = ops->height * st20_pkts_in_line;
  double ractive = 1080.0 / 1125.0;

  ebu_info->frame_time = frame_time;
  ebu_info->frame_time_sampling =
      (double)(fps_tm.sampling_clock_rate) * fps_tm.den / fps_tm.mul;
  ebu_info->trs = frame_time * ractive / st20_total_pkts;
  ebu_info->tr_offset =
      ops->height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 725.0);

  ebu_info->c_max_narrow_pass =
      RTE_MAX(4, (double)st20_total_pkts / (43200 * ractive * frame_time_s));
  ebu_info->c_max_wide_pass =
      RTE_MAX(16, (double)st20_total_pkts / (21600 * frame_time_s));

  ebu_info->vrx_full_narrow_pass = RTE_MAX(8, st20_total_pkts / (27000 * frame_time_s));
  ebu_info->vrx_full_wide_pass = RTE_MAX(720, st20_total_pkts / (300 * frame_time_s));

  ebu_info->rtp_offset_max_pass =
      ceil((ebu_info->tr_offset / NS_PER_S) * fps_tm.sampling_clock_rate) + 1;

  ebu_info->dropped_results = 2; /* we drop the first 2 results */

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

static inline void rx_video_session_lock(struct st_rx_video_sessions_mgr* mgr, int sidx) {
  rte_spinlock_lock(&mgr->mutex[sidx]);
}

static inline int rx_video_session_try_lock(struct st_rx_video_sessions_mgr* mgr,
                                            int sidx) {
  return rte_spinlock_trylock(&mgr->mutex[sidx]);
}

static inline void rx_video_session_unlock(struct st_rx_video_sessions_mgr* mgr,
                                           int sidx) {
  rte_spinlock_unlock(&mgr->mutex[sidx]);
}

static void* rx_video_session_get_frame(struct st_rx_video_session_impl* s) {
  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (0 == rte_atomic32_read(&s->st20_frames_refcnt[i])) {
      dbg("%s(%d), find frame at %d\n", __func__, s->idx, i);
      rte_atomic32_inc(&s->st20_frames_refcnt[i]);
      return s->st20_frames[i];
    }
  }

  dbg("%s(%d), no free frame\n", __func__, s->idx);
  return NULL;
}

int st_rx_video_session_put_frame(struct st_rx_video_session_impl* s, void* frame) {
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

void rx_video_session_slot_dump(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    info("%s(%d), tmstamp %u recv_size %d pkts_received %u\n", __func__, i, slot->tmstamp,
         rte_atomic32_read(&slot->frame_recv_size), slot->pkts_received);
  }
}

static int rx_video_session_init(struct st_main_impl* impl,
                                 struct st_rx_video_sessions_mgr* mgr,
                                 struct st_rx_video_session_impl* s, int idx) {
  s->idx = idx;
  s->parnet = impl;
  return 0;
}

static int rx_video_session_uinit(struct st_main_impl* impl,
                                  struct st_rx_video_session_impl* s) {
  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
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
  struct st_rx_video_slot_impl* slot;
  uint8_t* frame_bitmap;

  /* init slot */
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    slot->idx = i;
    slot->frame = NULL;
    rte_atomic32_set(&slot->frame_recv_size, 0);
    slot->pkts_received = 0;
    slot->pkts_redunant_received = 0;
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
  s->slot_max = 1; /* default only one slot */

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static void rx_video_frame_notify(struct st_rx_video_session_impl* s,
                                  struct st_rx_video_slot_impl* slot) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_frame_meta* meta = &slot->meta;

  meta->width = ops->width;
  meta->height = ops->height;
  meta->fmt = ops->fmt;
  meta->fps = ops->fps;
  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->frame_total_size = s->st20_frame_size;
  meta->frame_recv_size = rte_atomic32_read(&slot->frame_recv_size);
  if (meta->frame_recv_size >= s->st20_frame_size) {
    meta->status = ST20_FRAME_STATUS_COMPLETE;
    if (ops->num_port > 1) {
      dbg("%s(%d): pks redunant %u received %u\n", __func__, s->idx,
          slot->pkts_redunant_received, slot->pkts_received);
      if ((slot->pkts_redunant_received + 16) < slot->pkts_received)
        meta->status = ST20_FRAME_STATUS_RECONSTRUCTED;
    }
    s->st20_stat_frames_received++;

    /* notify frame */
    int ret = -EIO;
    if (ops->notify_frame_ready)
      ret = ops->notify_frame_ready(ops->priv, slot->frame, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      st_rx_video_session_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  } else {
    dbg("%s(%d): frame_recv_size %d, frame_total_size %" PRIu64 ", tmstamp %u\n",
        __func__, s->idx, meta->frame_recv_size, meta->frame_total_size, slot->tmstamp);
    meta->status = ST20_FRAME_STATUS_CORRUPTED;
    s->st20_stat_frames_dropped++;
    /* notify the incomplete frame if user required */
    if (ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME) {
      ops->notify_frame_ready(ops->priv, slot->frame, meta);
    } else {
      st_rx_video_session_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  }
}

static struct st_rx_video_slot_impl* rx_video_frame_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp) {
  int i, slot_idx;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < s->slot_max; i++) {
    slot = &s->slots[i];

    if (slot->frame && (tmstamp == slot->tmstamp)) return slot;
  }
  slot_idx = (s->slot_idx + 1) % s->slot_max;
  slot = &s->slots[slot_idx];
  // rx_video_session_slot_dump(s);

  if (rte_atomic32_read(&slot->frame_recv_size)) {
    /* drop frame */
    if (slot->frame) {
      rx_video_frame_notify(s, slot);
      slot->frame = NULL;
    }
    rte_atomic32_set(&slot->frame_recv_size, 0);
  }
  /* put the frame if any */
  if (slot->frame) {
    st_rx_video_session_put_frame(s, slot->frame);
    slot->frame = NULL;
  }

  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  s->slot_idx = slot_idx;

  slot->frame = rx_video_session_get_frame(s);
  if (!slot->frame) {
    dbg("%s: slot %d get frame fail\n", __func__, slot_idx);
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
                                             enum st_session_port s_port,
                                             bool ctrl_thread) {
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st20_rx_ops* ops = &s->ops;
  struct st_interface* inf = st_if(impl, port);
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
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
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint16_t seq_id = ntohs(rtp->base.seq_number);
  int pkt_idx = -1;
  /* do we need check if header is valid? */

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_frame_slot_by_tmstamp(s, tmstamp);
  if (!slot || !slot->frame) {
    s->st20_stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (seq_id >= slot->seq_id_base)
      pkt_idx = seq_id - slot->seq_id_base;
    else
      pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
    bool is_set = st_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      s->st20_stat_pkts_redunant_dropped++;
      slot->pkts_redunant_received++;
      return -EIO;
    }
  } else {
    /* the first pkt should always dispatch to control thread */
    if (!line1_number && !line1_offset && ctrl_thread) { /* first packet */
      slot->seq_id_base = seq_id;
      slot->seq_id_got = true;
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, s->idx,
          s_port, seq_id, slot->seq_id_base);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (line1_number * ops->width + line1_offset) / s->st20_pg.coverage * s->st20_pg.size;
  if ((offset + line1_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    s->st20_stat_pkts_offset_dropped++;
    return -EIO;
  }
  rte_memcpy(slot->frame + offset, payload, line1_length);
  uint16_t line2_length = 0;
  if (extra_rtp) {
    uint16_t line2_number = ntohs(extra_rtp->row_number);
    uint16_t line2_offset = ntohs(extra_rtp->row_offset);
    line2_length = ntohs(extra_rtp->row_length);

    dbg("%s(%d,%d), line2 info %d %d %d\n", __func__, s->idx, s_port, line2_number,
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
  }

  rte_atomic32_add(&slot->frame_recv_size, line1_length + line2_length);
  s->st20_stat_pkts_received++;
  slot->pkts_received++;

  int32_t frame_recv_size = rte_atomic32_read(&slot->frame_recv_size);
  if (frame_recv_size >= s->st20_frame_size) {
    dbg("%s(%d,%d): full frame on %p(%d)\n", __func__, s->idx, s_port, slot->frame,
        frame_recv_size);
    dbg("%s(%d,%d): tmstamp %u slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    /* end of frame */
    rx_video_frame_notify(s, slot);
    rte_atomic32_set(&slot->frame_recv_size, 0);
    slot->pkts_received = 0;
    slot->pkts_redunant_received = 0;
    slot->frame = NULL; /* frame pass to app */
  }

  if (rv_ebu_enabled(impl) && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    rv_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf), pkt_idx);
  }

  return 0;
}

static int rx_video_session_handle_rtp_pkt(struct st_main_impl* impl,
                                           struct st_rx_video_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum st_session_port s_port) {
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st20_rx_ops* ops = &s->ops;
  struct st_interface* inf = st_if(impl, port);
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  int pkt_idx = -1;

  /* do we need check if header is valid? */

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_rtp_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    s->st20_stat_pkts_no_slot++;
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

  if (rv_ebu_enabled(impl) && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    rv_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf), pkt_idx);
  }

  return 0;
}

static int rx_video_session_tasklet(struct st_main_impl* impl,
                                    struct st_rx_video_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_VIDEO_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port, ret;
  enum st20_type type = s->ops.type;
  struct rte_ring* pkt_ring = s->pkt_lcore_ring;
  bool ctl_thread = pkt_ring ? false : true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->queue_active[s_port]) continue;
    rv = rte_eth_rx_burst(s->port_id[s_port], s->queue_id[s_port], &mbuf[0],
                          ST_RX_VIDEO_BURTS_SIZE);
    if (pkt_ring) {
      /* first pass to the pkt ring if it has pkt handling lcore */
      unsigned int n =
          rte_ring_sp_enqueue_bulk(s->pkt_lcore_ring, (void**)&mbuf[0], rv, NULL);
      rv -= n;
      s->st20_stat_pkts_enqueue_fallback += rv;
    }
    if (rv > 0) {
      if (ST20_TYPE_FRAME_LEVEL == type) {
        for (uint16_t i = 0; i < rv; i++)
          rx_video_session_handle_frame_pkt(impl, s, mbuf[i], s_port, ctl_thread);
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

static int rx_video_session_uinit_pkt_lcore(struct st_main_impl* impl,
                                            struct st_rx_video_session_impl* s) {
  int idx = s->idx;

  if (rte_atomic32_read(&s->pkt_lcore_active)) {
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    info("%s(%d), stop lcore\n", __func__, idx);
    while (rte_atomic32_read(&s->pkt_lcore_stopped) == 0) {
      st_sleep_ms(10);
    }
  }

  if (s->has_pkt_lcore) {
    rte_eal_wait_lcore(s->pkt_lcore);
    st_dev_put_lcore(impl, s->pkt_lcore);
    s->has_pkt_lcore = false;
  }

  if (s->pkt_lcore_ring) {
    st_ring_dequeue_clean(s->pkt_lcore_ring);
    rte_ring_free(s->pkt_lcore_ring);
    s->pkt_lcore_ring = NULL;
  }

  return 0;
}

static int rx_video_session_pkt_lcore_func(void* args) {
  struct st_rx_video_session_impl* s = args;
  struct st_main_impl* impl = s->parnet;
  int idx = s->idx, ret;
  struct rte_mbuf* pkt;

  info("%s(%d), start\n", __func__, idx);
  while (rte_atomic32_read(&s->pkt_lcore_active)) {
    ret = rte_ring_sc_dequeue(s->pkt_lcore_ring, (void**)&pkt);
    if (ret >= 0) {
      rx_video_session_handle_frame_pkt(impl, s, pkt, ST_SESSION_PORT_P, true);
      rte_pktmbuf_free(pkt);
    }
  }

  rte_atomic32_set(&s->pkt_lcore_stopped, 1);
  info("%s(%d), end\n", __func__, idx);
  return 0;
}

static int rx_video_session_init_pkt_lcore(struct st_main_impl* impl,
                                           struct st_rx_video_sessions_mgr* mgr,
                                           struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count, lcore;
  int mgr_idx = mgr->idx, idx = s->idx, ret;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-VIDEO-PKT-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = ST_RX_VIDEO_BURTS_SIZE * 4;
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), ring create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->pkt_lcore_ring = ring;

  ret = st_dev_get_lcore(impl, &lcore);
  if (ret < 0) {
    err("%s(%d,%d), get lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rx_video_session_uinit_pkt_lcore(impl, s);
    return ret;
  }
  s->pkt_lcore = lcore;
  s->has_pkt_lcore = true;

  rte_atomic32_set(&s->pkt_lcore_active, 1);
  ret = rte_eal_remote_launch(rx_video_session_pkt_lcore_func, s, lcore);
  if (ret < 0) {
    err("%s(%d,%d), launch lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    rx_video_session_uinit_pkt_lcore(impl, s);
    return ret;
  }

  return 0;
}

static int rx_video_session_uinit_sw(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s) {
  rx_video_session_uinit_pkt_lcore(impl, s);
  rx_video_session_free_frames(s);
  rx_video_session_free_rtps(s);
  rx_video_session_uinit_slot(s);
  return 0;
}

static int rx_video_session_init_sw(struct st_main_impl* impl,
                                    struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  enum st20_type type = ops->type;
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

  s->has_pkt_lcore = false;
  rte_atomic32_set(&s->pkt_lcore_stopped, 0);
  rte_atomic32_set(&s->pkt_lcore_active, 0);

  uint64_t bps;
  bool pkt_handle_lcore = false;
  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s(%d), get bps fail %d\n", __func__, idx, ret);
    rx_video_session_uinit_sw(impl, s);
    return ret;
  }
  /* for trafic > 40g, two lcore used  */
  if ((bps / (1000 * 1000)) > (40 * 1000)) pkt_handle_lcore = true;

  if (pkt_handle_lcore) {
    ret = rx_video_session_init_pkt_lcore(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), init_pkt_lcore fail %d\n", __func__, idx, ret);
      rx_video_session_uinit_sw(impl, s);
      return ret;
    }
    /* enable multi slot as it has two threads running */
    s->slot_max = ST_VIDEO_RX_REC_NUM_OFO;
  }

  return 0;
}

static int rx_video_session_uinit_mcast(struct st_main_impl* impl,
                                        struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (st_is_multicast_ip(ops->sip_addr[i]))
      st_mcast_leave(impl, *(uint32_t*)ops->sip_addr[i],
                     st_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rx_video_session_init_mcast(struct st_main_impl* impl,
                                       struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  int ret;

  for (int i = 0; i < ops->num_port; i++) {
    if (!st_is_multicast_ip(ops->sip_addr[i])) continue;
    ret = st_mcast_join(impl, *(uint32_t*)ops->sip_addr[i],
                        st_port_logic2phy(s->port_maps, i));
    if (ret < 0) return ret;
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
    err("%s(%d), get pgroup fail %d\n", __func__, idx, ret);
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
  s->st20_stat_pkts_no_slot = 0;
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

  ret = rx_video_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_video_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_video_session_uinit_sw(impl, s);
    rx_video_session_uinit_hw(impl, s);
    return -EIO;
  }

  info("%s(%d), %d frames with size %" PRIu64 " %" PRIu64 ", type %d\n", __func__, idx,
       s->st20_frames_cnt, s->st20_frame_size, s->st20_frame_bitmap_size, ops->type);
  return 0;
}

static int rx_video_sessions_tasklet_handler(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_rx_video_session_impl* s;
  int sidx;
  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    if (rx_video_session_try_lock(mgr, sidx)) {
      if (mgr->active[sidx]) {
        s = &mgr->sessions[sidx];
        rx_video_session_tasklet(impl, s);
      }
      rx_video_session_unlock(mgr, sidx);
    }
  }

  return 0;
}

static void rx_video_session_stat(struct st_rx_video_sessions_mgr* mgr,
                                  struct st_rx_video_session_impl* s) {
  int m_idx = mgr->idx, idx = s->idx;
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st20_stat_last_time) / NS_PER_S;
  double framerate = s->st20_stat_frames_received / time_sec;

  info("RX_VIDEO_SESSION(%d,%d): fps %f, received frames %d, pkts %d\n", m_idx, idx,
       framerate, s->st20_stat_frames_received, s->st20_stat_pkts_received);
  s->st20_stat_frames_received = 0;
  s->st20_stat_pkts_received = 0;
  s->st20_stat_last_time = cur_time_ns;

  if (s->st20_stat_frames_dropped || s->st20_stat_pkts_idx_dropped ||
      s->st20_stat_pkts_offset_dropped) {
    info(
        "RX_VIDEO_SESSION(%d,%d): incomplete frames %d, pkts (idx error: %d, offset "
        "error: %d)\n",
        m_idx, idx, s->st20_stat_frames_dropped, s->st20_stat_pkts_idx_dropped,
        s->st20_stat_pkts_offset_dropped);
    s->st20_stat_frames_dropped = 0;
    s->st20_stat_pkts_idx_dropped = 0;
  }
  if (s->st20_stat_pkts_rtp_ring_full) {
    info("RX_VIDEO_SESSION(%d,%d): rtp dropped pkts %d as ring full\n", m_idx, idx,
         s->st20_stat_pkts_rtp_ring_full);
    s->st20_stat_pkts_rtp_ring_full = 0;
  }
  if (s->st20_stat_pkts_no_slot) {
    info("RX_VIDEO_SESSION(%d,%d): dropped pkts %d as no slot\n", m_idx, idx,
         s->st20_stat_pkts_no_slot);
    s->st20_stat_pkts_no_slot = 0;
  }
  if (s->st20_stat_pkts_redunant_dropped) {
    info("RX_VIDEO_SESSION(%d,%d): redunant dropped pkts %d\n", m_idx, idx,
         s->st20_stat_pkts_redunant_dropped);
    s->st20_stat_pkts_redunant_dropped = 0;
  }
  if (s->st20_stat_pkts_enqueue_fallback) {
    info("RX_VIDEO_SESSION(%d,%d): lcore enqueue fallback pkts %d\n", m_idx, idx,
         s->st20_stat_pkts_enqueue_fallback);
    s->st20_stat_pkts_enqueue_fallback = 0;
  }
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

static int rx_video_session_detach(struct st_main_impl* impl,
                                   struct st_rx_video_sessions_mgr* mgr,
                                   struct st_rx_video_session_impl* s) {
  if (rv_ebu_enabled(mgr->parnet)) rx_video_session_ebu_result(s);
  rx_video_session_stat(mgr, s);
  rx_video_session_uinit_mcast(impl, s);
  rx_video_session_uinit_sw(impl, s);
  rx_video_session_uinit_hw(impl, s);
  return 0;
}

static int rx_video_session_update_src(struct st_main_impl* impl,
                                       struct st_rx_video_session_impl* s,
                                       struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st20_rx_ops* ops = &s->ops;

  rx_video_session_uinit_mcast(impl, s);
  rx_video_session_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], ST_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
  }

  ret = rx_video_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rx_video_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

int st_rx_video_sessions_mgr_update_src(struct st_rx_video_sessions_mgr* mgr,
                                        struct st_rx_video_session_impl* s,
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

  rx_video_session_lock(mgr, sidx);
  ret = rx_video_session_update_src(mgr->parnet, s, src);
  rx_video_session_unlock(mgr, sidx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, sidx, ret);
    return ret;
  }

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
    rte_spinlock_init(&mgr->mutex[i]);
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
      mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
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

  if (!mgr->active[sidx]) {
    err("%s(%d,%d), not active\n", __func__, midx, sidx);
    return -EIO;
  }

  rx_video_session_lock(mgr, sidx);

  rx_video_session_detach(mgr->parnet, mgr, s);

  mgr->active[sidx] = false;

  rx_video_session_unlock(mgr, sidx);

  return 0;
}

int st_rx_video_sessions_mgr_update(struct st_rx_video_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (mgr->active[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
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
    for (int j = 0; j < mgr->max_idx; j++) {
      if (mgr->active[j]) {
        s = &mgr->sessions[j];
        rx_video_session_stat(mgr, s);
      }
    }
  }
}
