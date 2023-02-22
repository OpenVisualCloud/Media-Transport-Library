/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_video_session.h"

#include <math.h>

#include "../mt_log.h"
#include "st_fmt.h"

static int rv_init_pkt_handler(struct st_rx_video_session_impl* s);
static int rvs_mgr_update(struct st_rx_video_sessions_mgr* mgr);

static inline double rv_ebu_pass_rate(struct st_rx_video_ebu_result* ebu_result,
                                      int pass) {
  return (double)pass * 100 / ebu_result->ebu_result_num;
}

static inline struct mtl_main_impl* rv_get_impl(struct st_rx_video_session_impl* s) {
  return s->parnet->parnet;
}

static void rv_ebu_final_result(struct st_rx_video_session_impl* s) {
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
  ebu->cinmtl_initial_time = pkt_tmstamp;
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
      ((pkt_tmstamp - ebu->cinmtl_initial_time) / trs) * ST_EBU_CINST_DRAIN_FACTOR;
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

static int rv_ebu_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
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
  info("%s(%d), st20_total_pkts %d\n", __func__, idx, st20_total_pkts);
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

static int rv_detector_init(struct mtl_main_impl* impl,
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
      case 1500:
        meta->fps = ST_FPS_P60;
        return;
      case 1501:
      case 1502:
        meta->fps = ST_FPS_P59_94;
        return;
      case 3000:
        meta->fps = ST_FPS_P30;
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

static bool inline rv_is_hdr_split(struct st_rx_video_session_impl* s) {
  return s->is_hdr_split;
}

static bool inline rv_is_dynamic_ext_frame(struct st_rx_video_session_impl* s) {
  return s->ops.query_ext_frame != NULL;
}

static struct st_frame_trans* rv_get_frame(struct st_rx_video_session_impl* s) {
  struct st_frame_trans* st20_frame;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];

    if (0 == rte_atomic32_read(&st20_frame->refcnt)) {
      dbg("%s(%d), find frame at %d\n", __func__, s->idx, i);
      rte_atomic32_inc(&st20_frame->refcnt);
      return st20_frame;
    }
  }

  dbg("%s(%d), no free frame\n", __func__, s->idx);
  return NULL;
}

static int rv_put_frame(struct st_rx_video_session_impl* s, void* frame) {
  int idx = s->idx;
  struct st_frame_trans* st20_frame;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];

    if (st20_frame->addr == frame) {
      dbg("%s(%d), put frame at %d\n", __func__, idx, i);
      rte_atomic32_dec(&st20_frame->refcnt);
      return 0;
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, idx, frame);
  return -EIO;
}

static int rv_free_frames(struct st_rx_video_session_impl* s) {
  if (s->st20_frames) {
    struct st_frame_trans* frame;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      st_frame_trans_uinit(frame);
    }
    mt_rte_free(s->st20_frames);
    s->st20_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rv_alloc_frames(struct mtl_main_impl* impl,
                           struct st_rx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx;
  size_t size = s->st20_uframe_size ? s->st20_uframe_size : s->st20_fb_size;
  struct st_frame_trans* st20_frame;
  void* frame;

  s->st20_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st20_frames) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames) {
    err("%s(%d), st20_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];
    rte_atomic32_set(&st20_frame->refcnt, 0);
    st20_frame->idx = i;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];

    if (s->ops.ext_frames) {
      frame = s->ops.ext_frames[i].buf_addr;
      if (!frame) {
        err("%s(%d), no external framebuffer\n", __func__, idx);
        rv_free_frames(s);
        return -EIO;
      }
      rte_iova_t frame_iova = s->ops.ext_frames[i].buf_iova;
      if (frame_iova == MTL_BAD_IOVA || frame_iova == 0) {
        err("%s(%d), external framebuffer not mapped to iova\n", __func__, idx);
        rv_free_frames(s);
        return -EIO;
      }
      st20_frame->addr = frame;
      st20_frame->iova = frame_iova;
      st20_frame->flags = ST_FT_FLAG_EXT;
      info("%s(%d), attach external frame %d, addr %p, iova %" PRIu64 "\n", __func__, idx,
           i, frame, frame_iova);
    } else if (rv_is_hdr_split(s) || rv_is_dynamic_ext_frame(s)) {
      st20_frame->iova = 0; /* detect later */
      st20_frame->addr = NULL;
      st20_frame->flags = 0;
    } else {
      frame = mt_rte_zmalloc_socket(size, soc_id);
      if (!frame) {
        err("%s(%d), frame malloc %" PRIu64 " fail for %d\n", __func__, idx, size, i);
        rv_free_frames(s);
        return -ENOMEM;
      }
      st20_frame->flags = ST_FT_FLAG_RTE_MALLOC;
      st20_frame->addr = frame;
      st20_frame->iova = rte_malloc_virt2iova(frame);
    }
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rv_free_rtps(struct st_rx_video_session_impl* s) {
  if (s->rtps_ring) {
    mt_ring_dequeue_clean(s->rtps_ring);
    rte_ring_free(s->rtps_ring);
    s->rtps_ring = NULL;
  }

  return 0;
}

static int rv_alloc_rtps(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                         struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-VIDEO-RTP-RING-M%d-R%d", mgr_idx, idx);
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
  s->rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rv_uinit_hdr_split(struct st_rx_video_session_impl* s) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->hdr_split_info[i].frames) {
      mt_rte_free(s->hdr_split_info[i].frames);
      s->hdr_split_info[i].frames = NULL;
    }
  }

  return 0;
}

static int rv_init_hdr_split(struct mtl_main_impl* impl,
                             struct st_rx_video_session_impl* s) {
  int num_port = s->ops.num_port;
  int idx = s->idx;
  size_t frame_size = s->st20_frame_size;

  enum mtl_port port;
  int soc_id;
  void* frames;
  uint32_t mbufs_per_frame;
  uint32_t mbufs_total;

  mbufs_per_frame = frame_size / ST_VIDEO_BPM_SIZE;
  if (frame_size % ST_VIDEO_BPM_SIZE) mbufs_per_frame++;
  mbufs_total = mbufs_per_frame * s->st20_frames_cnt;
  /* extra mbufs since frame may not start from zero pos */
  mbufs_total += (mbufs_per_frame - 1);

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    soc_id = mt_socket_id(impl, port);
    size_t frames_size = mbufs_total * ST_VIDEO_BPM_SIZE;
    /* more extra space since rte_mbuf_data_iova_default has offset */
    frames = mt_rte_zmalloc_socket(frames_size + 4096, soc_id);
    if (!frames) {
      err("%s(%d), frames malloc fail for %d, mbufs_total %u\n", __func__, idx, i,
          mbufs_total);
      rv_uinit_hdr_split(s);
      return -ENOMEM;
    }
    s->hdr_split_info[i].frames = frames;
    s->hdr_split_info[i].frames_size = frames_size;
    s->hdr_split_info[i].frames_iova = rte_malloc_virt2iova(frames);
    s->hdr_split_info[i].mbufs_per_frame = mbufs_per_frame;
    s->hdr_split_info[i].mbufs_total = mbufs_total;
    info("%s(%d,%d), frames (%p-%p), mbufs_total %u\n", __func__, idx, i, frames,
         frames + frames_size, mbufs_total);
  }

  return 0;
}

#ifdef ST_HAS_DPDK_HDR_SPLIT
/* run within the context of receiver lcore */
static int rv_hdrs_mbuf_callback_fn(void* priv, struct rte_eth_hdrs_mbuf* mbuf) {
  struct st_rx_video_session_impl* s = priv;
  struct st_rx_video_hdr_split_info* hdr_split = &s->hdr_split_info[MTL_SESSION_PORT_P];
  uint32_t alloc_idx = hdr_split->mbuf_alloc_idx;
  uint32_t cur_frame_mbuf_idx = hdr_split->cur_frame_mbuf_idx;

  if (cur_frame_mbuf_idx) {
    uint32_t next_frame_start_idx = cur_frame_mbuf_idx + hdr_split->mbufs_per_frame;

    if (alloc_idx == next_frame_start_idx) {
      /* start of next frame, reset if remainning frame space is capable one frame */
      if ((alloc_idx + hdr_split->mbufs_per_frame) >= hdr_split->mbufs_total) {
        dbg("%s(%d), idx reset at idx %u, cur_frame_mbuf_idx %u\n", __func__, s->idx,
            alloc_idx, cur_frame_mbuf_idx);
        /* notify for mismatch frame address */
        if (cur_frame_mbuf_idx % hdr_split->mbufs_per_frame) {
          dbg("%s(%d), idx reset for mismatch frame at idx %u, cur_frame_mbuf_idx %u\n",
              __func__, s->idx, alloc_idx, cur_frame_mbuf_idx);
        }
        alloc_idx = 0;
      }
    }
  } else { /* warm up mbuf stage */
    uint32_t remainning_mbufs = hdr_split->mbufs_total - alloc_idx;
    if (remainning_mbufs < hdr_split->mbufs_per_frame) {
      /* all mbuf ready, start from zero */
      info("%s(%d), alloc idx reset at %u as pool ready\n", __func__, s->idx, alloc_idx);
      alloc_idx = 0;
      hdr_split->mbuf_pool_ready = true;
    }
  }

  mbuf->buf_addr = hdr_split->frames + alloc_idx * ST_VIDEO_BPM_SIZE;
  mbuf->buf_iova = hdr_split->frames_iova + alloc_idx * ST_VIDEO_BPM_SIZE;
  dbg("%s(%d), mbuf alloc idx %u, buf %p\n", __func__, s->idx, alloc_idx, mbuf->buf_addr);

  /* point to next alloc_idx */
  alloc_idx++;
  if (alloc_idx >= hdr_split->mbufs_total) {
    /* only happen if cur_frame_mbuf_idx not get updated since imiss */
    err("%s(%d), alloc idx %u(%p) reset as it reach end %u\n", __func__, s->idx,
        alloc_idx, mbuf->buf_addr, hdr_split->mbufs_total);
    alloc_idx = 0;
    hdr_split->mbuf_pool_ready = true;
  }
  hdr_split->mbuf_alloc_idx = alloc_idx;

  return 0;
}
#endif

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

void rv_slot_dump(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    info("%s(%d), tmstamp %u recv_size %" PRIu64 " pkts_received %u\n", __func__, i,
         slot->tmstamp, rv_slot_get_frame_size(s, slot), slot->pkts_received);
  }
}

