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
#include "st_dma.h"
#include "st_fmt.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_sch.h"
#include "st_util.h"

static inline double rv_ebu_pass_rate(struct st_rx_video_ebu_result* ebu_result,
                                      int pass) {
  return (double)pass * 100 / ebu_result->ebu_result_num;
}

static void rx_video_session_ebu_result(struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  struct st_rx_video_ebu_result* ebu_result = &s->ebu_result;

  if (ebu_result->ebu_result_num < 0) {
    err("%s(%d), ebu result not enough\n", __func__, idx);
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
  info("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n", __func__, idx,
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
  if (ebu->frame_idx % (60 * 5) == 0) { /* every 5(60fps)/10(30fps) seconds */
    ebu_result->ebu_result_num++;
    if (!ebu_info->dropped_results) {
      rv_ebu_result(s);
      if (ebu_result->ebu_result_num) {
        info("%s(%d), Compliance Rate Narrow %.2f%%, total %d narrow %d\n\n", __func__,
             s->idx, rv_ebu_pass_rate(ebu_result, ebu_result->compliance_narrow),
             ebu_result->ebu_result_num, ebu_result->compliance_narrow);
      }
    } else {
      if (ebu_result->ebu_result_num > ebu_info->dropped_results) {
        ebu_info->dropped_results = 0;
        ebu_result->ebu_result_num = 0;
      }
    }
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

  if (!ebu_info->init) return;

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

  int st20_total_pkts = s->detector.pkt_per_frame;
  err("%s(%d), st20_total_pkts %d\n", __func__, idx, st20_total_pkts);
  if (!st20_total_pkts) {
    err("%s(%d), can not get total packets number\n", __func__, idx);
    return -EINVAL;
  }

  double ractive = 1080.0 / 1125.0;
  if (ops->interlaced && ops->height <= 576) {
    ractive = (ops->height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }

  ebu_info->frame_time = frame_time;
  ebu_info->frame_time_sampling =
      (double)(fps_tm.sampling_clock_rate) * fps_tm.den / fps_tm.mul;
  ebu_info->trs = frame_time * ractive / st20_total_pkts;
  if (!ops->interlaced) {
    ebu_info->tr_offset =
        ops->height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 750.0);
  } else {
    if (ops->height == 480) {
      ebu_info->tr_offset = frame_time * (20.0 / 525.0) * 2;
    } else if (ops->height == 576) {
      ebu_info->tr_offset = frame_time * (26.0 / 625.0) * 2;
    } else {
      ebu_info->tr_offset = frame_time * (22.0 / 1125.0) * 2;
    }
  }

  ebu_info->c_max_narrow_pass =
      RTE_MAX(4, (double)st20_total_pkts / (43200 * ractive * frame_time_s));
  ebu_info->c_max_wide_pass =
      RTE_MAX(16, (double)st20_total_pkts / (21600 * frame_time_s));

  ebu_info->vrx_full_narrow_pass = RTE_MAX(8, st20_total_pkts / (27000 * frame_time_s));
  ebu_info->vrx_full_wide_pass = RTE_MAX(720, st20_total_pkts / (300 * frame_time_s));

  ebu_info->rtp_offset_max_pass =
      ceil((ebu_info->tr_offset / NS_PER_S) * fps_tm.sampling_clock_rate) + 1;

  ebu_info->dropped_results = 4; /* we drop the first 4 results */

  info("%s[%02d], trs %f tr offset %f sampling %f\n", __func__, idx, ebu_info->trs,
       ebu_info->tr_offset, ebu_info->frame_time_sampling);
  info(
      "%s[%02d], cmax_narrow %d cmax_wide %d vrx_full_narrow %d vrx_full_wide %d "
      "rtp_offset_max %d\n",
      __func__, idx, ebu_info->c_max_narrow_pass, ebu_info->c_max_wide_pass,
      ebu_info->vrx_full_narrow_pass, ebu_info->vrx_full_wide_pass,
      ebu_info->rtp_offset_max_pass);
  ebu_info->init = true;
  return 0;
}

static int rv_detector_init(struct st_main_impl* impl,
                            struct st_rx_video_session_impl* s) {
  struct st_rx_video_detector* detector = &s->detector;
  struct st20_detect_meta* meta = &detector->meta;

  detector->status = ST20_DETECT_STAT_DETECTING;
  detector->bpm = true;
  detector->frame_num = 0;
  detector->single_line = true;
  detector->pkt_per_frame = 0;

  meta->width = 0;
  meta->height = 0;
  meta->fps = ST_FPS_MAX;
  meta->packing = ST20_PACKING_MAX;
  meta->interlaced = false;
  return 0;
}

static void rv_detector_calculate_dimension(struct st_rx_video_session_impl* s,
                                            struct st_rx_video_detector* detector,
                                            int max_line_num) {
  struct st20_detect_meta* meta = &detector->meta;

  dbg("%s(%d), interlaced %d, max_line_num %d\n", __func__, s->idx,
      meta->interlaced ? 1 : 0, max_line_num);
  if (meta->interlaced) {
    switch (max_line_num) {
      case 539:
        meta->height = 1080;
        meta->width = 1920;
        break;
      case 239:
        meta->height = 480;
        meta->width = 640;
        break;
      case 359:
        meta->height = 720;
        meta->width = 1280;
        break;
      case 1079:
        meta->height = 2160;
        meta->width = 3840;
        break;
      case 2159:
        meta->height = 4320;
        meta->width = 7680;
        break;
      default:
        err("%s(%d), max_line_num %d\n", __func__, s->idx, max_line_num);
        break;
    }
  } else {
    switch (max_line_num) {
      case 1079:
        meta->height = 1080;
        meta->width = 1920;
        break;
      case 479:
        meta->height = 480;
        meta->width = 640;
        break;
      case 719:
        meta->height = 720;
        meta->width = 1280;
        break;
      case 2159:
        meta->height = 2160;
        meta->width = 3840;
        break;
      case 4319:
        meta->height = 4320;
        meta->width = 7680;
        break;
      default:
        err("%s(%d), max_line_num %d\n", __func__, s->idx, max_line_num);
        break;
    }
  }
}

static void rv_detector_calculate_fps(struct st_rx_video_session_impl* s,
                                      struct st_rx_video_detector* detector) {
  struct st20_detect_meta* meta = &detector->meta;
  int d0 = detector->rtp_tm[1] - detector->rtp_tm[0];
  int d1 = detector->rtp_tm[2] - detector->rtp_tm[1];

  if (abs(d0 - d1) <= 1) {
    dbg("%s(%d), d0 = %d, d1 = %d\n", __func__, s->idx, d0, d1);
    switch (d0) {
      case 1501:
      case 1502:
        meta->fps = ST_FPS_P59_94;
        return;
      case 3003:
        meta->fps = ST_FPS_P29_97;
        return;
      case 3600:
        meta->fps = ST_FPS_P25;
        return;
      case 1800:
        meta->fps = ST_FPS_P50;
        return;
      default:
        err("%s(%d), err d0 %d d1 %d\n", __func__, s->idx, d0, d1);
        break;
    }
  } else {
    err("%s(%d), err d0 %d d1 %d\n", __func__, s->idx, d0, d1);
  }
}

static void rv_detector_calculate_n_packet(struct st_rx_video_session_impl* s,
                                           struct st_rx_video_detector* detector) {
  int total0 = detector->pkt_num[1] - detector->pkt_num[0];
  int total1 = detector->pkt_num[2] - detector->pkt_num[1];

  if (total0 == total1) {
    detector->pkt_per_frame = total0;
  } else {
    err("%s(%d), err total0 %d total1 %d\n", __func__, s->idx, total0, total1);
  }
}

static void rv_detector_calculate_packing(struct st_rx_video_session_impl* s,
                                          struct st_rx_video_detector* detector) {
  struct st20_detect_meta* meta = &detector->meta;

  if (detector->bpm)
    meta->packing = ST20_PACKING_BPM;
  else if (detector->single_line)
    meta->packing = ST20_PACKING_GPM_SL;
  else
    meta->packing = ST20_PACKING_GPM;
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
  size_t size = s->st20_uframe_size ? s->st20_uframe_size : s->st20_frame_size;
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

static inline void rv_slot_init_frame_size(struct st_rx_video_session_impl* s,
                                           struct st_rx_video_slot_impl* slot) {
  slot->frame_recv_size = 0;
  slot->pkt_lcore_frame_recv_size = 0;
}

static inline size_t rv_slot_get_frame_size(struct st_rx_video_session_impl* s,
                                            struct st_rx_video_slot_impl* slot) {
  return slot->frame_recv_size + slot->pkt_lcore_frame_recv_size;
}

static inline void rv_slot_add_frame_size(struct st_rx_video_session_impl* s,
                                          struct st_rx_video_slot_impl* slot,
                                          size_t size) {
  slot->frame_recv_size += size;
}

static inline void rv_slot_pkt_lcore_add_frame_size(struct st_rx_video_session_impl* s,
                                                    struct st_rx_video_slot_impl* slot,
                                                    size_t size) {
  slot->pkt_lcore_frame_recv_size += size;
}

void rx_video_session_slot_dump(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    info("%s(%d), tmstamp %u recv_size %" PRIu64 " pkts_received %u\n", __func__, i,
         slot->tmstamp, rv_slot_get_frame_size(s, slot), slot->pkts_received);
  }
}

static int rx_video_session_init(struct st_main_impl* impl,
                                 struct st_rx_video_sessions_mgr* mgr,
                                 struct st_rx_video_session_impl* s, int idx) {
  s->idx = idx;
  s->sch_idx = mgr->idx;
  s->parnet = impl;
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
    if (slot->slice_info) {
      st_rte_free(slot->slice_info);
      slot->slice_info = NULL;
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
  struct st_rx_video_slot_slice_info* slice_info;
  enum st20_type type = s->ops.type;

  /* init slot */
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    slot->idx = i;
    slot->frame = NULL;
    rv_slot_init_frame_size(s, slot);
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

    if (ST20_TYPE_SLICE_LEVEL == type) {
      slice_info = st_rte_zmalloc_socket(sizeof(*slice_info), soc_id);
      if (!slice_info) {
        err("%s(%d), slice malloc fail\n", __func__, idx);
        return -ENOMEM;
      }
      slot->slice_info = slice_info;
    }
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
  meta->field = slot->field;
  meta->frame_total_size = s->st20_frame_size;
  meta->uframe_total_size = s->st20_uframe_size;
  meta->frame_recv_size = rv_slot_get_frame_size(s, slot);
  if (meta->frame_recv_size >= s->st20_frame_size) {
    meta->status = ST20_FRAME_STATUS_COMPLETE;
    if (ops->num_port > 1) {
      dbg("%s(%d): pks redunant %u received %u\n", __func__, s->idx,
          slot->pkts_redunant_received, slot->pkts_received);
      if ((slot->pkts_redunant_received + 16) < slot->pkts_received)
        meta->status = ST20_FRAME_STATUS_RECONSTRUCTED;
    }
    rte_atomic32_inc(&s->st20_stat_frames_received);

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
    dbg("%s(%d): frame_recv_size %" PRIu64 ", frame_total_size %" PRIu64 ", tmstamp %u\n",
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

static void rx_st22_frame_notify(struct st_rx_video_session_impl* s,
                                 struct st_rx_video_slot_impl* slot) {
  struct st20_rx_ops* ops = &s->ops;
  struct st22_frame_meta* meta = &slot->st22_meta;

  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->frame_total_size = rv_slot_get_frame_size(s, slot);

  /* notify frame */
  int ret = -EIO;
  struct st22_rx_video_info* st22_info = s->st22_info;

  rte_atomic32_inc(&s->st20_stat_frames_received);
  if (st22_info->notify_frame_ready)
    ret = st22_info->notify_frame_ready(ops->priv, slot->frame, meta);
  if (ret < 0) {
    err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
    st_rx_video_session_put_frame(s, slot->frame);
    slot->frame = NULL;
  }
}

static void rx_video_slice_notify(struct st_rx_video_session_impl* s,
                                  struct st_rx_video_slot_impl* slot,
                                  struct st_rx_video_slot_slice_info* slice_info) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_slice_meta* meta = &s->slice_meta;

  /* w, h, fps, fmt, etc are fixed info */
  meta->timestamp = slot->tmstamp;
  meta->field = slot->field;
  meta->frame_recv_size = rv_slot_get_frame_size(s, slot);
  meta->frame_recv_lines = slice_info->ready_slices * s->slice_lines;
  ops->notify_slice_ready(ops->priv, slot->frame, meta);
  s->st20_stat_slices_received++;
}

static void rx_video_slice_add(struct st_rx_video_session_impl* s,
                               struct st_rx_video_slot_impl* slot, uint32_t offset,
                               uint32_t size) {
  struct st_rx_video_slot_slice_info* slice_info = slot->slice_info;
  struct st_rx_video_slot_slice* main_slice = &slice_info->slices[0];
  uint32_t ready_slices = 0;
  struct st_rx_video_slot_slice* slice;

  /* main slice always start from 0(seq_id_base) */
  if (unlikely(offset != main_slice->size)) {
    /* check all slice and try to append */
    for (int i = 1; i < ST_VIDEO_RX_SLICE_NUM; i++) {
      slice = &slice_info->slices[i];
      if (!slice->size) { /* a null slice */
        slice->offset = offset;
        slice->size = size;
        slice_info->extra_slices++;
        dbg("%s(%d), slice(%u:%u) add to %d\n", __func__, s->idx, offset, size, i);
        return;
      }

      /* append to exist slice */
      if (offset == (slice->size + slice->offset)) {
        slice->size += size;
        return;
      }
    }

    s->st20_stat_pkts_slice_fail++;
    return;
  }

  main_slice->size += size;
  if (unlikely(slice_info->extra_slices)) {
    /* try to merge the slice */
    bool merged;
  repeat_merge:
    merged = false;
    for (int i = 1; i < ST_VIDEO_RX_SLICE_NUM; i++) {
      slice = &slice_info->slices[i];
      if (slice->size && (slice->offset == main_slice->size)) {
        main_slice->size += slice->size;
        slice->size = 0;
        slice->offset = 0;
        merged = true;
        slice_info->extra_slices--;
        s->st20_stat_pkts_slice_merged++;
        dbg("%s(%d), slice %d(%u:%u) merge to main\n", __func__, s->idx, i, offset, size);
      }
    }
    if (merged) goto repeat_merge;
  }

  /* check ready slice */
  ready_slices = main_slice->size / s->slice_size;
  if (ready_slices > slice_info->ready_slices) {
    dbg("%s(%d), ready_slices %u\n", __func__, s->idx, ready_slices);
    slice_info->ready_slices = ready_slices;
    rx_video_slice_notify(s, slot, slice_info);
  }
}

static struct st_rx_video_slot_impl* rx_video_frame_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp) {
  int i, slot_idx;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < s->slot_max; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) return slot;
  }

  if (s->dma_dev && !st_dma_empty(s->dma_dev)) {
    /* still in progress of previous frame, drop current pkt */
    rte_atomic32_inc(&s->dma_previous_busy_cnt);
    dbg("%s: still has dma inflight %u\n", __func__,
        s->dma_dev->nb_borrowed[s->dma_lender]);
    return NULL;
  }

  slot_idx = (s->slot_idx + 1) % s->slot_max;
  slot = &s->slots[slot_idx];
  // rx_video_session_slot_dump(s);

  if ((!s->st22_info) && (rv_slot_get_frame_size(s, slot) > 0)) {
    /* drop frame */
    if (slot->frame) {
      rx_video_frame_notify(s, slot);
      slot->frame = NULL;
    }
    rv_slot_init_frame_size(s, slot);
  }
  /* put the frame if any */
  if (slot->frame) {
    s->st20_stat_frames_dropped++;
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
  slot->frame_iova = rte_malloc_virt2iova(slot->frame);
  s->dma_slot = slot;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);
  if (slot->slice_info) memset(slot->slice_info, 0x0, sizeof(*slot->slice_info));

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

static void rx_video_session_slot_full_frame(struct st_rx_video_session_impl* s,
                                             struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rx_video_frame_notify(s, slot);
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  slot->frame = NULL; /* frame pass to app */
}

static void rx_st22_session_slot_full_frame(struct st_rx_video_session_impl* s,
                                            struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rx_st22_frame_notify(s, slot);
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  slot->frame = NULL; /* frame pass to app */
}

static void rx_st22_session_slot_drop_frame(struct st_rx_video_session_impl* s,
                                            struct st_rx_video_slot_impl* slot) {
  st_rx_video_session_put_frame(s, slot->frame);
  slot->frame = NULL;
  s->st20_stat_frames_dropped++;
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
}

static int rx_video_session_free_dma(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s) {
  if (s->dma_dev) {
    st_dma_free_dev(impl, s->dma_dev);
    s->dma_dev = NULL;
  }

  return 0;
}

static int rx_video_slice_dma_drop_mbuf(void* priv, struct rte_mbuf* mbuf) {
  struct st_rx_video_session_impl* s = priv;
  rx_video_slice_add(s, s->dma_slot, st_rx_mbuf_get_offset(mbuf),
                     st_rx_mbuf_get_len(mbuf));
  return 0;
}

static int rx_video_session_init_dma(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int idx = s->idx;
  bool share_dma = true;
  enum st20_type type = s->ops.type;

  struct st_dma_request_req req;
  req.nb_desc = s->dma_nb_desc;
  req.max_shared = share_dma ? ST_DMA_MAX_SESSIONS : 1;
  req.sch_idx = s->sch_idx;
  req.socket_id = st_socket_id(impl, port);
  req.priv = s;
  if (type == ST20_TYPE_SLICE_LEVEL)
    req.drop_mbuf_cb = rx_video_slice_dma_drop_mbuf;
  else
    req.drop_mbuf_cb = NULL;
  struct st_dma_lender_dev* dma_dev = st_dma_request_dev(impl, &req);
  if (!dma_dev) {
    info("%s(%d), fail, can not request dma dev\n", __func__, idx);
    return -EIO;
  }

  s->dma_dev = dma_dev;

  info("%s(%d), succ, dma %d lender id %u\n", __func__, idx, st_dma_dev_id(dma_dev),
       st_dma_lender_id(dma_dev));
  return 0;
}

#ifdef ST_PCAPNG_ENABLED
int st_rx_video_session_start_pcapng(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s,
                                     uint32_t max_dump_packets, bool sync,
                                     struct st_pcap_dump_meta* meta) {
  if (s->pcapng != NULL) {
    err("%s, pcapng dump already started\n", __func__);
    return -EIO;
  }

  enum st_port port = s->port_maps[ST_SESSION_PORT_P];
  int idx = s->idx;
  int pkt_len = ST_PKT_MAX_ETHER_BYTES;

  char file_name[ST_PCAP_FILE_MAX_LEN];
  if (s->st22_info) {
    snprintf(file_name, ST_PCAP_FILE_MAX_LEN, "st22_rx_%d_%u_XXXXXX.pcapng", idx,
             max_dump_packets);
  } else {
    snprintf(file_name, ST_PCAP_FILE_MAX_LEN, "st20_rx_%d_%u_XXXXXX.pcapng", idx,
             max_dump_packets);
  }

  /* mkstemps needs windows wrapping */
  int fd = mkstemps(file_name, strlen(".pcapng"));
  if (fd == -1) {
    err("%s(%d), failed to open pcapng file\n", __func__, idx);
    return -EIO;
  }

  struct rte_pcapng* pcapng = rte_pcapng_fdopen(fd, NULL, NULL, "kahawai-rx-video", NULL);
  if (pcapng == NULL) {
    err("%s(%d), failed to create pcapng\n", __func__, idx);
    close(fd);
    return -EIO;
  }

  struct rte_mempool* mp = rte_pktmbuf_pool_create_by_ops(
      "pcapng_test_pool", 256, 0, 0, rte_pcapng_mbuf_size(pkt_len),
      st_socket_id(impl, port), "ring_mp_sc");

  if (mp == NULL) {
    err("%s(%d), failed to create pcapng mempool\n", __func__, idx);
    rte_pcapng_close(pcapng);
    return -ENOMEM;
  }

  s->pcapng_pool = mp;
  s->pcapng_dumped_pkts = 0;
  s->pcapng_dropped_pkts = 0;
  s->pcapng_max_pkts = max_dump_packets;
  s->pcapng = pcapng;
  info("%s(%d), pcapng (%s,%u) started, pcapng pool at %p\n", __func__, idx, file_name,
       max_dump_packets, mp);

  if (sync) {
    int time_out = 100; /* 100*100ms, 10s */
    int i = 0;
    for (; i < time_out; i++) {
      if (!s->pcapng) break;
      st_sleep_ms(100);
    }
    if (i >= time_out) {
      err("%s(%d), pcapng(%s,%u) dump timeout\n", __func__, idx, file_name,
          max_dump_packets);
      return -EIO;
    }
    if (meta) {
      meta->dumped_packets = s->pcapng_dumped_pkts;
      snprintf(meta->file_name, ST_PCAP_FILE_MAX_LEN, "%s", file_name);
    }
    info("%s(%d), pcapng(%s,%u) dump finish\n", __func__, idx, file_name,
         max_dump_packets);
  }

  return 0;
}

static int rx_video_session_stop_pcapng(struct st_rx_video_session_impl* s) {
  s->pcapng_dropped_pkts = 0;
  s->pcapng_max_pkts = 0;

  if (s->pcapng) {
    rte_pcapng_close(s->pcapng);
    s->pcapng = NULL;
  }

  if (s->pcapng_pool) {
    rte_mempool_free(s->pcapng_pool);
    s->pcapng_pool = NULL;
  }
  return 0;
}

static int rx_video_session_dump_pcapng(struct st_main_impl* impl,
                                        struct st_rx_video_session_impl* s,
                                        struct rte_mbuf** mbuf, uint16_t rv, int s_port) {
  struct rte_mbuf* pcapng_mbuf[rv];
  int pcapng_mbuf_cnt = 0;
  ssize_t len;
  struct st_interface* inf = st_if(impl, st_port_logic2phy(s->port_maps, s_port));

  for (uint16_t i = 0; i < rv; i++) {
    struct rte_mbuf* mc;
    uint64_t timestamp_cycle, timestamp_ns;
    if (st_has_ebu(impl) && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
      timestamp_cycle = 0;
      timestamp_ns = st_mbuf_get_hw_time_stamp(impl, mbuf[i]);
    } else {
      timestamp_cycle = rte_get_tsc_cycles();
      timestamp_ns = 0;
    }
    mc = rte_pcapng_copy(s->port_id[s_port], s->queue_id[s_port], mbuf[i], s->pcapng_pool,
                         ST_PKT_MAX_ETHER_BYTES, timestamp_cycle, timestamp_ns,
                         RTE_PCAPNG_DIRECTION_IN);
    if (mc == NULL) {
      dbg("%s(%d,%d), can not copy packet\n", __func__, s->idx, s_port);
      s->pcapng_dropped_pkts++;
      continue;
    }
    pcapng_mbuf[pcapng_mbuf_cnt++] = mc;
  }
  len = rte_pcapng_write_packets(s->pcapng, pcapng_mbuf, pcapng_mbuf_cnt);
  rte_pktmbuf_free_bulk(&pcapng_mbuf[0], pcapng_mbuf_cnt);
  if (len <= 0) {
    dbg("%s(%d,%d), can not write packet\n", __func__, s->idx, s_port);
    s->pcapng_dropped_pkts++;
    return -EIO;
  }
  s->pcapng_dumped_pkts += pcapng_mbuf_cnt;
  return 0;
}

#else
int st_rx_video_session_start_pcapng(struct st_main_impl* impl,
                                     struct st_rx_video_session_impl* s,
                                     uint32_t max_dump_packets, bool sync,
                                     struct st_pcap_dump_meta* meta) {
  return -EINVAL;
}
#endif

static int rx_video_session_dma_dequeue(struct st_main_impl* impl,
                                        struct st_rx_video_session_impl* s) {
  struct st_dma_lender_dev* dma_dev = s->dma_dev;

  uint16_t nb_dq = st_dma_completed(dma_dev, ST_RX_VIDEO_BURTS_SIZE, NULL, NULL);

  if (nb_dq) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    st_dma_drop_mbuf(dma_dev, nb_dq);
  }

  /* all dma action finished */
  struct st_rx_video_slot_impl* dma_slot = s->dma_slot;
  if (st_dma_empty(dma_dev) && dma_slot) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    int32_t frame_recv_size = rv_slot_get_frame_size(s, dma_slot);
    if (frame_recv_size >= s->st20_frame_size) {
      dbg("%s(%d): full frame\n", __func__, s->idx);
      rx_video_session_slot_full_frame(s, dma_slot);
      s->dma_slot = NULL;
    }
  }

  return 0;
}

static inline uint32_t rfc4175_rtp_seq_id(struct st20_rfc4175_rtp_hdr* rtp) {
  uint16_t seq_id_base = ntohs(rtp->base.seq_number);
  uint16_t seq_id_ext = ntohs(rtp->seq_number_ext);
  uint32_t seq_id = (uint32_t)seq_id_base | (((uint32_t)seq_id_ext) << 16);
  return seq_id;
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
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint16_t line1_length = ntohs(rtp->row_length); /* 1200 for 1080p */
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint32_t seq_id_u32 = rfc4175_rtp_seq_id(rtp);
  uint8_t payload_type = rtp->base.payload_type;
  int pkt_idx = -1, ret;

  if (payload_type != ops->payload_type) {
    s->st20_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_frame_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    s->st20_stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  slot->field = (line1_number & ST20_SECOND_FIELD) ? SECOND_FIELD : FIRST_FIELD;
  line1_number &= ~ST20_SECOND_FIELD;
  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (seq_id_u32 >= slot->seq_id_base_u32)
      pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
    else
      pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base_u32);
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
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id not got, %u %u\n", __func__, s->idx,
          s_port, seq_id_u32, line1_number, line1_offset);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  if (!slot->frame) {
    dbg("%s(%d,%d): slot frame not inited\n", __func__, s->idx, s_port);
    s->st20_stat_pkts_no_slot++;
    return -EIO;
  }

  /* caculate offset */
  uint32_t offset =
      (line1_number * ops->width + line1_offset) / s->st20_pg.coverage * s->st20_pg.size;

  uint32_t payload_length = line1_length;
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);

  if ((offset + payload_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    dbg("%s, number %u offset %u len %u\n", __func__, line1_number, line1_offset,
        line1_length);
    s->st20_stat_pkts_offset_dropped++;
    return -EIO;
  }

  bool dma_copy = false;
  struct st_dma_lender_dev* dma_dev = s->dma_dev;
  bool ebu = st_has_ebu(impl);
  if (ebu) {
    /* no copy for ebu */
  } else if (s->st20_uframe_size) {
    /* user frame mode, pass to app to handle the payload */
    struct st20_uframe_pg_meta* pg_meta = &s->pg_meta;
    pg_meta->payload = payload;
    pg_meta->row_length = line1_length;
    pg_meta->row_number = line1_number;
    pg_meta->row_offset = line1_offset;
    pg_meta->pg_cnt = line1_length / s->st20_pg.size;
    ops->uframe_pg_callback(ops->priv, slot->frame, pg_meta);
    if (extra_rtp) {
      pg_meta->payload = payload + line1_length;
      pg_meta->row_length = ntohs(extra_rtp->row_length);
      pg_meta->row_number = ntohs(extra_rtp->row_number);
      pg_meta->row_offset = ntohs(extra_rtp->row_offset);
      pg_meta->pg_cnt = pg_meta->row_length / s->st20_pg.size;
      ops->uframe_pg_callback(ops->priv, slot->frame, pg_meta);
    }
  } else {
    /* copy the payload to target frame */
    if (dma_dev && (payload_length > ST_RX_VIDEO_DMA_MIN_SIZE) && !st_dma_full(dma_dev)) {
      rte_iova_t payload_iova =
          rte_pktmbuf_iova_offset(mbuf, sizeof(struct st_rfc4175_video_hdr));
      if (extra_rtp) payload_iova += sizeof(*extra_rtp);
      ret = st_dma_copy(dma_dev, slot->frame_iova + offset, payload_iova, payload_length);
      if (ret < 0) {
        /* use cpu copy if dma copy fail */
        rte_memcpy(slot->frame + offset, payload, payload_length);
      } else {
        /* abstrct dma dev takes ownership of this mbuf */
        st_rx_mbuf_set_offset(mbuf, offset);
        st_rx_mbuf_set_len(mbuf, payload_length);
        ret = st_dma_borrow_mbuf(dma_dev, mbuf);
        if (ret) { /* never happen in real life */
          err("%s(%d,%d), mbuf copied but not enqueued \n", __func__, s->idx, s_port);
          /* mbuf freed and dma copy will operate an invalid src! */
          rte_pktmbuf_free(mbuf);
        }
        dma_copy = true;
        s->st20_stat_pkts_dma++;
      }
    } else {
      rte_memcpy(slot->frame + offset, payload, payload_length);
    }
  }

  if (ctrl_thread) {
    rv_slot_pkt_lcore_add_frame_size(s, slot, payload_length);
  } else {
    rv_slot_add_frame_size(s, slot, payload_length);
  }
  s->st20_stat_pkts_received++;
  slot->pkts_received++;

  /* slice */
  if (slot->slice_info && !dma_copy) { /* ST20_TYPE_SLICE_LEVEL */
    rx_video_slice_add(s, slot, offset, payload_length);
  }

  /* check if frame is full */
  size_t frame_recv_size = rv_slot_get_frame_size(s, slot);
  bool end_frame = false;
  if (dma_dev) {
    if (frame_recv_size >= s->st20_frame_size && st_dma_empty(dma_dev)) end_frame = true;
  } else {
    if (frame_recv_size >= s->st20_frame_size) end_frame = true;
  }
  if (end_frame) {
    dbg("%s(%d,%d): full frame on %p(%d)\n", __func__, s->idx, s_port, slot->frame,
        frame_recv_size);
    dbg("%s(%d,%d): tmstamp %u slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    /* end of frame */
    rx_video_session_slot_full_frame(s, slot);
  }

  if (ebu && inf->feature & ST_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
    rv_ebu_on_packet(s, tmstamp, st_mbuf_get_hw_time_stamp(impl, mbuf), pkt_idx);
  }

  /* indicate caller not to free mbuf as dma copy */
  return dma_copy ? 1 : 0;
}

static int rx_video_session_handle_rtp_pkt(struct st_main_impl* impl,
                                           struct st_rx_video_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum st_session_port s_port) {
  struct st20_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  uint8_t payload_type = rtp->payload_type;
  int pkt_idx = -1;

  if (payload_type != ops->payload_type) {
    dbg("%s, payload_type mismatch %d %d\n", __func__, payload_type, ops->payload_type);
    s->st20_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

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
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
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
      rte_atomic32_inc(&s->st20_stat_frames_received);
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, s->idx,
          s_port, seq_id, slot->seq_id_base);
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

  return 0;
}

struct st22_box {
  uint32_t lbox; /* box lenght */
  char tbox[4];
};

/* Video Support Box and Color Specification Box */
static int rx_video_session_parse_st22_boxes(struct st_main_impl* impl,
                                             struct st_rx_video_session_impl* s,
                                             void* boxes,
                                             struct st_rx_video_slot_impl* slot) {
  uint32_t jpvs_len = 0;
  uint32_t colr_len = 0;
  struct st22_box* box;

  box = boxes;
  if (0 == strncmp(box->tbox, "jpvs", 4)) {
    jpvs_len = ntohl(box->lbox);
    boxes += jpvs_len;
  }

  box = boxes;
  if (0 == strncmp(box->tbox, "colr", 4)) {
    colr_len = ntohl(box->lbox);
    boxes += colr_len;
  }

  if ((jpvs_len + colr_len) > 512) {
    info("%s(%d): err jpvs_len %u colr_len %u\n", __func__, s->idx, jpvs_len, colr_len);
    return -EIO;
  }

  slot->st22_box_hdr_length = jpvs_len + colr_len;
  dbg("%s(%d): st22_box_hdr_length %u\n", __func__, s->idx, slot->st22_box_hdr_length);

#if 0
  uint8_t* buf = boxes - slot->st22_box_hdr_length;
  for (uint16_t i = 0; i < slot->st22_box_hdr_length; i++) {
    printf("0x%02x ", buf[i]);
  }
  info("end\n");
#endif
  return 0;
}

static int rx_video_session_handle_st22_pkt(struct st_main_impl* impl,
                                            struct st_rx_video_session_impl* s,
                                            struct rte_mbuf* mbuf,
                                            enum st_session_port s_port) {
  struct st20_rx_ops* ops = &s->ops;
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st22_rfc9134_video_hdr) - sizeof(struct st22_rfc9134_rtp_hdr);
  struct st22_rfc9134_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st22_rfc9134_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t payload_length = mbuf->data_len - sizeof(struct st22_rfc9134_video_hdr);
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint16_t seq_id = ntohs(rtp->base.seq_number);
  uint8_t payload_type = rtp->base.payload_type;
  uint16_t p_counter = (uint16_t)rtp->p_counter_lo + ((uint16_t)rtp->p_counter_hi << 8);
  uint16_t sep_counter =
      (uint16_t)rtp->sep_counter_lo + ((uint16_t)rtp->sep_counter_hi << 5);
  int pkt_counter = p_counter + sep_counter * 2048;
  int pkt_idx = -1;
  int ret;

  if (payload_type != ops->payload_type) {
    s->st20_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  if (rtp->kmode) {
    s->st20_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rx_video_frame_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    s->st20_stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  dbg("%s(%d,%d), seq_id %d kmode %u trans_order %u\n", __func__, s->idx, s_port, seq_id,
      rtp->kmode, rtp->trans_order);
  dbg("%s(%d,%d), seq_id %d p_counter %u sep_counter %u\n", __func__, s->idx, s_port,
      seq_id, p_counter, sep_counter);

  if (slot->seq_id_got) {
    if (!rtp->base.marker && (payload_length != slot->st22_payload_length)) {
      s->st20_stat_pkts_wrong_hdr_dropped++;
      return -EIO;
    }
    /* check if the same pks got already */
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
    /* first packet */
    if (!pkt_counter) { /* first packet */
      ret = rx_video_session_parse_st22_boxes(impl, s, payload, slot);
      if (ret < 0) {
        s->st20_stat_pkts_idx_dropped++;
        return -EIO;
      }
      slot->seq_id_base = seq_id;
      slot->st22_payload_length = payload_length;
      slot->seq_id_got = true;
      st_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), get seq_id %d tmstamp %u, p_counter %u sep_counter %u, "
          "payload_length %u\n",
          __func__, s->idx, s_port, seq_id, tmstamp, p_counter, sep_counter,
          payload_length);
    } else {
      dbg("%s(%d,%d), drop seq_id %d tmstamp %u as base seq not got, p_counter %u "
          "sep_counter %u\n",
          __func__, s->idx, s_port, seq_id, tmstamp, p_counter, sep_counter);
      s->st20_stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  if (!slot->frame) {
    dbg("%s(%d,%d): slot frame not inited\n", __func__, s->idx, s_port);
    s->st20_stat_pkts_no_slot++;
    return -EIO;
  }

  /* copy payload */
  uint32_t offset;
  if (!pkt_counter) { /* first pkt */
    offset = 0;
    payload += slot->st22_box_hdr_length;
    payload_length -= slot->st22_box_hdr_length;
  } else {
    offset = pkt_counter * slot->st22_payload_length - slot->st22_box_hdr_length;
  }
  if ((offset + payload_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    s->st20_stat_pkts_offset_dropped++;
    return -EIO;
  }
  rte_memcpy(slot->frame + offset, payload, payload_length);
  rv_slot_add_frame_size(s, slot, payload_length);
  s->st20_stat_pkts_received++;
  slot->pkts_received++;

  /* check if frame is full */
  if (rtp->base.marker) {
    size_t expect_frame_size = offset + payload_length;
    if (expect_frame_size == rv_slot_get_frame_size(s, slot))
      rx_st22_session_slot_full_frame(s, slot);
    else
      rx_st22_session_slot_drop_frame(s, slot);
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
  struct rte_mbuf* pkt = NULL;

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
  rx_video_session_free_dma(impl, s);
  rx_video_session_free_frames(s);
  rx_video_session_free_rtps(s);
  rx_video_session_uinit_slot(s);
  if (s->st22_info) {
    st_rte_free(s->st22_info);
    s->st22_info = NULL;
  }
  return 0;
}

static int rx_video_session_init_st22_frame(struct st_main_impl* impl,
                                            struct st_rx_video_session_impl* s,
                                            struct st22_rx_ops* st22_frame_ops) {
  struct st22_rx_video_info* st22_info;

  st22_info = st_rte_zmalloc_socket(sizeof(*st22_info), st_socket_id(impl, ST_PORT_P));
  if (!st22_info) return -ENOMEM;

  st22_info->notify_frame_ready = st22_frame_ops->notify_frame_ready;

  st22_info->meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;

  s->st22_info = st22_info;

  return 0;
}

static int rx_video_session_init_sw(struct st_main_impl* impl,
                                    struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s,
                                    struct st22_rx_ops* st22_ops) {
  struct st20_rx_ops* ops = &s->ops;
  enum st20_type type = ops->type;
  int idx = s->idx;
  int ret;

  if (st22_ops) {
    ret = rx_video_session_init_st22_frame(impl, s, st22_ops);
    if (ret < 0) {
      err("%s(%d), st22 frame init fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  if (st20_is_frame_type(type)) {
    ret = rx_video_session_alloc_frames(impl, s);
  } else if (type == ST20_TYPE_RTP_LEVEL) {
    ret = rx_video_session_alloc_rtps(impl, mgr, s);
  } else {
    err("%s(%d), error type %d\n", __func__, idx, type);
    return -EIO;
  }
  if (ret < 0) {
    rx_video_session_uinit_sw(impl, s);
    return ret;
  }

  ret = rx_video_session_init_slot(impl, s);
  if (ret < 0) {
    rx_video_session_uinit_sw(impl, s);
    return ret;
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    struct st20_slice_meta* slice_meta = &s->slice_meta;
    slice_meta->width = ops->width;
    slice_meta->height = ops->height;
    slice_meta->fmt = ops->fmt;
    slice_meta->fps = ops->fps;
    slice_meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    slice_meta->frame_total_size = s->st20_frame_size;
    slice_meta->uframe_total_size = s->st20_uframe_size;
    slice_meta->field = FIRST_FIELD;
    info("%s(%d), slice lines %u\n", __func__, idx, s->slice_lines);
  }

  if (s->st20_uframe_size) {
    /* user frame mode */
    struct st20_uframe_pg_meta* pg_meta = &s->pg_meta;
    pg_meta->width = ops->width;
    pg_meta->height = ops->height;
    pg_meta->fmt = ops->fmt;
    pg_meta->fps = ops->fps;
    pg_meta->frame_total_size = s->st20_frame_size;
    pg_meta->uframe_total_size = s->st20_uframe_size;
    info("%s(%d), uframe size %" PRIu64 "\n", __func__, idx, s->st20_uframe_size);
  }

  /* try to request dma dev */
  if (st20_is_frame_type(type) && (ops->flags & ST20_RX_FLAG_DMA_OFFLOAD) &&
      (!s->st20_uframe_size)) {
    rx_video_session_init_dma(impl, s);
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
  if ((bps / (1000 * 1000)) > (40 * 1000)) {
    if (!s->dma_dev) pkt_handle_lcore = true;
  }

  if (pkt_handle_lcore) {
    if (type == ST20_TYPE_SLICE_LEVEL) {
      err("%s(%d), additional pkt lcore not support slice type\n", __func__, idx);
      rx_video_session_uinit_sw(impl, s);
      return -EINVAL;
    }
    ret = rx_video_session_init_pkt_lcore(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), init_pkt_lcore fail %d\n", __func__, idx, ret);
      rx_video_session_uinit_sw(impl, s);
      return ret;
    }
    /* enable multi slot as it has two threads running */
    s->slot_max = ST_VIDEO_RX_REC_NUM_OFO;
  }

  if (st_has_ebu(impl)) {
    rv_ebu_init(impl, s);
  }

  return 0;
}

static int rx_video_session_handle_detect_pkt(struct st_main_impl* impl,
                                              struct st_rx_video_session_impl* s,
                                              struct st_rx_video_sessions_mgr* mgr,
                                              struct rte_mbuf* mbuf,
                                              enum st_session_port s_port) {
  int ret;
  struct st20_rx_ops* ops = &s->ops;
  struct st_rx_video_detector* detector = &s->detector;
  struct st20_detect_meta* meta = &detector->meta;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t line1_number = ntohs(rtp->row_number);
  uint16_t line1_offset = ntohs(rtp->row_offset);
  /* detect field bit */
  if (line1_number & ST20_SECOND_FIELD) meta->interlaced = true;
  line1_number &= ~ST20_SECOND_FIELD;
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint32_t payload_length = ntohs(rtp->row_length);
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint8_t payload_type = rtp->base.payload_type;

  if (payload_type != ops->payload_type) {
    dbg("%s, payload_type mismatch %d %d\n", __func__, payload_type, ops->payload_type);
    s->st20_stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* detect continuous bit */
  if (extra_rtp) detector->single_line = false;
  /* detect bpm */
  if (payload_length % 180 != 0) detector->bpm = false;
  /* on frame/field marker bit */
  if (rtp->base.marker) {
    if (detector->frame_num < 3) {
      detector->rtp_tm[detector->frame_num] = tmstamp;
      detector->pkt_num[detector->frame_num] = s->st20_stat_pkts_received;
      detector->frame_num++;
    } else {
      rv_detector_calculate_dimension(s, detector, line1_number);
      rv_detector_calculate_fps(s, detector);
      rv_detector_calculate_n_packet(s, detector);
      rv_detector_calculate_packing(s, detector);
      detector->frame_num = 0;
    }
    if (meta->fps != ST_FPS_MAX && meta->packing != ST20_PACKING_MAX) {
      if (!meta->height) {
        detector->status = ST20_DETECT_STAT_FAIL;
        err("%s(%d,%d): st20 failed to detect dimension, max_line: %d\n", __func__,
            s->idx, s_port, line1_number);
      } else { /* detected */
        ops->width = meta->width;
        ops->height = meta->height;
        ops->fps = meta->fps;
        ops->packing = meta->packing;
        ops->interlaced = meta->interlaced;
        if (ops->notify_detected) {
          struct st20_detect_reply reply = {0};
          ret = ops->notify_detected(ops->priv, meta, &reply);
          if (ret < 0) {
            err("%s(%d), notify_detected return fail %d\n", __func__, s->idx, ret);
            detector->status = ST20_DETECT_STAT_FAIL;
            return ret;
          }
          s->slice_lines = reply.slice_lines;
          s->st20_uframe_size = reply.uframe_size;
          info("%s(%d), detected, slice_lines %u, uframe_size %ld\n", __func__, s->idx,
               s->slice_lines, s->st20_uframe_size);
        }
        if (!s->slice_lines) s->slice_lines = ops->height / 32;
        s->slice_size =
            ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
        s->st20_frames_cnt = ops->framebuff_cnt;
        s->st20_frame_size =
            ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
        if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
        /* at least 1000 byte for each packet */
        s->st20_frame_bitmap_size = s->st20_frame_size / 1000 / 8;
        /* one line at line 2 packets for all the format */
        if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
          s->st20_frame_bitmap_size = ops->height * 2 / 8;
        ret = rx_video_session_init_sw(impl, mgr, s, NULL);
        if (ret < 0) {
          err("%s(%d), rx_video_session_init_sw fail %d\n", __func__, s->idx, ret);
          detector->status = ST20_DETECT_STAT_FAIL;
          return ret;
        }
        detector->status = ST20_DETECT_STAT_SUCCESS;
        info("st20 detected(%d,%d): width: %d, height: %d, fps: %f\n", s->idx, s_port,
             meta->width, meta->height, st_frame_rate(meta->fps));
        info("st20 detected(%d,%d): packing: %d, field: %s, pkts per %s: %d\n", s->idx,
             s_port, meta->packing, meta->interlaced ? "interlaced" : "progressive",
             meta->interlaced ? "field" : "frame", detector->pkt_per_frame);
      }
    }
  }

  s->st20_stat_pkts_received++;
  return 0;
}

static int rx_video_session_tasklet(struct st_main_impl* impl,
                                    struct st_rx_video_session_impl* s,
                                    struct st_rx_video_sessions_mgr* mgr) {
  struct rte_mbuf* mbuf[ST_RX_VIDEO_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port, ret;
  enum st20_type type = s->ops.type;
  struct rte_ring* pkt_ring = s->pkt_lcore_ring;
  bool ctl_thread = pkt_ring ? false : true;
  bool dma_copy = false;
  bool update_nic_burst = false;

  if (s->dma_dev) rx_video_session_dma_dequeue(impl, s);

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->queue_active[s_port]) continue;
    rv = rte_eth_rx_burst(s->port_id[s_port], s->queue_id[s_port], &mbuf[0],
                          ST_RX_VIDEO_BURTS_SIZE);
    s->pri_nic_burst_cnt++;
    if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
      update_nic_burst = true;
    }
    if (update_nic_burst) {
      rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
      s->pri_nic_burst_cnt = 0;
    }
    if (pkt_ring) {
      /* first pass to the pkt ring if it has pkt handling lcore */
      unsigned int n =
          rte_ring_sp_enqueue_bulk(s->pkt_lcore_ring, (void**)&mbuf[0], rv, NULL);
      rv -= n; /* n is zero or rx */
      s->st20_stat_pkts_enqueue_fallback += rv;
    }
    if (rv > 0) {
      struct rte_mbuf* free_mbuf[ST_RX_VIDEO_BURTS_SIZE];
      int free_mbuf_cnt = 0;

      s->pri_nic_inflight_cnt++;
      if (update_nic_burst) {
        rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
        s->pri_nic_inflight_cnt = 0;
      }

      if (st20_is_frame_type(type)) {
        for (uint16_t i = 0; i < rv; i++) {
          if (s->detector.status == ST20_DETECT_STAT_DETECTING) {
            ret = rx_video_session_handle_detect_pkt(impl, s, mgr, mbuf[i], s_port);
            if (ret < 0)
              err("%s(%d,%d), rx_video_session_handle_detect_pkt fail, %d\n", __func__,
                  s->idx, s_port, ret);
            free_mbuf[free_mbuf_cnt] = mbuf[i];
            free_mbuf_cnt++;
          } else if (s->detector.status == ST20_DETECT_STAT_SUCCESS ||
                     s->detector.status == ST20_DETECT_STAT_DISABLED) {
            if (s->st22_info)
              ret = rx_video_session_handle_st22_pkt(impl, s, mbuf[i], s_port);
            else
              ret =
                  rx_video_session_handle_frame_pkt(impl, s, mbuf[i], s_port, ctl_thread);
            if (ret <= 0) { /* set to free if it is not handle by dma */
              free_mbuf[free_mbuf_cnt] = mbuf[i];
              free_mbuf_cnt++;
            } else {
              dma_copy = true;
            }
          } else {
            err_once("%s(%d,%d), detect fail, please choose the rigth format\n", __func__,
                     s->idx, s_port);
            free_mbuf[free_mbuf_cnt] = mbuf[i];
            free_mbuf_cnt++;
          }
        }
      } else {
        for (uint16_t i = 0; i < rv; i++) {
          ret = rx_video_session_handle_rtp_pkt(impl, s, mbuf[i], s_port);
          if (ret < 0) { /* set to free if it is dropped pkt */
            free_mbuf[free_mbuf_cnt] = mbuf[i];
            free_mbuf_cnt++;
          }
        }
      }
#ifdef ST_PCAPNG_ENABLED
      /* dump mbufs to pcapng file */
      if (s->pcapng != NULL) {
        if (s->pcapng_dumped_pkts < s->pcapng_max_pkts) {
          ret = rx_video_session_dump_pcapng(
              impl, s, mbuf, RTE_MIN(rv, s->pcapng_max_pkts - s->pcapng_dumped_pkts),
              s_port);
          if (ret < 0) continue;
        } else { /* got enough packets, stop dumping */
          info("%s(%d,%d), pcapng dump finished, dumped %u packets, dropped %u pcakets\n",
               __func__, s->idx, s_port, s->pcapng_dumped_pkts, s->pcapng_dropped_pkts);
          rx_video_session_stop_pcapng(s);
        }
      }
#endif
      rte_pktmbuf_free_bulk(&free_mbuf[0], free_mbuf_cnt);
    }
  }

  /* submit if any */
  if (dma_copy && s->dma_dev) st_dma_submit(s->dma_dev);

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
  struct st_rx_flow flow;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    memset(&flow, 0xff, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.sip_addr[i], ST_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, st_sip_addr(impl, port), ST_IP_ADDR_LEN);
    flow.port_flow = true;
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

static int rx_video_session_uinit_mcast(struct st_main_impl* impl,
                                        struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (st_is_multicast_ip(ops->sip_addr[i]))
      st_mcast_leave(impl, st_ip_to_u32(ops->sip_addr[i]),
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
    ret = st_mcast_join(impl, st_ip_to_u32(ops->sip_addr[i]),
                        st_port_logic2phy(s->port_maps, i));
    if (ret < 0) return ret;
  }

  return 0;
}

static int rx_video_session_attach(struct st_main_impl* impl,
                                   struct st_rx_video_sessions_mgr* mgr,
                                   struct st_rx_video_session_impl* s,
                                   struct st20_rx_ops* ops,
                                   struct st22_rx_ops* st22_ops) {
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

  s->slice_lines = ops->slice_lines;
  if (!s->slice_lines) s->slice_lines = ops->height / 32;
  s->slice_size = ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_frames_cnt = ops->framebuff_cnt;
  if (st22_ops)
    s->st20_frame_size = st22_ops->framebuff_max_size;
  else
    s->st20_frame_size = ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_uframe_size = ops->uframe_size;
  if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
  /* at least 1000 byte for each packet */
  s->st20_frame_bitmap_size = s->st20_frame_size / 1000 / 8;
  /* one line at line 2 packets for all the format */
  if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
    s->st20_frame_bitmap_size = ops->height * 2 / 8;
  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
  }

  s->st20_stat_pkts_idx_dropped = 0;
  s->st20_stat_pkts_no_slot = 0;
  s->st20_stat_pkts_offset_dropped = 0;
  s->st20_stat_pkts_redunant_dropped = 0;
  s->st20_stat_pkts_wrong_hdr_dropped = 0;
  s->st20_stat_pkts_received = 0;
  s->st20_stat_pkts_dma = 0;
  s->st20_stat_pkts_rtp_ring_full = 0;
  s->st20_stat_frames_dropped = 0;
  rte_atomic32_set(&s->st20_stat_frames_received, 0);
  s->st20_stat_last_time = st_get_monotonic_time();
  s->dma_nb_desc = 128;
  s->dma_slot = NULL;
  s->dma_dev = NULL;

  s->pri_nic_burst_cnt = 0;
  s->pri_nic_inflight_cnt = 0;
  rte_atomic32_set(&s->nic_burst_cnt, 0);
  rte_atomic32_set(&s->nic_inflight_cnt, 0);
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  s->cpu_busy_score = 0;
  s->dma_busy_score = 0;

  ret = rx_video_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_video_session_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  if (st20_is_frame_type(ops->type) && (!st22_ops) &&
      ((ops->flags & ST20_RX_FLAG_AUTO_DETECT) || st_has_ebu(impl))) {
    /* init sw after detected */
    ret = rv_detector_init(impl, s);
    if (ret < 0) {
      err("%s(%d), rv_detector_init fail %d\n", __func__, idx, ret);
      rx_video_session_uinit_hw(impl, s);
      return -EIO;
    }
  } else {
    ret = rx_video_session_init_sw(impl, mgr, s, st22_ops);
    if (ret < 0) {
      err("%s(%d), rx_video_session_init_sw fail %d\n", __func__, idx, ret);
      rx_video_session_uinit_hw(impl, s);
      return -EIO;
    }
  }

  ret = rx_video_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_video_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_video_session_uinit_sw(impl, s);
    rx_video_session_uinit_hw(impl, s);
    return -EIO;
  }

  info("%s(%d), %d frames with size %" PRIu64 "(%" PRIu64 ",%" PRIu64 "), type %d\n",
       __func__, idx, s->st20_frames_cnt, s->st20_frame_size, s->st20_frame_bitmap_size,
       s->st20_uframe_size, ops->type);
  info("%s(%d), ops info, w %u h %u fmt %d packing %d pt %d\n", __func__, idx, ops->width,
       ops->height, ops->fmt, ops->packing, ops->payload_type);
  return 0;
}

static int rx_video_sessions_tasklet_handler(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_rx_video_session_impl* s;
  int sidx;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    rx_video_session_tasklet(impl, s, mgr);
    rx_video_session_put(mgr, sidx);
  }

  return 0;
}

void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s) {
  rte_atomic32_set(&s->nic_burst_cnt, 0);
  rte_atomic32_set(&s->nic_inflight_cnt, 0);
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  s->cpu_busy_score = 0;
  s->dma_busy_score = 0;
}

void rx_video_session_cal_cpu_busy(struct st_rx_video_session_impl* s) {
  float nic_burst_cnt = rte_atomic32_read(&s->nic_burst_cnt);
  float nic_inflight_cnt = rte_atomic32_read(&s->nic_inflight_cnt);
  float dma_previous_busy_cnt = rte_atomic32_read(&s->dma_previous_busy_cnt);
  float cpu_busy_score = 0;
  float dma_busy_score = s->dma_busy_score; /* save old */

  rx_video_session_clear_cpu_busy(s);

  if (nic_burst_cnt) {
    cpu_busy_score = 100.0 * nic_inflight_cnt / nic_burst_cnt;
  }
  s->cpu_busy_score = cpu_busy_score;
  if (dma_previous_busy_cnt) {
    dma_busy_score += 40.0;
    if (dma_busy_score > 100.0) dma_busy_score = 100.0;
  } else {
    dma_busy_score = 0;
  }
  s->dma_busy_score = dma_busy_score;
}

static int rx_video_session_migrate_dma(struct st_main_impl* impl,
                                        struct st_rx_video_session_impl* s) {
  rx_video_session_free_dma(impl, s);
  rx_video_session_init_dma(impl, s);
  return 0;
}

static void rx_video_session_stat(struct st_rx_video_sessions_mgr* mgr,
                                  struct st_rx_video_session_impl* s) {
  int m_idx = mgr->idx, idx = s->idx;
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st20_stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->st20_stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->st20_stat_frames_received, 0);

  if (s->st20_stat_slices_received) {
    info("RX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d slices %d, cpu busy %f\n",
         m_idx, idx, s->ops_name, framerate, frames_received, s->st20_stat_pkts_received,
         s->st20_stat_slices_received, s->cpu_busy_score);
  } else {
    info("RX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d, cpu busy %f\n", m_idx,
         idx, s->ops_name, framerate, frames_received, s->st20_stat_pkts_received,
         s->cpu_busy_score);
  }
  s->st20_stat_pkts_received = 0;
  s->st20_stat_slices_received = 0;
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
  if (s->st20_stat_pkts_wrong_hdr_dropped) {
    info("RX_VIDEO_SESSION(%d,%d): wrong hdr dropped pkts %d\n", m_idx, idx,
         s->st20_stat_pkts_wrong_hdr_dropped);
    s->st20_stat_pkts_wrong_hdr_dropped = 0;
  }
  if (s->st20_stat_pkts_enqueue_fallback) {
    info("RX_VIDEO_SESSION(%d,%d): lcore enqueue fallback pkts %d\n", m_idx, idx,
         s->st20_stat_pkts_enqueue_fallback);
    s->st20_stat_pkts_enqueue_fallback = 0;
  }
  if (s->dma_dev) {
    info("RX_VIDEO_SESSION(%d,%d): pkts %d by dma copy, dma busy %f\n", m_idx, idx,
         s->st20_stat_pkts_dma, s->dma_busy_score);
    s->st20_stat_pkts_dma = 0;
  }
  if (s->st20_stat_pkts_slice_fail) {
    info("RX_VIDEO_SESSION(%d,%d): pkts %d drop as slice add fail\n", m_idx, idx,
         s->st20_stat_pkts_slice_fail);
    s->st20_stat_pkts_slice_fail = 0;
  }
  if (s->st20_stat_pkts_slice_merged) {
    info("RX_VIDEO_SESSION(%d,%d): pkts %d merged as slice\n", m_idx, idx,
         s->st20_stat_pkts_slice_merged);
    s->st20_stat_pkts_slice_merged = 0;
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
  if (st_has_ebu(mgr->parnet)) rx_video_session_ebu_result(s);
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
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rx_video_session_update_src(mgr->parnet, s, src);
  rx_video_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_video_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                      struct st_rx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  int ret;
  struct st_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
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

static int rx_video_sessions_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                                        struct st_rx_video_session_impl* s, int idx) {
  rx_video_session_detach(mgr->parnet, mgr, s);
  mgr->sessions[idx] = NULL;
  st_rte_free(s);
  return 0;
}

static int rx_video_sessions_mgr_uinit(struct st_rx_video_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_video_session_impl* s;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = rx_video_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rx_video_sessions_mgr_detach(mgr, s, i);
    rx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

struct st_rx_video_session_impl* st_rx_video_sessions_mgr_attach(
    struct st_rx_video_sessions_mgr* mgr, struct st20_rx_ops* ops,
    struct st22_rx_ops* st22_ops) {
  int midx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  int ret;
  struct st_rx_video_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (!rx_video_session_get_empty(mgr, i)) continue;

    s = st_rte_zmalloc_socket(sizeof(*s), st_socket_id(impl, ST_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      return NULL;
    }
    ret = rx_video_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    ret = rx_video_session_attach(mgr->parnet, mgr, s, ops, st22_ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_video_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_rx_video_sessions_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rx_video_sessions_mgr_detach(mgr, s, idx);

  rx_video_session_put(mgr, idx);

  return 0;
}

int st_rx_video_sessions_mgr_update(struct st_rx_video_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_rx_video_sessions_stat(struct st_main_impl* impl) {
  struct st_sch_impl* sch;
  struct st_rx_video_sessions_mgr* mgr;
  struct st_rx_video_session_impl* s;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_is_active(sch)) continue;
    mgr = &sch->rx_video_mgr;
    for (int j = 0; j < mgr->max_idx; j++) {
      s = rx_video_session_get(mgr, j);
      if (!s) continue;
      rx_video_session_stat(mgr, s);
      rx_video_session_put(mgr, j);
    }
  }
}

int st_rx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->rx_video_init) return 0;

  /* create t video context */
  ret = rx_video_sessions_mgr_init(impl, sch, &sch->rx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_rx_video_sessions_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->rx_video_init = true;
  return 0;
}

int st_rx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch) {
  if (!sch->rx_video_init) return 0;

  rx_video_sessions_mgr_uinit(&sch->rx_video_mgr);
  sch->rx_video_init = false;

  return 0;
}

int st_rx_video_session_migrate(struct st_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx) {
  rx_video_session_init(impl, mgr, s, idx);
  if (s->dma_dev) rx_video_session_migrate_dma(impl, s);
  return 0;
}