static int rv_init(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                   struct st_rx_video_session_impl* s, int idx) {
  s->idx = idx;
  s->parnet = mgr;
  return 0;
}

static int rv_uinit_slot(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    if (slot->frame_bitmap) {
      mt_rte_free(slot->frame_bitmap);
      slot->frame_bitmap = NULL;
    }
    if (slot->slice_info) {
      mt_rte_free(slot->slice_info);
      slot->slice_info = NULL;
    }
    if (slot->frame) {
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rv_init_slot(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
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
    frame_bitmap = mt_rte_zmalloc_socket(bitmap_size, soc_id);
    if (!frame_bitmap) {
      err("%s(%d), bitmap malloc %" PRIu64 " fail\n", __func__, idx, bitmap_size);
      return -ENOMEM;
    }
    slot->frame_bitmap = frame_bitmap;

    if (ST20_TYPE_SLICE_LEVEL == type) {
      slice_info = mt_rte_zmalloc_socket(sizeof(*slice_info), soc_id);
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

static void rv_frame_notify(struct st_rx_video_session_impl* s,
                            struct st_rx_video_slot_impl* slot) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_rx_frame_meta* meta = &slot->meta;

  dbg("%s(%d), start\n", __func__, s->idx);
  meta->width = ops->width;
  meta->height = ops->height;
  meta->fmt = ops->fmt;
  meta->fps = ops->fps;
  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->second_field = slot->second_field;
  meta->frame_total_size = s->st20_frame_size;
  meta->uframe_total_size = s->st20_uframe_size;
  meta->frame_recv_size = rv_slot_get_frame_size(s, slot);
  if (meta->frame_recv_size >= s->st20_frame_size) {
    meta->status = ST_FRAME_STATUS_COMPLETE;
    if (ops->num_port > 1) {
      dbg("%s(%d): pks redunant %u received %u\n", __func__, s->idx,
          slot->pkts_redunant_received, slot->pkts_received);
      if ((slot->pkts_redunant_received + 16) < slot->pkts_received)
        meta->status = ST_FRAME_STATUS_RECONSTRUCTED;
    }
    rte_atomic32_inc(&s->stat_frames_received);

    /* notify frame */
    int ret = -EIO;
    dbg("%s(%d): tmstamp %u\n", __func__, s->idx, slot->tmstamp);
    if (ops->notify_frame_ready)
      ret = ops->notify_frame_ready(ops->priv, slot->frame, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  } else {
    dbg("%s(%d): frame_recv_size %" PRIu64 ", frame_total_size %" PRIu64 ", tmstamp %u\n",
        __func__, s->idx, meta->frame_recv_size, meta->frame_total_size, slot->tmstamp);
    meta->status = ST_FRAME_STATUS_CORRUPTED;
    s->stat_frames_dropped++;
    rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
    /* notify the incomplete frame if user required */
    if (ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME) {
      ops->notify_frame_ready(ops->priv, slot->frame, meta);
    } else {
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  }
}

static void rv_st22_frame_notify(struct st_rx_video_session_impl* s,
                                 struct st_rx_video_slot_impl* slot,
                                 enum st_frame_status status) {
  struct st20_rx_ops* ops = &s->ops;
  struct st22_rx_frame_meta* meta = &slot->st22_meta;

  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->frame_total_size = rv_slot_get_frame_size(s, slot);
  meta->status = status;

  /* notify frame */
  int ret = -EIO;
  struct st22_rx_video_info* st22_info = s->st22_info;

  if (st_is_frame_complete(status)) {
    rte_atomic32_inc(&s->stat_frames_received);
    if (st22_info->notify_frame_ready)
      ret = st22_info->notify_frame_ready(ops->priv, slot->frame, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  } else {
    s->stat_frames_dropped++;
    rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
    /* notify the incomplete frame if user required */
    if (ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME) {
      st22_info->notify_frame_ready(ops->priv, slot->frame, meta);
    } else {
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  }
}

static void rv_slice_notify(struct st_rx_video_session_impl* s,
                            struct st_rx_video_slot_impl* slot,
                            struct st_rx_video_slot_slice_info* slice_info) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_rx_slice_meta* meta = &s->slice_meta;

  /* w, h, fps, fmt, etc are fixed info */
  meta->timestamp = slot->tmstamp;
  meta->second_field = slot->second_field;
  meta->frame_recv_size = rv_slot_get_frame_size(s, slot);
  meta->frame_recv_lines = slice_info->ready_slices * s->slice_lines;
  ops->notify_slice_ready(ops->priv, slot->frame, meta);
  s->stat_slices_received++;
}

static void rv_slice_add(struct st_rx_video_session_impl* s,
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

    s->stat_pkts_slice_fail++;
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
        s->stat_pkts_slice_merged++;
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
    rv_slice_notify(s, slot, slice_info);
  }
}

static struct st_rx_video_slot_impl* rv_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp, void* hdr_split_pd) {
  int i, slot_idx;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < s->slot_max; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) return slot;
  }

  dbg("%s(%d): new tmstamp %u\n", __func__, s->idx, tmstamp);
  if (s->dma_dev && !mt_dma_empty(s->dma_dev)) {
    /* still in progress of previous frame, drop current pkt */
    rte_atomic32_inc(&s->dma_previous_busy_cnt);
    dbg("%s(%d): still has dma inflight %u\n", __func__, s->idx,
        s->dma_dev->nb_borrowed[s->dma_lender]);
    return NULL;
  }

  slot_idx = (s->slot_idx + 1) % s->slot_max;
  slot = &s->slots[slot_idx];
  // rv_slot_dump(s);

  /* drop frame if any previous */
  if (slot->frame) {
    if (s->st22_info)
      rv_st22_frame_notify(s, slot, ST_FRAME_STATUS_CORRUPTED);
    else
      rv_frame_notify(s, slot);
    slot->frame = NULL;
  }

  rv_slot_init_frame_size(s, slot);
  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  s->slot_idx = slot_idx;

  struct st_frame_trans* frame_info = rv_get_frame(s);
  if (!frame_info) {
    s->stat_slot_get_frame_fail++;
    dbg("%s(%d): slot %d get frame fail\n", __func__, s->idx, slot_idx);
    return NULL;
  }
  if (hdr_split_pd) { /* reslove base addr */
    frame_info->addr = hdr_split_pd;
  }
  if (rv_is_dynamic_ext_frame(s)) {
    struct st20_ext_frame ext_frame;
    struct st20_rx_ops* ops = &s->ops;
    struct st20_rx_frame_meta* meta = &slot->meta;

    meta->width = ops->width;
    meta->height = ops->height;
    meta->fmt = ops->fmt;
    meta->fps = ops->fps;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = slot->tmstamp;
    meta->frame_total_size = s->st20_frame_size;
    meta->uframe_total_size = s->st20_uframe_size;
    if (s->ops.query_ext_frame(s->ops.priv, &ext_frame, meta) < 0) {
      s->stat_slot_query_ext_fail++;
      dbg("%s(%d): query ext frame fail\n", __func__, s->idx);
      rte_atomic32_dec(&frame_info->refcnt);
      return NULL;
    }
    frame_info->addr = ext_frame.buf_addr;
    frame_info->iova = ext_frame.buf_iova;
    frame_info->flags |= ST_FT_FLAG_EXT;
    meta->opaque = ext_frame.opaque;
  }
  slot->frame = frame_info->addr;
  slot->frame_iova = frame_info->iova;

  s->dma_slot = slot;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);
  if (slot->slice_info) memset(slot->slice_info, 0x0, sizeof(*slot->slice_info));

  rte_atomic32_inc(&s->cbs_frame_slot_cnt);

  dbg("%s(%d): assign slot %d frame %p for tmstamp %u\n", __func__, s->idx, slot_idx,
      slot->frame, tmstamp);
  return slot;
}

static struct st_rx_video_slot_impl* rv_rtp_slot_by_tmstamp(
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
  // rv_slot_dump(s);

  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  s->slot_idx = slot_idx;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);

  dbg("%s: assign slot %d for tmstamp %u\n", __func__, slot_idx, tmstamp);
  return slot;
}

static void rv_slot_full_frame(struct st_rx_video_session_impl* s,
                               struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rv_frame_notify(s, slot);
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  slot->frame = NULL; /* frame pass to app */
}

static void rv_st22_slot_full_frame(struct st_rx_video_session_impl* s,
                                    struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rv_st22_frame_notify(s, slot, ST_FRAME_STATUS_COMPLETE);
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
  slot->frame = NULL; /* frame pass to app */
}

static void rv_st22_slot_drop_frame(struct st_rx_video_session_impl* s,
                                    struct st_rx_video_slot_impl* slot) {
  rv_put_frame(s, slot->frame);
  slot->frame = NULL;
  s->stat_frames_dropped++;
  rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
  rv_slot_init_frame_size(s, slot);
  slot->pkts_received = 0;
  slot->pkts_redunant_received = 0;
}

static int rv_free_dma(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  if (s->dma_dev) {
    mt_dma_free_dev(impl, s->dma_dev);
    s->dma_dev = NULL;
  }

  return 0;
}

static int rv_slice_dma_drop_mbuf(void* priv, struct rte_mbuf* mbuf) {
  struct st_rx_video_session_impl* s = priv;
  rv_slice_add(s, s->dma_slot, st_rx_mbuf_get_offset(mbuf), st_rx_mbuf_get_len(mbuf));
  return 0;
}

static int rv_init_dma(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int idx = s->idx;
  bool share_dma = true;
  enum st20_type type = s->ops.type;

  struct mt_dma_request_req req;
  req.nb_desc = s->dma_nb_desc;
  req.max_shared = share_dma ? MT_DMA_MAX_SESSIONS : 1;
  req.sch_idx = s->parnet->idx;
  req.socket_id = mt_socket_id(impl, port);
  req.priv = s;
  if (type == ST20_TYPE_SLICE_LEVEL)
    req.drop_mbuf_cb = rv_slice_dma_drop_mbuf;
  else
    req.drop_mbuf_cb = NULL;
  struct mtl_dma_lender_dev* dma_dev = mt_dma_request_dev(impl, &req);
  if (!dma_dev) {
    info("%s(%d), fail, can not request dma dev\n", __func__, idx);
    return -EIO;
  }

  s->dma_dev = dma_dev;

  info("%s(%d), succ, dma %d lender id %u\n", __func__, idx, mt_dma_dev_id(dma_dev),
       mt_dma_lender_id(dma_dev));
  return 0;
}

#ifdef ST_PCAPNG_ENABLED
static int rv_start_pcapng(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                           uint32_t max_dump_packets, bool sync,
                           struct st_pcap_dump_meta* meta) {
  if (s->pcapng != NULL) {
    err("%s, pcapng dump already started\n", __func__);
    return -EIO;
  }

  enum mtl_port port = s->port_maps[MTL_SESSION_PORT_P];
  int idx = s->idx;
  int pkt_len = ST_PKT_MAX_ETHER_BYTES;

  char file_name[MTL_PCAP_FILE_MAX_LEN];
  if (s->st22_info) {
    snprintf(file_name, MTL_PCAP_FILE_MAX_LEN, "st22_rx_%d_%u_XXXXXX.pcapng", idx,
             max_dump_packets);
  } else {
    snprintf(file_name, MTL_PCAP_FILE_MAX_LEN, "st20_rx_%d_%u_XXXXXX.pcapng", idx,
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

  struct rte_mempool* mp =
      mt_mempool_create_by_ops(impl, port, "pcapng_test_pool", 256, MT_MBUF_CACHE_SIZE, 0,
                               rte_pcapng_mbuf_size(pkt_len), "ring_mp_sc");
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
      mt_sleep_ms(100);
    }
    if (i >= time_out) {
      err("%s(%d), pcapng(%s,%u) dump timeout\n", __func__, idx, file_name,
          max_dump_packets);
      return -EIO;
    }
    if (meta) {
      meta->dumped_packets = s->pcapng_dumped_pkts;
      snprintf(meta->file_name, MTL_PCAP_FILE_MAX_LEN, "%s", file_name);
    }
    info("%s(%d), pcapng(%s,%u) dump finish\n", __func__, idx, file_name,
         max_dump_packets);
  }

  return 0;
}

static int rv_stop_pcapng(struct st_rx_video_session_impl* s) {
  s->pcapng_dropped_pkts = 0;
  s->pcapng_max_pkts = 0;

  if (s->pcapng) {
    rte_pcapng_close(s->pcapng);
    s->pcapng = NULL;
  }

  if (s->pcapng_pool) {
    mt_mempool_free(s->pcapng_pool);
    s->pcapng_pool = NULL;
  }
  return 0;
}

static int rv_dump_pcapng(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                          struct rte_mbuf** mbuf, uint16_t rv, int s_port) {
  struct rte_mbuf* pcapng_mbuf[rv];
  int pcapng_mbuf_cnt = 0;
  ssize_t len;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);
  uint16_t queue_id = mt_dev_rx_queue_id(s->queue[s_port]);

  for (uint16_t i = 0; i < rv; i++) {
    struct rte_mbuf* mc;
    uint64_t timestamp_cycle, timestamp_ns;
    if (mt_has_ebu(impl) && inf->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
      timestamp_cycle = 0;
      timestamp_ns = mt_mbuf_hw_time_stamp(impl, mbuf[i], port);
    } else {
      timestamp_cycle = rte_get_tsc_cycles();
      timestamp_ns = 0;
    }
    mc = rte_pcapng_copy(s->port_id[s_port], queue_id, mbuf[i], s->pcapng_pool,
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
static int rv_start_pcapng(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                           uint32_t max_dump_packets, bool sync,
                           struct st_pcap_dump_meta* meta) {
  return -EINVAL;
}
#endif

static int rv_dma_dequeue(struct mtl_main_impl* impl,
                          struct st_rx_video_session_impl* s) {
  struct mtl_dma_lender_dev* dma_dev = s->dma_dev;

  uint16_t nb_dq = mt_dma_completed(dma_dev, ST_RX_VIDEO_BURTS_SIZE, NULL, NULL);

  if (nb_dq) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    mt_dma_drop_mbuf(dma_dev, nb_dq);
  }

  /* all dma action finished */
  struct st_rx_video_slot_impl* dma_slot = s->dma_slot;
  if (mt_dma_empty(dma_dev) && dma_slot) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    int32_t frame_recv_size = rv_slot_get_frame_size(s, dma_slot);
    if (frame_recv_size >= s->st20_frame_size) {
      dbg("%s(%d): full frame\n", __func__, s->idx);
      rv_slot_full_frame(s, dma_slot);
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

static int rv_handle_frame_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                               enum mtl_session_port s_port, bool ctrl_thread) {
  struct st20_rx_ops* ops = &s->ops;
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
  struct rte_mbuf* mbuf_next = mbuf->next;

  if (payload_type != ops->payload_type) {
    s->stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }
  if (mbuf_next && mbuf_next->data_len) {
    /* for some reason mbuf splits into 2 segments (1024 bytes + left bytes) */
    /* todo: payload needs to be copied from 2 places */
    s->stat_pkts_multi_segments_received++;
    return -EIO;
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, NULL);
  if (!slot || !slot->frame) {
    s->stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  slot->second_field = (line1_number & ST20_SECOND_FIELD) ? true : false;
  line1_number &= ~ST20_SECOND_FIELD;

  /* check if the same pkt got already */
  if (slot->seq_id_got) {
    if (seq_id_u32 >= slot->seq_id_base_u32)
      pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
    else
      pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base_u32);
      s->stat_pkts_idx_oo_bitmap++;
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      info("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
           pkt_idx);
      s->stat_pkts_redunant_dropped++;
      slot->pkts_redunant_received++;
      return -EIO;
    }
  } else {
    /* the first pkt should always dispatch to control thread */
    if (!line1_number && !line1_offset && ctrl_thread) { /* first packet */
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id_u32,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id not got, %u %u\n", __func__, s->idx,
          s_port, seq_id_u32, line1_number, line1_offset);
      s->stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* caculate offset */
  uint32_t offset;
  offset = line1_number * s->st20_linesize +
           line1_offset / s->st20_pg.coverage * s->st20_pg.size;
  size_t payload_length = line1_length;
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  if ((offset + payload_length) >
      s->st20_fb_size + s->st20_bytes_in_line - s->st20_linesize) {
    dbg("%s(%d,%d): invalid offset %u frame buffer size %" PRIu64 "\n", __func__, s->idx,
        s_port, offset, s->st20_fb_size);
    dbg("%s, number %u offset %u len %u\n", __func__, line1_number, line1_offset,
        line1_length);
    s->stat_pkts_offset_dropped++;
    return -EIO;
  }

  bool need_copy = true;
  struct mtl_dma_lender_dev* dma_dev = s->dma_dev;
  struct mtl_main_impl* impl = rv_get_impl(s);
  bool ebu = mt_has_ebu(impl);
  if (ebu) {
    /* no copy for ebu */
    need_copy = false;
    enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
    struct mt_interface* inf = mt_if(impl, port);
    if (inf->feature & MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP) {
      rv_ebu_on_packet(s, tmstamp, mt_mbuf_hw_time_stamp(impl, mbuf, port), pkt_idx);
    }
  }
  if (s->st20_uframe_size) {
    /* user frame mode, pass to app to handle the payload */
    struct st20_rx_uframe_pg_meta* pg_meta = &s->pg_meta;
    pg_meta->payload = payload;
    pg_meta->row_length = line1_length;
    pg_meta->row_number = line1_number;
    pg_meta->row_offset = line1_offset;
    pg_meta->pg_cnt = line1_length / s->st20_pg.size;
    pg_meta->timestamp = tmstamp;
    ops->uframe_pg_callback(ops->priv, slot->frame, pg_meta);
    if (extra_rtp) {
      pg_meta->payload = payload + line1_length;
      pg_meta->row_length = ntohs(extra_rtp->row_length);
      pg_meta->row_number = ntohs(extra_rtp->row_number);
      pg_meta->row_offset = ntohs(extra_rtp->row_offset);
      pg_meta->pg_cnt = pg_meta->row_length / s->st20_pg.size;
      ops->uframe_pg_callback(ops->priv, slot->frame, pg_meta);
    }
  } else if (need_copy) {
    /* copy the payload to target frame by dma or cpu */
    if (extra_rtp && s->st20_linesize > s->st20_bytes_in_line) {
      /* packet acrosses line padding, copy two lines data */
      rte_memcpy(slot->frame + offset, payload, line1_length);
      rte_memcpy(slot->frame + (line1_number + 1) * s->st20_linesize,
                 payload + line1_length, payload_length - line1_length);
    } else if (dma_dev && (payload_length > ST_RX_VIDEO_DMA_MIN_SIZE) &&
               !mt_dma_full(dma_dev)) {
      rte_iova_t payload_iova =
          rte_pktmbuf_iova_offset(mbuf, sizeof(struct st_rfc4175_video_hdr));
      if (extra_rtp) payload_iova += sizeof(*extra_rtp);
      ret = mt_dma_copy(dma_dev, slot->frame_iova + offset, payload_iova, payload_length);
      if (ret < 0) {
        /* use cpu copy if dma copy fail */
        rte_memcpy(slot->frame + offset, payload, payload_length);
      } else {
        /* abstrct dma dev takes ownership of this mbuf */
        st_rx_mbuf_set_offset(mbuf, offset);
        st_rx_mbuf_set_len(mbuf, payload_length);
        ret = mt_dma_borrow_mbuf(dma_dev, mbuf);
        if (ret) { /* never happen in real life */
          err("%s(%d,%d), mbuf copied but not enqueued \n", __func__, s->idx, s_port);
          /* mbuf freed and dma copy will operate an invalid src! */
          rte_pktmbuf_free(mbuf);
        }
        s->dma_copy = true;
        s->stat_pkts_dma++;
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
  s->stat_pkts_received++;
  slot->pkts_received++;

  /* slice */
  if (slot->slice_info && !s->dma_copy) { /* ST20_TYPE_SLICE_LEVEL */
    rv_slice_add(s, slot, offset, payload_length);
  }

  /* check if frame is full */
  size_t frame_recv_size = rv_slot_get_frame_size(s, slot);
  bool end_frame = false;
  if (dma_dev) {
    if (frame_recv_size >= s->st20_frame_size && mt_dma_empty(dma_dev)) end_frame = true;
  } else {
    if (frame_recv_size >= s->st20_frame_size) end_frame = true;
  }
  if (end_frame) {
    dbg("%s(%d,%d): full frame on %p(%" PRIu64 ")\n", __func__, s->idx, s_port,
        slot->frame, frame_recv_size);
    dbg("%s(%d,%d): tmstamp %u slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    /* end of frame */
    rv_slot_full_frame(s, slot);
  }

  return 0;
}

static int rv_handle_rtp_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                             enum mtl_session_port s_port, bool ctrl_thread) {
  struct st20_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t seq_id_u32 = rfc4175_rtp_seq_id((struct st20_rfc4175_rtp_hdr*)rtp);
  uint8_t payload_type = rtp->payload_type;
  int pkt_idx = -1;

  if (payload_type != ops->payload_type) {
    dbg("%s, payload_type mismatch %d %d\n", __func__, payload_type, ops->payload_type);
    s->stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rv_rtp_slot_by_tmstamp(s, tmstamp);
  if (!slot) {
    s->stat_pkts_no_slot++;
    return -ENOMEM;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (s->st22_handle) {
      if (seq_id >= slot->seq_id_base)
        pkt_idx = seq_id - slot->seq_id_base;
      else
        pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;
    } else {
      if (seq_id_u32 >= slot->seq_id_base_u32)
        pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
      else
        pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    }

    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base);
      s->stat_pkts_idx_oo_bitmap++;
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, idx, s_port, pkt_idx);
      s->stat_pkts_redunant_dropped++;
      return -EIO;
    }
  } else {
    if (!slot->seq_id_got) { /* first packet */
      slot->seq_id_base = seq_id;
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      rte_atomic32_inc(&s->stat_frames_received);
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, s->idx,
          s_port, seq_id, slot->seq_id_base);
      s->stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* enqueue the packet ring to app */
  int ret = rte_ring_sp_enqueue(s->rtps_ring, (void*)mbuf);
  if (ret < 0) {
    dbg("%s(%d,%d), drop as rtps ring full, pkt_idx %d base %u\n", __func__, idx, s_port,
        pkt_idx, slot->seq_id_base);
    s->stat_pkts_rtp_ring_full++;
    return -EIO;
  }

  ops->notify_rtp_ready(ops->priv);
  s->stat_pkts_received++;

  return 0;
}

struct st22_box {
  uint32_t lbox; /* box lenght */
  char tbox[4];
};

/* Video Support Box and Color Specification Box */
static int rv_parse_st22_boxes(struct st_rx_video_session_impl* s, void* boxes,
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
    info("0x%02x ", buf[i]);
  }
  info("end\n");
#endif
  return 0;
}

static int rv_handle_st22_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                              enum mtl_session_port s_port, bool ctrl_thread) {
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
    s->stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  if (rtp->kmode) {
    s->stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, NULL);
  if (!slot) {
    s->stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  dbg("%s(%d,%d), seq_id %d kmode %u trans_order %u\n", __func__, s->idx, s_port, seq_id,
      rtp->kmode, rtp->trans_order);
  dbg("%s(%d,%d), seq_id %d p_counter %u sep_counter %u\n", __func__, s->idx, s_port,
      seq_id, p_counter, sep_counter);

  if (slot->seq_id_got) {
    if (!rtp->base.marker && (payload_length != slot->st22_payload_length)) {
      s->stat_pkts_wrong_hdr_dropped++;
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
      s->stat_pkts_idx_oo_bitmap++;
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      s->stat_pkts_redunant_dropped++;
      slot->pkts_redunant_received++;
      return -EIO;
    }
  } else {
    /* first packet */
    if (!pkt_counter) { /* first packet */
      ret = rv_parse_st22_boxes(s, payload, slot);
      if (ret < 0) {
        s->stat_pkts_idx_dropped++;
        return -EIO;
      }
      slot->seq_id_base = seq_id;
      slot->st22_payload_length = payload_length;
      slot->seq_id_got = true;
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), get seq_id %d tmstamp %u, p_counter %u sep_counter %u, "
          "payload_length %u\n",
          __func__, s->idx, s_port, seq_id, tmstamp, p_counter, sep_counter,
          payload_length);
    } else {
      dbg("%s(%d,%d), drop seq_id %d tmstamp %u as base seq not got, p_counter %u "
          "sep_counter %u\n",
          __func__, s->idx, s_port, seq_id, tmstamp, p_counter, sep_counter);
      s->stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  if (!slot->frame) {
    dbg("%s(%d,%d): slot frame not inited\n", __func__, s->idx, s_port);
    s->stat_pkts_no_slot++;
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
    s->stat_pkts_offset_dropped++;
    return -EIO;
  }
  rte_memcpy(slot->frame + offset, payload, payload_length);
  rv_slot_add_frame_size(s, slot, payload_length);
  s->stat_pkts_received++;
  slot->pkts_received++;

  /* check if frame is full */
  if (rtp->base.marker) {
    size_t expect_frame_size = offset + payload_length;
    size_t rece_frame_size = rv_slot_get_frame_size(s, slot);

    dbg("%s(%d,%d): marker get, frame size %" PRIu64 " %" PRIu64 ", tmstamp %u\n",
        __func__, s->idx, s_port, rece_frame_size, expect_frame_size, tmstamp);
    if (expect_frame_size == rece_frame_size) {
      rv_st22_slot_full_frame(s, slot);
    } else {
      rv_st22_slot_drop_frame(s, slot);
    }
  }

  return 0;
}

static int rv_handle_hdr_split_pkt(struct st_rx_video_session_impl* s,
                                   struct rte_mbuf* mbuf, enum mtl_session_port s_port,
                                   bool ctrl_thread) {
  struct st20_rx_ops* ops = &s->ops;
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
  int pkt_idx = -1;
  struct st_rx_video_hdr_split_info* hdr_split = &s->hdr_split_info[s_port];
  struct rte_mbuf* mbuf_next = mbuf->next;

  if (payload_type != ops->payload_type) {
    s->stat_pkts_wrong_hdr_dropped++;
    return -EINVAL;
  }
  if (!hdr_split->mbuf_pool_ready) {
    s->stat_pkts_no_slot++;
    return -EINVAL;
  }

  if (mbuf_next && mbuf_next->data_len) {
    payload = rte_pktmbuf_mtod(mbuf_next, void*);
  }

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, payload);
  if (!slot || !slot->frame) {
    s->stat_pkts_no_slot++;
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  slot->second_field = (line1_number & ST20_SECOND_FIELD) ? true : false;
  line1_number &= ~ST20_SECOND_FIELD;

  /* check if the same pkt got already */
  if (slot->seq_id_got) {
    if (seq_id_u32 >= slot->seq_id_base_u32)
      pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
    else
      pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base_u32);
      s->stat_pkts_idx_oo_bitmap++;
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      s->stat_pkts_redunant_dropped++;
      slot->pkts_redunant_received++;
      return -EIO;
    }
  } else {
    if (!line1_number && !line1_offset) { /* first packet */
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id_u32,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id not got, %u %u\n", __func__, s->idx,
          s_port, seq_id_u32, line1_number, line1_offset);
      s->stat_pkts_idx_dropped++;
      return -EIO;
    }
  }

  /* caculate offset */
  uint32_t offset =
      (line1_number * ops->width + line1_offset) / s->st20_pg.coverage * s->st20_pg.size;
  size_t payload_length = line1_length;
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  if ((offset + payload_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    dbg("%s, number %u offset %u len %u\n", __func__, line1_number, line1_offset,
        line1_length);
    s->stat_pkts_offset_dropped++;
    return -EIO;
  }

  uint8_t marker = rtp->base.marker;
  if ((payload_length != ST_VIDEO_BPM_SIZE) && !marker) {
    s->stat_pkts_not_bpm++;
    return -EIO;
  }

  bool need_copy = false;

  if (!pkt_idx) {
    hdr_split->cur_frame_addr = payload;
    /* Cut RTE_PKTMBUF_HEADROOM since rte_mbuf_data_iova_default has offset */
    hdr_split->cur_frame_mbuf_idx =
        (payload - RTE_PKTMBUF_HEADROOM - hdr_split->frames) / ST_VIDEO_BPM_SIZE;
    dbg("%s(%d,%d), cur_frame_addr %p cur_frame_idx %u\n", __func__, s->idx, s_port,
        hdr_split->cur_frame_addr, slot->cur_frame_mbuf_idx);
    if (hdr_split->cur_frame_mbuf_idx % hdr_split->mbufs_per_frame) {
      s->stat_mismatch_hdr_split_frame++;
      dbg("%s(%d,%d), cur_frame_addr %p cur_frame_idx %u mbufs_per_frame %u\n", __func__,
          s->idx, s_port, hdr_split->cur_frame_addr, hdr_split->cur_frame_mbuf_idx,
          hdr_split->mbufs_per_frame);
    }
  } else {
    void* expect_payload = hdr_split->cur_frame_addr + pkt_idx * ST_VIDEO_BPM_SIZE;
    if (expect_payload != payload) {
      dbg("%s(%d,%d), payload mismatch %p:%p on pkt %d\n", __func__, s->idx, s_port,
          payload, expect_payload, pkt_idx);
      /* may caused by ooo, imiss, the last pkt(ddp not split for unknow cause) */
      if (marker && (expect_payload < (hdr_split->frames + hdr_split->frames_size))) {
        need_copy = true;
        s->stat_pkts_copy_hdr_split++;
      } else { /* no way to recover since nic is in writing dram */
        s->stat_pkts_wrong_payload_hdr_split++;
        return -EIO;
      }
    }
  }

  if (need_copy) {
    rte_memcpy(slot->frame + offset, payload, payload_length);
  }

  rv_slot_add_frame_size(s, slot, payload_length);
  s->stat_pkts_received++;
  slot->pkts_received++;

  /* slice */
  if (slot->slice_info) {
    rv_slice_add(s, slot, offset, payload_length);
  }

  /* check if frame is full */
  size_t frame_recv_size = rv_slot_get_frame_size(s, slot);
  if (frame_recv_size >= s->st20_frame_size) {
    dbg("%s(%d,%d): full frame on %p(%d)\n", __func__, s->idx, s_port, slot->frame,
        frame_recv_size);
    dbg("%s(%d,%d): tmstamp %u slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    rv_slot_full_frame(s, slot);
  }

  return 0;
}

static int rv_uinit_pkt_lcore(struct mtl_main_impl* impl,
                              struct st_rx_video_session_impl* s) {
  int idx = s->idx;

  if (rte_atomic32_read(&s->pkt_lcore_active)) {
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    info("%s(%d), stop lcore\n", __func__, idx);
    while (rte_atomic32_read(&s->pkt_lcore_stopped) == 0) {
      mt_sleep_ms(10);
    }
  }

  if (s->has_pkt_lcore) {
    rte_eal_wait_lcore(s->pkt_lcore);
    mt_dev_put_lcore(impl, s->pkt_lcore);
    s->has_pkt_lcore = false;
  }

  if (s->pkt_lcore_ring) {
    mt_ring_dequeue_clean(s->pkt_lcore_ring);
    rte_ring_free(s->pkt_lcore_ring);
    s->pkt_lcore_ring = NULL;
  }

  return 0;
}

static int rv_pkt_lcore_func(void* args) {
  struct st_rx_video_session_impl* s = args;
  int idx = s->idx, ret;
  struct rte_mbuf* pkt = NULL;

  info("%s(%d), start\n", __func__, idx);
  while (rte_atomic32_read(&s->pkt_lcore_active)) {
    ret = rte_ring_sc_dequeue(s->pkt_lcore_ring, (void**)&pkt);
    if (ret >= 0) {
      rv_handle_frame_pkt(s, pkt, MTL_SESSION_PORT_P, true);
      rte_pktmbuf_free(pkt);
    }
  }

  rte_atomic32_set(&s->pkt_lcore_stopped, 1);
  info("%s(%d), end\n", __func__, idx);
  return 0;
}

static int rv_init_pkt_lcore(struct mtl_main_impl* impl,
                             struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count, lcore;
  int mgr_idx = mgr->idx, idx = s->idx, ret;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  snprintf(ring_name, 32, "RX-VIDEO-PKT-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = ST_RX_VIDEO_BURTS_SIZE * 4;
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), ring create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->pkt_lcore_ring = ring;

  ret = mt_dev_get_lcore(impl, &lcore);
  if (ret < 0) {
    err("%s(%d,%d), get lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rv_uinit_pkt_lcore(impl, s);
    return ret;
  }
  s->pkt_lcore = lcore;
  s->has_pkt_lcore = true;

  rte_atomic32_set(&s->pkt_lcore_active, 1);
  ret = rte_eal_remote_launch(rv_pkt_lcore_func, s, lcore);
  if (ret < 0) {
    err("%s(%d,%d), launch lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    rv_uinit_pkt_lcore(impl, s);
    return ret;
  }

  return 0;
}

static int rv_init_st22(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                        struct st22_rx_ops* st22_frame_ops) {
  struct st22_rx_video_info* st22_info;

  st22_info = mt_rte_zmalloc_socket(sizeof(*st22_info), mt_socket_id(impl, MTL_PORT_P));
  if (!st22_info) return -ENOMEM;

  st22_info->notify_frame_ready = st22_frame_ops->notify_frame_ready;

  st22_info->meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;

  s->st22_info = st22_info;

  return 0;
}

static int rv_uinit_st22(struct st_rx_video_session_impl* s) {
  if (s->st22_info) {
    mt_rte_free(s->st22_info);
    s->st22_info = NULL;
  }

  return 0;
}

static int rv_uinit_sw(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  rv_uinit_pkt_lcore(impl, s);
  rv_free_dma(impl, s);
  rv_uinit_slot(s);
  rv_free_frames(s);
  rv_free_rtps(s);
  rv_uinit_st22(s);
  rv_uinit_hdr_split(s);
  return 0;
}

static int rv_init_sw(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                      struct st_rx_video_session_impl* s, struct st22_rx_ops* st22_ops) {
  struct st20_rx_ops* ops = &s->ops;
  enum st20_type type = ops->type;
  int idx = s->idx;
  int ret;

  if (rv_is_hdr_split(s)) {
    ret = rv_init_hdr_split(impl, s);
    if (ret < 0) {
      rv_uinit_sw(impl, s);
      return ret;
    }
  }

  if (st22_ops) {
    ret = rv_init_st22(impl, s, st22_ops);
    if (ret < 0) {
      err("%s(%d), st22 frame init fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  if (st20_is_frame_type(type)) {
    ret = rv_alloc_frames(impl, s);
  } else if (type == ST20_TYPE_RTP_LEVEL) {
    ret = rv_alloc_rtps(impl, mgr, s);
  } else {
    err("%s(%d), error type %d\n", __func__, idx, type);
    return -EIO;
  }
  if (ret < 0) {
    rv_uinit_sw(impl, s);
    return ret;
  }

  ret = rv_init_slot(impl, s);
  if (ret < 0) {
    rv_uinit_sw(impl, s);
    return ret;
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    struct st20_rx_slice_meta* slice_meta = &s->slice_meta;
    slice_meta->width = ops->width;
    slice_meta->height = ops->height;
    slice_meta->fmt = ops->fmt;
    slice_meta->fps = ops->fps;
    slice_meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    slice_meta->frame_total_size = s->st20_frame_size;
    slice_meta->uframe_total_size = s->st20_uframe_size;
    slice_meta->second_field = false;
    info("%s(%d), slice lines %u\n", __func__, idx, s->slice_lines);
  }

  if (s->st20_uframe_size) {
    /* user frame mode */
    struct st20_rx_uframe_pg_meta* pg_meta = &s->pg_meta;
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
      !s->st20_uframe_size && !rv_is_hdr_split(s)) {
    rv_init_dma(impl, s);
  }

  s->has_pkt_lcore = false;
  rte_atomic32_set(&s->pkt_lcore_stopped, 0);
  rte_atomic32_set(&s->pkt_lcore_active, 0);

  uint64_t bps;
  bool pkt_handle_lcore = false;
  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s(%d), get bps fail %d\n", __func__, idx, ret);
    rv_uinit_sw(impl, s);
    return ret;
  }
  if (st20_is_frame_type(type)) {
    /* for trafic > 40g, two lcore used  */
    if ((bps / (1000 * 1000)) > (40 * 1000)) {
      if (!s->dma_dev) pkt_handle_lcore = true;
    }
  }

  /* only one core for hdr split mode */
  if (rv_is_hdr_split(s)) pkt_handle_lcore = false;

  if (pkt_handle_lcore) {
    if (type == ST20_TYPE_SLICE_LEVEL) {
      err("%s(%d), additional pkt lcore not support slice type\n", __func__, idx);
      rv_uinit_sw(impl, s);
      return -EINVAL;
    }
    ret = rv_init_pkt_lcore(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), init_pkt_lcore fail %d\n", __func__, idx, ret);
      rv_uinit_sw(impl, s);
      return ret;
    }
    /* enable multi slot as it has two threads running */
    s->slot_max = ST_VIDEO_RX_REC_NUM_OFO;
  }

  if (mt_has_ebu(impl)) {
    rv_ebu_init(impl, s);
  }

  /* init vsync */
  struct st_fps_timing fps_tm;
  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    rv_uinit_sw(impl, s);
    return ret;
  }
  s->vsync.meta.frame_time = (double)1000000000.0 * fps_tm.den / fps_tm.mul;
  st_vsync_calculate(impl, &s->vsync);
  s->vsync.init = true;
  /* init advice sleep us */
  int estimated_total_pkts = s->st20_frame_size / ST_VIDEO_BPM_SIZE;
  double trs = s->vsync.meta.frame_time / estimated_total_pkts;
  double sleep_ns = trs * 128;
  s->advice_sleep_us = sleep_ns / NS_PER_US;
  if (mt_tasklet_has_sleep(impl)) {
    info("%s(%d), advice sleep us %" PRIu64 ", trs %fns, total pkts %d\n", __func__, idx,
         s->advice_sleep_us, trs, estimated_total_pkts);
  }

  return 0;
}

static int rv_handle_detect_err(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                                enum mtl_session_port s_port, bool ctrl_thread) {
  err_once("%s(%d,%d), detect fail, please choose the rigth format\n", __func__, s->idx,
           s_port);
  return 0;
}

static int rv_detect_change_status(struct st_rx_video_session_impl* s,
                                   enum st20_detect_status new_status) {
  if (s->detector.status == new_status) return 0;

  s->detector.status = new_status;
  rv_init_pkt_handler(s);
  return 0;
}

static int rv_handle_detect_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                                enum mtl_session_port s_port, bool ctrl_thread) {
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
    s->stat_pkts_wrong_hdr_dropped++;
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
      detector->pkt_num[detector->frame_num] = s->stat_pkts_received;
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
        rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
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
            rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
            return ret;
          }
          s->slice_lines = reply.slice_lines;
          s->st20_uframe_size = reply.uframe_size;
          info("%s(%d), detected, slice_lines %u, uframe_size %" PRIu64 "\n", __func__,
               s->idx, s->slice_lines, s->st20_uframe_size);
        }
        if (!s->slice_lines) s->slice_lines = ops->height / 32;
        s->slice_size =
            ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
        s->st20_frames_cnt = ops->framebuff_cnt;
        s->st20_frame_size =
            ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
        if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
        s->st20_bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
        s->st20_linesize = s->st20_bytes_in_line;
        if (ops->linesize > s->st20_linesize)
          s->st20_linesize = ops->linesize;
        else if (ops->linesize) {
          err("%s(%d), invalid linesize %u\n", __func__, s->idx, ops->linesize);
          return -EINVAL;
        }
        s->st20_fb_size = s->st20_linesize * ops->height;
        if (ops->interlaced) s->st20_fb_size = s->st20_fb_size >> 1;
        /* at least 1000 byte for each packet */
        s->st20_frame_bitmap_size = s->st20_frame_size / 1000 / 8;
        /* one line at line 2 packets for all the format */
        if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
          s->st20_frame_bitmap_size = ops->height * 2 / 8;
        ret = rv_init_sw(rv_get_impl(s), s->parnet, s, NULL);
        if (ret < 0) {
          err("%s(%d), rv_init_sw fail %d\n", __func__, s->idx, ret);
          rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
          return ret;
        }
        rvs_mgr_update(s->parnet); /* update mgr since we has new advice sleep us */
        rv_detect_change_status(s, ST20_DETECT_STAT_SUCCESS);
        info("st20 detected(%d,%d): width: %d, height: %d, fps: %f\n", s->idx, s_port,
             meta->width, meta->height, st_frame_rate(meta->fps));
        info("st20 detected(%d,%d): packing: %d, field: %s, pkts per %s: %d\n", s->idx,
             s_port, meta->packing, meta->interlaced ? "interlaced" : "progressive",
             meta->interlaced ? "field" : "frame", detector->pkt_per_frame);
      }
    }
  }

  s->stat_pkts_received++;
  return 0;
}

static int rv_handle_mbuf(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_video_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_port s_port = s_priv->port;

  struct rte_ring* pkt_ring = s->pkt_lcore_ring;
  bool ctl_thread = pkt_ring ? false : true;
  int ret = 0;

#ifdef ST_PCAPNG_ENABLED /* dump mbufs to pcapng file */
  if ((s->pcapng != NULL) && (s->pcapng_max_pkts)) {
    if (s->pcapng_dumped_pkts < s->pcapng_max_pkts) {
      rv_dump_pcapng(impl, s, mbuf,
                     RTE_MIN(nb, s->pcapng_max_pkts - s->pcapng_dumped_pkts), s_port);
    } else { /* got enough packets, stop dumping */
      info("%s(%d,%d), pcapng dump finished, dumped %u packets, dropped %u pcakets\n",
           __func__, s->idx, s_port, s->pcapng_dumped_pkts, s->pcapng_dropped_pkts);
      rv_stop_pcapng(s);
    }
  }
#endif

  if (pkt_ring) {
    /* first pass to the pkt ring if it has pkt handling lcore */
    unsigned int n =
        rte_ring_sp_enqueue_bulk(s->pkt_lcore_ring, (void**)&mbuf[0], nb, NULL);
    nb -= n; /* n is zero or rx */
    s->stat_pkts_enqueue_fallback += nb;
  }
  if (!nb) return 0;

  s->pri_nic_inflight_cnt++;

  /* now dispatch the pkts to handler */
  for (uint16_t i = 0; i < nb; i++) {
    ret += s->pkt_handler(s, mbuf[i], s_port, ctl_thread);
  }
  return ret;
}

static int rv_tasklet(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                      struct st_rx_video_sessions_mgr* mgr) {
  struct rte_mbuf* mbuf[ST_RX_VIDEO_BURTS_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;

  bool done = true;

  if (s->dma_dev) {
    rv_dma_dequeue(impl, s);
    /* check if has pending pkts in dma */
    if (!mt_dma_empty(s->dma_dev)) done = false;
  }
  s->dma_copy = false;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (s->rss[s_port]) {
      rv = mt_rss_burst(s->rss[s_port], ST_RX_VIDEO_BURTS_SIZE);
    } else if (s->queue[s_port]) {
      rv = mt_dev_rx_burst(s->queue[s_port], &mbuf[0], ST_RX_VIDEO_BURTS_SIZE);
      if (rv) {
        rv_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
        rte_pktmbuf_free_bulk(&mbuf[0], rv);
      }
    } else {
      continue;
    }

    s->pri_nic_burst_cnt++;
    if (s->pri_nic_burst_cnt > ST_VIDEO_STAT_UPDATE_INTERVAL) {
      rte_atomic32_add(&s->nic_burst_cnt, s->pri_nic_burst_cnt);
      s->pri_nic_burst_cnt = 0;
      rte_atomic32_add(&s->nic_inflight_cnt, s->pri_nic_inflight_cnt);
      s->pri_nic_inflight_cnt = 0;
    }

    if (rv) done = false;
  }

  /* submit if any */
  if (s->dma_copy && s->dma_dev) mt_dma_submit(s->dma_dev);

  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static int rv_uinit_hw(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->queue[i]) {
      mt_dev_put_rx_queue(impl, s->queue[i]);
      s->queue[i] = NULL;
    }
    if (s->rss[i]) {
      mt_rss_put(s->rss[i]);
      s->rss[i] = NULL;
    }
  }

  return 0;
}

static int rv_init_hw(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  int idx = s->idx, num_port = ops->num_port;
  struct mt_rx_flow flow;
  enum mtl_port port;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].port = port;

    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, ops->sip_addr[i], MTL_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st20_dst_port[i];
    if (rv_is_hdr_split(s)) {
      flow.hdr_split = true;
#ifdef ST_HAS_DPDK_HDR_SPLIT
      flow.hdr_split_mbuf_cb_priv = s;
      flow.hdr_split_mbuf_cb = rv_hdrs_mbuf_callback_fn;
#else
      err("%s(%d), no hdr_split support on this build\n", __func__, idx);
      rv_uinit_hw(impl, s);
      return -ENOTSUP;
#endif
    } else {
      flow.hdr_split = false;
    }

    /* no flow for data path only */
    if (mt_pmd_is_kernel(impl, port) && (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY))
      s->queue[i] = mt_dev_get_rx_queue(impl, port, NULL);
    else if (mt_has_rss(impl, port)) {
      flow.priv = &s->priv[i];
      flow.cb = rv_handle_mbuf;
      s->rss[i] = mt_rss_get(impl, port, &flow);
    } else
      s->queue[i] = mt_dev_get_rx_queue(impl, port, &flow);
    if (!s->queue[i] && !s->rss[i]) {
      rv_uinit_hw(impl, s);
      return -EIO;
    }
    s->port_id[i] = mt_port_id(impl, port);
    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         mt_has_rss(impl, port) ? mt_rss_queue_id(s->rss[i])
                                : mt_dev_rx_queue_id(s->queue[i]),
         flow.dst_port);
  }

  return 0;
}

static int rv_uinit_mcast(struct mtl_main_impl* impl,
                          struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;

  for (int i = 0; i < ops->num_port; i++) {
    if (mt_is_multicast_ip(ops->sip_addr[i]))
      mt_mcast_leave(impl, mt_ip_to_u32(ops->sip_addr[i]),
                     mt_port_logic2phy(s->port_maps, i));
  }

  return 0;
}

static int rv_init_mcast(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  int ret;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!mt_is_multicast_ip(ops->sip_addr[i])) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    if (mt_pmd_is_kernel(impl, port) && (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY)) {
      info("%s(%d), skip mcast join for port %d\n", __func__, s->idx, i);
      return 0;
    }
    ret = mt_mcast_join(impl, mt_ip_to_u32(ops->sip_addr[i]), port);
    if (ret < 0) return ret;
  }

  return 0;
}

static int rv_init_pkt_handler(struct st_rx_video_session_impl* s) {
  if (st20_is_frame_type(s->ops.type)) {
    enum st20_detect_status detect_status = s->detector.status;
    if (detect_status == ST20_DETECT_STAT_DETECTING) {
      s->pkt_handler = rv_handle_detect_pkt;
    } else if ((detect_status != ST20_DETECT_STAT_SUCCESS) &&
               (detect_status != ST20_DETECT_STAT_DISABLED)) {
      s->pkt_handler = rv_handle_detect_err;
    } else {
      if (s->st22_info)
        s->pkt_handler = rv_handle_st22_pkt;
      else if (rv_is_hdr_split(s))
        s->pkt_handler = rv_handle_hdr_split_pkt;
      else
        s->pkt_handler = rv_handle_frame_pkt;
    }
  } else {
    s->pkt_handler = rv_handle_rtp_pkt;
  }

  return 0;
}

static int rv_attach(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                     struct st_rx_video_session_impl* s, struct st20_rx_ops* ops,
                     struct st22_rx_ops* st22_ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  ret = st20_get_pgroup(ops->fmt, &s->st20_pg);
  if (ret < 0) {
    err("%s(%d), get pgroup fail %d\n", __func__, idx, ret);
    return ret;
  }

  if (st20_is_frame_type(ops->type) && (ops->flags & ST20_RX_FLAG_HDR_SPLIT)) {
    s->is_hdr_split = true;
    info("%s(%d), hdr_split enabled in ops\n", __func__, idx);
  }

  s->st20_bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_linesize = s->st20_bytes_in_line;
  if (ops->linesize > s->st20_linesize)
    s->st20_linesize = ops->linesize;
  else if (ops->linesize) {
    err("%s(%d), invalid linesize %u\n", __func__, idx, ops->linesize);
    return -EINVAL;
  }

  s->st20_fb_size = s->st20_linesize * ops->height;
  if (ops->interlaced) s->st20_fb_size = s->st20_fb_size >> 1;
  s->slice_lines = ops->slice_lines;
  if (!s->slice_lines) s->slice_lines = ops->height / 32;
  s->slice_size = ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_frames_cnt = ops->framebuff_cnt;
  if (st22_ops) {
    s->st20_frame_size = st22_ops->framebuff_max_size;
    s->st20_fb_size = s->st20_frame_size;
  } else
    s->st20_frame_size = ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_uframe_size = ops->uframe_size;
  if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
  /* at least 800 byte for each packet */
  s->st20_frame_bitmap_size = s->st20_frame_size / 800 / 8;
  /* one line at line 2 packets for all the format */
  if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
    s->st20_frame_bitmap_size = ops->height * 2 / 8;
  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
  }

  s->stat_pkts_idx_dropped = 0;
  s->stat_pkts_idx_oo_bitmap = 0;
  s->stat_pkts_no_slot = 0;
  s->stat_pkts_offset_dropped = 0;
  s->stat_pkts_redunant_dropped = 0;
  s->stat_pkts_wrong_hdr_dropped = 0;
  s->stat_pkts_received = 0;
  s->stat_pkts_dma = 0;
  s->stat_pkts_rtp_ring_full = 0;
  s->stat_frames_dropped = 0;
  rte_atomic32_set(&s->stat_frames_received, 0);
  rte_atomic32_set(&s->cbs_incomplete_frame_cnt, 0);
  rte_atomic32_set(&s->cbs_frame_slot_cnt, 0);
  s->stat_last_time = mt_get_monotonic_time();
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

  ret = rv_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rv_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  if (st20_is_frame_type(ops->type) && (!st22_ops) &&
      ((ops->flags & ST20_RX_FLAG_AUTO_DETECT) || mt_has_ebu(impl))) {
    /* init sw after detected */
    ret = rv_detector_init(impl, s);
    if (ret < 0) {
      err("%s(%d), rv_detector_init fail %d\n", __func__, idx, ret);
      rv_uinit_hw(impl, s);
      return -EIO;
    }
  } else {
    ret = rv_init_sw(impl, mgr, s, st22_ops);
    if (ret < 0) {
      err("%s(%d), rv_init_sw fail %d\n", __func__, idx, ret);
      rv_uinit_hw(impl, s);
      return -EIO;
    }
  }

  ret = rv_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rv_init_mcast fail %d\n", __func__, idx, ret);
    rv_uinit_sw(impl, s);
    rv_uinit_hw(impl, s);
    return -EIO;
  }

  ret = rv_init_pkt_handler(s);
  if (ret < 0) {
    err("%s(%d), init pkt handler fail %d\n", __func__, idx, ret);
    rv_uinit_sw(impl, s);
    rv_uinit_hw(impl, s);
    return -EIO;
  }

  info("%s(%d), %d frames with size %" PRIu64 "(%" PRIu64 ",%" PRIu64 "), type %d\n",
       __func__, idx, s->st20_frames_cnt, s->st20_frame_size, s->st20_frame_bitmap_size,
       s->st20_uframe_size, ops->type);
  info("%s(%d), w %u h %u fmt %s packing %d pt %d flags 0x%x\n", __func__, idx,
       ops->width, ops->height, st20_frame_fmt_name(ops->fmt), ops->packing,
       ops->payload_type, ops->flags);
  return 0;
}

static int rv_poll_vsync(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st_vsync_info* vsync = &s->vsync;
  uint64_t cur_tsc = mt_get_tsc(impl);

  if (!vsync->init) return 0;

  if (cur_tsc > vsync->next_epoch_tsc) {
    uint64_t tsc_delta = cur_tsc - vsync->next_epoch_tsc;
    dbg("%s(%d), vsync with epochs %" PRIu64 "\n", __func__, s->idx, vsync->meta.epoch);
    s->ops.notify_event(s->ops.priv, ST_EVENT_VSYNC, &vsync->meta);
    st_vsync_calculate(impl, vsync); /* set next vsync */
    /* check tsc delta for status */
    if (tsc_delta > NS_PER_MS) s->stat_vsync_mismatch++;
  }

  return 0;
}

static int rvs_tasklet_handler(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parnet;
  struct st_rx_video_session_impl* s;
  int sidx;
  int pending = MT_TASKLET_ALL_DONE;

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    /* check vsync if it has vsync flag enabled */
    if (s->ops.flags & ST20_RX_FLAG_ENABLE_VSYNC) rv_poll_vsync(impl, s);

    pending += rv_tasklet(impl, s, mgr);
    rx_video_session_put(mgr, sidx);
  }

  return pending;
}

void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s) {
  rte_atomic32_set(&s->nic_burst_cnt, 0);
  rte_atomic32_set(&s->nic_inflight_cnt, 0);
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  rte_atomic32_set(&s->cbs_frame_slot_cnt, 0);
  rte_atomic32_set(&s->cbs_incomplete_frame_cnt, 0);
  s->cpu_busy_score = 0;
  s->dma_busy_score = 0;
}

void rx_video_session_cal_cpu_busy(struct st_rx_video_session_impl* s) {
  float nic_burst_cnt = rte_atomic32_read(&s->nic_burst_cnt);
  float nic_inflight_cnt = rte_atomic32_read(&s->nic_inflight_cnt);
  float dma_previous_busy_cnt = rte_atomic32_read(&s->dma_previous_busy_cnt);
  int frame_slot_cnt = rte_atomic32_read(&s->cbs_frame_slot_cnt);
  int incomplete_frame_cnt = rte_atomic32_read(&s->cbs_incomplete_frame_cnt);
  float cpu_busy_score = 0;
  float dma_busy_score = s->dma_busy_score;     /* save old */
  float old_cpu_busy_score = s->cpu_busy_score; /* save old */

  rx_video_session_clear_cpu_busy(s);

  if (nic_burst_cnt) {
    cpu_busy_score = 100.0 * nic_inflight_cnt / nic_burst_cnt;
  }
  if ((frame_slot_cnt > 10) && (incomplete_frame_cnt > 10)) {
    /* do we need check if imiss? */
    cpu_busy_score = old_cpu_busy_score + 40;
  }
  if (cpu_busy_score > 100.0) cpu_busy_score = 100.0;
  s->cpu_busy_score = cpu_busy_score;

  if (dma_previous_busy_cnt) {
    dma_busy_score += 40.0;
    if (dma_busy_score > 100.0) dma_busy_score = 100.0;
  } else {
    dma_busy_score = 0;
  }
  s->dma_busy_score = dma_busy_score;
}

static int rv_migrate_dma(struct mtl_main_impl* impl,
                          struct st_rx_video_session_impl* s) {
  rv_free_dma(impl, s);
  rv_init_dma(impl, s);
  return 0;
}

static void rv_stat(struct st_rx_video_sessions_mgr* mgr,
                    struct st_rx_video_session_impl* s) {
  int m_idx = mgr ? mgr->idx : 0, idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->stat_frames_received, 0);

  if (s->stat_slices_received) {
    notice(
        "RX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d slices %d, cpu busy %f\n",
        m_idx, idx, s->ops_name, framerate, frames_received, s->stat_pkts_received,
        s->stat_slices_received, s->cpu_busy_score);
  } else {
    notice("RX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d, cpu busy %f\n", m_idx,
           idx, s->ops_name, framerate, frames_received, s->stat_pkts_received,
           s->cpu_busy_score);
  }
  s->stat_pkts_received = 0;
  s->stat_slices_received = 0;
  s->stat_last_time = cur_time_ns;

  if (s->stat_frames_dropped || s->stat_pkts_idx_dropped || s->stat_pkts_offset_dropped) {
    notice(
        "RX_VIDEO_SESSION(%d,%d): incomplete frames %d, pkts (idx error: %d, offset "
        "error: %d, idx out of bitmap: %d)\n",
        m_idx, idx, s->stat_frames_dropped, s->stat_pkts_idx_dropped,
        s->stat_pkts_offset_dropped, s->stat_pkts_idx_oo_bitmap);
    s->stat_frames_dropped = 0;
    s->stat_pkts_idx_dropped = 0;
    s->stat_pkts_idx_oo_bitmap = 0;
  }
  if (s->stat_pkts_rtp_ring_full) {
    notice("RX_VIDEO_SESSION(%d,%d): rtp dropped pkts %d as ring full\n", m_idx, idx,
           s->stat_pkts_rtp_ring_full);
    s->stat_pkts_rtp_ring_full = 0;
  }
  if (s->stat_pkts_no_slot) {
    notice("RX_VIDEO_SESSION(%d,%d): dropped pkts %d as no slot\n", m_idx, idx,
           s->stat_pkts_no_slot);
    s->stat_pkts_no_slot = 0;
  }
  if (s->stat_pkts_redunant_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): redunant dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_redunant_dropped);
    s->stat_pkts_redunant_dropped = 0;
  }
  if (s->stat_pkts_wrong_hdr_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong hdr dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_hdr_dropped);
    s->stat_pkts_wrong_hdr_dropped = 0;
  }
  if (s->stat_pkts_enqueue_fallback) {
    notice("RX_VIDEO_SESSION(%d,%d): lcore enqueue fallback pkts %d\n", m_idx, idx,
           s->stat_pkts_enqueue_fallback);
    s->stat_pkts_enqueue_fallback = 0;
  }
  if (s->dma_dev) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d by dma copy, dma busy %f\n", m_idx, idx,
           s->stat_pkts_dma, s->dma_busy_score);
    s->stat_pkts_dma = 0;
  }
  if (s->stat_pkts_slice_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d drop as slice add fail\n", m_idx, idx,
           s->stat_pkts_slice_fail);
    s->stat_pkts_slice_fail = 0;
  }
  if (s->stat_pkts_slice_merged) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d merged as slice\n", m_idx, idx,
           s->stat_pkts_slice_merged);
    s->stat_pkts_slice_merged = 0;
  }
  if (s->stat_pkts_multi_segments_received) {
    notice("RX_VIDEO_SESSION(%d,%d): multi segments pkts %d\n", m_idx, idx,
           s->stat_pkts_multi_segments_received);
    s->stat_pkts_multi_segments_received = 0;
  }
  if (s->stat_pkts_not_bpm) {
    notice("RX_VIDEO_SESSION(%d,%d): not bpm hdr split pkts %d\n", m_idx, idx,
           s->stat_pkts_not_bpm);
    s->stat_pkts_not_bpm = 0;
  }
  if (s->stat_pkts_wrong_payload_hdr_split) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong payload hdr split pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_payload_hdr_split);
    s->stat_pkts_wrong_payload_hdr_split = 0;
  }
  if (s->stat_mismatch_hdr_split_frame) {
    notice("RX_VIDEO_SESSION(%d,%d): hdr split mismatch frames %d\n", m_idx, idx,
           s->stat_mismatch_hdr_split_frame);
    s->stat_mismatch_hdr_split_frame = 0;
  }
  if (s->stat_vsync_mismatch) {
    notice("RX_VIDEO_SESSION(%d,%d): vsync mismatch cnt %u\n", m_idx, idx,
           s->stat_vsync_mismatch);
    s->stat_vsync_mismatch = 0;
  }
  if (s->stat_slot_get_frame_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): slot get frame fail %u\n", m_idx, idx,
           s->stat_slot_get_frame_fail);
    s->stat_slot_get_frame_fail = 0;
  }
  if (s->stat_slot_query_ext_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): slot query ext fail %u\n", m_idx, idx,
           s->stat_slot_query_ext_fail);
    s->stat_slot_query_ext_fail = 0;
  }
}

static int rvs_tasklet_start(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  int idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parnet;
  struct st_rx_video_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_video_session_try_get(mgr, sidx);
    if (!s) continue;
    /* re-calculate the vsync */
    st_vsync_calculate(impl, &s->vsync);
    rx_video_session_put(mgr, sidx);
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rvs_tasklet_stop(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rv_detach(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                     struct st_rx_video_session_impl* s) {
  if (mt_has_ebu(mgr->parnet)) rv_ebu_final_result(s);
  rv_stat(mgr, s);
  rv_uinit_mcast(impl, s);
  rv_uinit_sw(impl, s);
  rv_uinit_hw(impl, s);
  return 0;
}

static int rv_update_src(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s,
                         struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st20_rx_ops* ops = &s->ops;

  rv_uinit_mcast(impl, s);
  rv_uinit_hw(impl, s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->sip_addr[i], src->sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
  }

  ret = rv_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rv_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int rv_mgr_update_src(struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s,
                             struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rv_update_src(mgr->parnet, s, src);
  rx_video_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int rvs_mgr_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch,
                        struct st_rx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mt_sch_tasklet_ops ops;

  mgr->parnet = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_video_sessions_mgr";
  ops.start = rvs_tasklet_start;
  ops.stop = rvs_tasklet_stop;
  ops.handler = rvs_tasklet_handler;

  mgr->tasklet = mt_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mt_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rvs_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                          struct st_rx_video_session_impl* s, int idx) {
  rv_detach(mgr->parnet, mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);
  return 0;
}

static int rvs_mgr_uinit(struct st_rx_video_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_video_session_impl* s;

  if (mgr->tasklet) {
    mt_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = rx_video_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rvs_mgr_detach(mgr, s, i);
    rx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static struct st_rx_video_session_impl* rv_mgr_attach(
    struct st_rx_video_sessions_mgr* mgr, struct st20_rx_ops* ops,
    struct st22_rx_ops* st22_ops) {
  int midx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parnet;
  int ret;
  struct st_rx_video_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (!rx_video_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, MTL_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      return NULL;
    }
    ret = rv_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rv_attach(mgr->parnet, mgr, s, ops, st22_ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      mt_rte_free(s);
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

static int st_rvs_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rvs_mgr_detach(mgr, s, idx);

  rx_video_session_put(mgr, idx);

  return 0;
}

static int rvs_mgr_update(struct st_rx_video_sessions_mgr* mgr) {
  int max_idx = 0;
  struct mtl_main_impl* impl = mgr->parnet;
  uint64_t sleep_us = mt_sch_default_sleep_us(impl);
  struct st_rx_video_session_impl* s;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = mgr->sessions[i];
    if (!s) continue;
    max_idx = i + 1;
    sleep_us = RTE_MIN(s->advice_sleep_us, sleep_us);
  }
  dbg("%s(%d), sleep us %" PRIu64 ", max_idx %d\n", __func__, mgr->idx, sleep_us,
      max_idx);
  mgr->max_idx = max_idx;
  if (mgr->tasklet) mt_tasklet_set_sleep(mgr->tasklet, sleep_us);
  return 0;
}

void st_rx_video_sessions_stat(struct mtl_main_impl* impl) {
  struct mt_sch_impl* sch;
  struct st_rx_video_sessions_mgr* mgr;
  struct st_rx_video_session_impl* s;

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    if (!mt_sch_is_active(sch)) continue;
    mgr = &sch->rx_video_mgr;
    for (int j = 0; j < mgr->max_idx; j++) {
      s = rx_video_session_get(mgr, j);
      if (!s) continue;
      rv_stat(mgr, s);
      rx_video_session_put(mgr, j);
    }
  }
}

int st_rx_video_sessions_sch_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->rx_video_init) return 0;

  /* create video context */
  ret = rvs_mgr_init(impl, sch, &sch->rx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_rvs_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->rx_video_init = true;
  return 0;
}

int st_rx_video_sessions_sch_uinit(struct mtl_main_impl* impl, struct mt_sch_impl* sch) {
  if (!sch->rx_video_init) return 0;

  rvs_mgr_uinit(&sch->rx_video_mgr);
  sch->rx_video_init = false;

  return 0;
}

int st_rx_video_session_migrate(struct mtl_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx) {
  rv_init(impl, mgr, s, idx);
  if (s->dma_dev) rv_migrate_dma(impl, s);
  return 0;
}

static int rv_ops_check(struct st20_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;
  enum st20_type type = ops->type;

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

  if (st20_is_frame_type(type)) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
    if (ops->type == ST20_TYPE_SLICE_LEVEL) {
      if (!ops->notify_slice_ready) {
        err("%s, pls set notify_slice_ready\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->flags & ST20_RX_FLAG_AUTO_DETECT) {
      if (!ops->notify_detected) {
        err("%s, pls set notify_detected\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->query_ext_frame) {
      if (!(ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
        err("%s, pls enable incomplete frame flag for query ext mode\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->flags & ST20_RX_FLAG_HDR_SPLIT) {
      if (num_ports > 1) {
        /* only 1 port allowed since the pkt payload is assigned to frame directly */
        err("%s, hdr split only support 1 port, num_ports %d\n", __func__, num_ports);
        return -EINVAL;
      }
    }
  }

  if (ops->uframe_size) {
    if (!ops->uframe_pg_callback) {
      err("%s, pls set uframe_pg_callback\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    if (!(ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
      err("%s, pls enable ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME for silce mode\n",
          __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int rv_st22_ops_check(struct st22_rx_ops* ops) {
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

  if (ops->type == ST22_TYPE_FRAME_LEVEL) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST22_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST22_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (ops->pack_type != ST22_PACK_CODESTREAM) {
      err("%s, invalid pack_type %d\n", __func__, ops->pack_type);
      return -EINVAL;
    }
    if (!ops->framebuff_max_size) {
      err("%s, pls set framebuff_max_size\n", __func__);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
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

st20_rx_handle st20_rx_create_with_mask(struct mtl_main_impl* impl,
                                        struct st20_rx_ops* ops, mt_sch_mask_t sch_mask) {
  struct mt_sch_impl* sch;
  struct st_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret, quota_mbs_wo_dma = 0;
  uint64_t bps;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rv_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;
  if (!mt_has_user_quota(impl)) {
    if (ST20_TYPE_RTP_LEVEL == ops->type) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_RTP_PER_SCH;
    } else {
      quota_mbs_wo_dma =
          quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_NO_DMA_PER_SCH;
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_PER_SCH;
    }
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), mt_socket_id(impl, MTL_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  enum mt_sch_type type =
      mt_has_rxv_separate_sch(impl) ? MT_SCH_TYPE_RX_VIDEO_ONLY : MT_SCH_TYPE_DEFAULT;
  sch = mt_sch_get(impl, quota_mbs, type, sch_mask);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = rv_mgr_attach(&sch->rx_video_mgr, ops, NULL);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), rv_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  if (!mt_has_user_quota(impl) && st20_is_frame_type(ops->type) && !s->dma_dev) {
    int extra_quota_mbs = quota_mbs_wo_dma - quota_mbs;
    ret = mt_sch_add_quota(sch, extra_quota_mbs);
    if (ret >= 0) quota_mbs += extra_quota_mbs;
  }

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  s_impl->parnet = impl;
  s_impl->type = MT_HANDLE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st20_handle = s_impl;

  rte_atomic32_inc(&impl->st20_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

st20_rx_handle st20_rx_create(mtl_handle mt, struct st20_rx_ops* ops) {
  return st20_rx_create_with_mask(mt, ops, MT_SCH_MASK_ALL);
}

int st20_rx_update_source(st20_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rv_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st20_rx_get_sch_idx(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st20_rx_pcapng_dump(st20_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;
  struct mtl_main_impl* impl = s_impl->parnet;
  int ret;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  ret = rv_start_pcapng(impl, s, max_dump_packets, sync, meta);

  return ret;
}

int st20_rx_free(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct mt_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;

  /* no need to lock as session is located already */
  ret = st_rvs_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

int st20_rx_put_framebuff(st20_rx_handle handle, void* frame) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return rv_put_frame(s, frame);
}

size_t st20_rx_get_framebuffer_size(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_fb_size;
}

int st20_rx_get_framebuffer_count(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  s = s_impl->impl;
  return s->st20_frames_cnt;
}

void* st20_rx_get_mbuf(st20_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->rtps_ring;
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

void st20_rx_put_mbuf(st20_rx_handle handle, void* mbuf) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_HANDLE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

bool st20_rx_dma_enabled(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return s->dma_dev ? true : false;
}

int st20_rx_get_queue_meta(st20_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  enum mtl_port port;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
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

st22_rx_handle st22_rx_create(mtl_handle mt, struct st22_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mt_sch_impl* sch;
  struct st22_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_rx_ops st20_ops;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rv_st22_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    ret = st20_get_bandwidth_bps(ops->width, ops->height, ST20_FMT_YUV_422_10BIT,
                                 ops->fps, &bps);
    if (ret < 0) {
      err("%s, get_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    bps /= 4; /* default compress ratio 1/4 */
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
    quota_mbs *= 2; /* double quota for RTP path */
  } else {
    ret = st22_frame_bandwidth_bps(ops->framebuff_max_size, ops->fps, &bps);
    if (ret < 0) {
      err("%s, frame_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), mt_socket_id(impl, MTL_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  enum mt_sch_type type =
      mt_has_rxv_separate_sch(impl) ? MT_SCH_TYPE_RX_VIDEO_ONLY : MT_SCH_TYPE_DEFAULT;
  sch = mt_sch_get(impl, quota_mbs, type, MT_SCH_MASK_ALL);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  /* reuse st20 type */
  memset(&st20_ops, 0, sizeof(st20_ops));
  st20_ops.name = ops->name;
  st20_ops.priv = ops->priv;
  st20_ops.num_port = ops->num_port;
  for (int i = 0; i < ops->num_port; i++) {
    memcpy(st20_ops.sip_addr[i], ops->sip_addr[i], MTL_IP_ADDR_LEN);
    strncpy(st20_ops.port[i], ops->port[i], MTL_PORT_MAX_LEN);
    st20_ops.udp_port[i] = ops->udp_port[i];
  }
  if (ops->flags & ST22_RX_FLAG_DATA_PATH_ONLY)
    st20_ops.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST22_RX_FLAG_ENABLE_VSYNC) st20_ops.flags |= ST20_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    st20_ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  st20_ops.pacing = ops->pacing;
  if (ops->type == ST22_TYPE_RTP_LEVEL)
    st20_ops.type = ST20_TYPE_RTP_LEVEL;
  else
    st20_ops.type = ST20_TYPE_FRAME_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.fmt = ST20_FMT_YUV_422_10BIT;
  st20_ops.payload_type = ops->payload_type;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.notify_rtp_ready = ops->notify_rtp_ready;
  st20_ops.framebuff_cnt = ops->framebuff_cnt;
  st20_ops.notify_event = ops->notify_event;
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = rv_mgr_attach(&sch->rx_video_mgr, &st20_ops, ops);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), rv_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = MT_ST22_HANDLE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st22_handle = s_impl;

  rte_atomic32_inc(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st22_rx_update_source(st22_rx_handle handle, struct st_rx_source_info* src) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rv_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st22_rx_get_sch_idx(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st22_rx_pcapng_dump(st22_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;
  struct mtl_main_impl* impl = s_impl->parnet;
  int ret;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  ret = rv_start_pcapng(impl, s, max_dump_packets, sync, meta);

  return ret;
}

int st22_rx_free(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct mt_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;

  /* no need to lock as session is located already */
  ret = st_rvs_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

void* st22_rx_get_mbuf(st22_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->rtps_ring;
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

void st22_rx_put_mbuf(st22_rx_handle handle, void* mbuf) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st22_rx_put_framebuff(st22_rx_handle handle, void* frame) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return rv_put_frame(s, frame);
}

void* st22_rx_get_fb_addr(st22_rx_handle handle, uint16_t idx) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st20_frames[idx].addr;
}

int st22_rx_get_queue_meta(st22_rx_handle handle, struct st_queue_meta* meta) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  enum mtl_port port;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
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
