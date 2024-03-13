/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "st_rx_timing_parser.h"

#include "../mt_log.h"

static inline float rv_tp_calculate_avg(uint32_t cnt, int64_t sum) {
  return cnt ? ((float)sum / cnt) : -1.0f;
}

void rv_tp_on_packet(struct st_rx_video_session_impl* s, enum mtl_session_port s_port,
                     struct st_rv_tp_slot* slot, uint32_t rtp_tmstamp, uint64_t pkt_time,
                     int pkt_idx) {
  struct st_rx_video_tp* tp = s->tp;
  uint64_t epoch_tmstamp;
  double tvd, trs = tp->trs;

  if (!slot->cur_epochs) { /* the first packet */
    uint64_t epochs = (double)pkt_time / s->frame_time;
    uint64_t epoch_tmstamp = (double)epochs * s->frame_time;

    slot->cur_epochs = epochs;
    slot->rtp_tmstamp = rtp_tmstamp;
    double first_pkt_time = (double)pkt_time - (trs * pkt_idx);
    slot->first_pkt_time = first_pkt_time;
    slot->meta.fpt = first_pkt_time - epoch_tmstamp;

    uint64_t tmstamp64 = epochs * s->frame_time_sampling;
    uint32_t tmstamp32 = tmstamp64;
    double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;
    double diff_rtp_ts_ns = diff_rtp_ts * s->frame_time / s->frame_time_sampling;
    slot->meta.latency = slot->meta.fpt - diff_rtp_ts_ns;
    slot->meta.rtp_offset = diff_rtp_ts;
    if (tp->pre_rtp_tmstamp[s_port]) {
      slot->meta.rtp_ts_delta = rtp_tmstamp - tp->pre_rtp_tmstamp[s_port];
    }
    tp->pre_rtp_tmstamp[s_port] = rtp_tmstamp;
  }

  epoch_tmstamp = (uint64_t)(slot->cur_epochs * s->frame_time);
  tvd = epoch_tmstamp + tp->pass.tr_offset;
  double expect_time = tvd + trs * (pkt_idx + 1);

  /* Calculate vrx */
  int32_t vrx_cur = (expect_time - pkt_time) / trs;
  slot->vrx_sum += vrx_cur;
  slot->meta.vrx_min = RTE_MIN(vrx_cur, slot->meta.vrx_min);
  slot->meta.vrx_max = RTE_MAX(vrx_cur, slot->meta.vrx_max);

  /* Calculate C-inst */
  int exp_cin_pkts = ((pkt_time - slot->first_pkt_time) / trs) * ST_TP_CINST_DRAIN_FACTOR;
  int cinst = RTE_MAX(0, pkt_idx - exp_cin_pkts);
  slot->cinst_sum += cinst;
  slot->meta.cinst_min = RTE_MIN(cinst, slot->meta.cinst_min);
  slot->meta.cinst_max = RTE_MAX(cinst, slot->meta.cinst_max);

  /* calculate Inter-packet time */
  if (slot->prev_pkt_time) {
    double ipt = (double)pkt_time - slot->prev_pkt_time;

    slot->ipt_sum += ipt;
    slot->meta.ipt_min = RTE_MIN(ipt, slot->meta.ipt_min);
    slot->meta.ipt_max = RTE_MAX(ipt, slot->meta.ipt_max);
  }
  slot->prev_pkt_time = pkt_time;

  slot->meta.pkts_cnt++;
}

static void rv_tp_compliant_set_cause(struct st20_rx_tp_meta* meta, char* cause) {
  snprintf(meta->failed_cause, sizeof(meta->failed_cause), "%s", cause);
}

static enum st_rx_tp_compliant rv_tp_compliant(struct st_rx_video_tp* tp,
                                               struct st_rv_tp_slot* slot) {
  /* fpt check */
  if (slot->meta.fpt > tp->pass.tr_offset) {
    rv_tp_compliant_set_cause(&slot->meta, "fpt exceed tr_offset");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  /* rtp ts delta check */
  if (slot->meta.rtp_ts_delta < tp->pass.rtp_ts_delta_min) {
    rv_tp_compliant_set_cause(&slot->meta, "rtp_ts_delta exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.rtp_ts_delta > tp->pass.rtp_ts_delta_max) {
    rv_tp_compliant_set_cause(&slot->meta, "rtp_ts_delta exceed max");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  /* rtp offset check */
  if (slot->meta.rtp_offset < tp->pass.rtp_offset_min) {
    rv_tp_compliant_set_cause(&slot->meta, "rtp_offset exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.rtp_offset > tp->pass.rtp_offset_max) {
    rv_tp_compliant_set_cause(&slot->meta, "rtp_offset exceed max");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  /* latency check */
  if (slot->meta.latency < tp->pass.latency_min) {
    rv_tp_compliant_set_cause(&slot->meta, "latency exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.latency > tp->pass.latency_max) {
    rv_tp_compliant_set_cause(&slot->meta, "latency exceed max");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  /* vrx check */
  if (slot->meta.vrx_min < tp->pass.vrx_min) {
    rv_tp_compliant_set_cause(&slot->meta, "vrx exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.vrx_max > tp->pass.vrx_max_wide) {
    rv_tp_compliant_set_cause(&slot->meta, "vrx exceed max");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  /* narrow or wide */
  if (slot->meta.cinst_min > tp->pass.cinst_min) {
    rv_tp_compliant_set_cause(&slot->meta, "cinst exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.cinst_max > tp->pass.cinst_max_wide) {
    rv_tp_compliant_set_cause(&slot->meta, "cinst exceed max");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.cinst_max > tp->pass.cinst_max_narrow) {
    rv_tp_compliant_set_cause(&slot->meta, "wide as cinst exceed narrow max");
    return ST_RX_TP_COMPLIANT_WIDE;
  }
  if (slot->meta.vrx_max > tp->pass.vrx_max_narrow) {
    rv_tp_compliant_set_cause(&slot->meta, "wide as vrx exceed narrow max");
    return ST_RX_TP_COMPLIANT_WIDE;
  }
  rv_tp_compliant_set_cause(&slot->meta, "narrow");
  return ST_RX_TP_COMPLIANT_NARROW;
}

void rv_tp_slot_parse_result(struct st_rx_video_session_impl* s,
                             enum mtl_session_port s_port, struct st_rv_tp_slot* slot) {
  struct st_rx_video_tp* tp = s->tp;
  float cinst_avg = rv_tp_calculate_avg(slot->meta.pkts_cnt, slot->cinst_sum);
  float vrx_avg = rv_tp_calculate_avg(slot->meta.pkts_cnt, slot->vrx_sum);
  float ipt_avg = rv_tp_calculate_avg(slot->meta.pkts_cnt, slot->ipt_sum);

  slot->meta.cinst_avg = cinst_avg;
  slot->meta.vrx_avg = vrx_avg;
  slot->meta.ipt_avg = ipt_avg;
  dbg("%s(%d), Cinst AVG %.2f MIN %d MAX %d test %s!\n", __func__, s->idx, cinst_avg,
      slot->cinst_min, slot->cinst_max, rv_tp_cinst_result(tp, slot));
  dbg("%s(%d), VRX AVG %.2f MIN %d MAX %d test %s!\n", __func__, s->idx, vrx_avg,
      slot->vrx_min, slot->vrx_max, rv_tp_vrx_result(tp, slot));
  dbg("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n", __func__, s->idx,
      ipt_avg, slot->ipt_min, slot->ipt_max);

  /* parse tp compliant for current frame */
  enum st_rx_tp_compliant compliant = rv_tp_compliant(tp, slot);
  slot->meta.compliant = compliant;

  if (!s->enable_timing_parser_stat) return;

  /* update stat */
  struct st_rv_tp_stat* stat = &tp->stat[s_port];
  struct st_rv_tp_slot* stat_slot = &stat->slot;

  stat->stat_compliant_result[compliant]++;

  stat_slot->vrx_sum += slot->vrx_sum;
  stat_slot->meta.vrx_min = RTE_MIN(stat_slot->meta.vrx_min, slot->meta.vrx_min);
  stat_slot->meta.vrx_max = RTE_MAX(stat_slot->meta.vrx_min, slot->meta.vrx_max);
  stat_slot->cinst_sum += slot->cinst_sum;
  stat_slot->meta.cinst_min = RTE_MIN(stat_slot->meta.cinst_min, slot->meta.cinst_min);
  stat_slot->meta.cinst_max = RTE_MAX(stat_slot->meta.cinst_max, slot->meta.cinst_max);
  stat_slot->ipt_sum += slot->ipt_sum;
  stat_slot->meta.ipt_min = RTE_MIN(stat_slot->meta.ipt_min, slot->meta.ipt_min);
  stat_slot->meta.ipt_max = RTE_MAX(stat_slot->meta.ipt_min, slot->meta.ipt_max);
  stat_slot->meta.pkts_cnt += slot->meta.pkts_cnt;

  stat->stat_fpt_min = RTE_MIN(stat->stat_fpt_min, slot->meta.fpt);
  stat->stat_fpt_max = RTE_MAX(stat->stat_fpt_max, slot->meta.fpt);
  stat->stat_fpt_sum += slot->meta.fpt;
  stat->stat_latency_min = RTE_MIN(stat->stat_latency_min, slot->meta.latency);
  stat->stat_latency_max = RTE_MAX(stat->stat_latency_max, slot->meta.latency);
  stat->stat_latency_sum += slot->meta.latency;
  stat->stat_rtp_offset_min = RTE_MIN(stat->stat_rtp_offset_min, slot->meta.rtp_offset);
  stat->stat_rtp_offset_max = RTE_MAX(stat->stat_rtp_offset_max, slot->meta.rtp_offset);
  stat->stat_rtp_offset_sum += slot->meta.rtp_offset;
  if (slot->meta.rtp_ts_delta) {
    stat->stat_rtp_ts_delta_min =
        RTE_MIN(stat->stat_rtp_ts_delta_min, slot->meta.rtp_ts_delta);
    stat->stat_rtp_ts_delta_max =
        RTE_MAX(stat->stat_rtp_ts_delta_max, slot->meta.rtp_ts_delta);
    stat->stat_rtp_ts_delta_sum += slot->meta.rtp_ts_delta;
  }
  stat->stat_frame_cnt++;
}

static void rv_tp_stat_init(struct st_rx_video_session_impl* s,
                            struct st_rx_video_tp* tp) {
  MTL_MAY_UNUSED(s);
  for (int s_port = 0; s_port < MTL_SESSION_PORT_MAX; s_port++) {
    struct st_rv_tp_stat* stat = &tp->stat[s_port];

    memset(stat, 0, sizeof(*stat));
    rv_tp_slot_init(&stat->slot);
    stat->stat_fpt_min = INT_MAX;
    stat->stat_fpt_max = INT_MIN;
    stat->stat_latency_min = INT_MAX;
    stat->stat_latency_max = INT_MIN;
    stat->stat_rtp_offset_min = INT_MAX;
    stat->stat_rtp_offset_max = INT_MIN;
    stat->stat_rtp_ts_delta_min = INT_MAX;
    stat->stat_rtp_ts_delta_max = INT_MIN;
  }
}

void rv_tp_stat(struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  struct st_rx_video_tp* tp = s->tp;
  if (!tp) return;

  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    struct st_rv_tp_stat* stat = &tp->stat[s_port];
    struct st_rv_tp_slot* stat_slot = &stat->slot;

    info("%s(%d,%d), COMPLIANT NARROW %d WIDE %d FAILED %d!\n", __func__, idx, s_port,
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_NARROW],
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_WIDE],
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_FAILED]);
    float cinst_avg = rv_tp_calculate_avg(stat_slot->meta.pkts_cnt, stat_slot->cinst_sum);
    float vrx_avg = rv_tp_calculate_avg(stat_slot->meta.pkts_cnt, stat_slot->vrx_sum);
    float ipt_avg = rv_tp_calculate_avg(stat_slot->meta.pkts_cnt, stat_slot->ipt_sum);
    info("%s(%d), Cinst AVG %.2f MIN %d MAX %d!\n", __func__, idx, cinst_avg,
         stat_slot->meta.cinst_min, stat_slot->meta.cinst_max);
    info("%s(%d), VRX AVG %.2f MIN %d MAX %d!\n", __func__, idx, vrx_avg,
         stat_slot->meta.vrx_min, stat_slot->meta.vrx_max);
    info("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n", __func__, idx,
         ipt_avg, stat_slot->meta.ipt_min, stat_slot->meta.ipt_max);
    float fpt_avg = rv_tp_calculate_avg(stat->stat_frame_cnt, stat->stat_fpt_sum);
    info("%s(%d), FPT AVG %.2f MIN %d MAX %d DIFF %d!\n", __func__, idx, fpt_avg,
         stat->stat_fpt_min, stat->stat_fpt_max, stat->stat_fpt_max - stat->stat_fpt_min);
    float latency_avg = rv_tp_calculate_avg(stat->stat_frame_cnt, stat->stat_latency_sum);
    info("%s(%d), LATENCY AVG %.2f MIN %d MAX %d!\n", __func__, idx, latency_avg,
         stat->stat_latency_min, stat->stat_latency_max);
    float rtp_offset_avg =
        rv_tp_calculate_avg(stat->stat_frame_cnt, stat->stat_rtp_offset_sum);
    info("%s(%d), RTP OFFSET AVG %.2f MIN %d MAX %d!\n", __func__, idx, rtp_offset_avg,
         stat->stat_rtp_offset_min, stat->stat_rtp_offset_max);
    float rtp_ts_delta_avg =
        rv_tp_calculate_avg(stat->stat_frame_cnt, stat->stat_rtp_ts_delta_sum);
    info("%s(%d), RTP TS DELTA AVG %.2f MIN %d MAX %d!\n", __func__, idx,
         rtp_ts_delta_avg, stat->stat_rtp_ts_delta_min, stat->stat_rtp_ts_delta_max);
  }

  rv_tp_stat_init(s, tp);
}

void rv_tp_slot_init(struct st_rv_tp_slot* slot) {
  memset(slot, 0, sizeof(*slot));

  slot->meta.cinst_max = INT_MIN;
  slot->meta.cinst_min = INT_MAX;
  slot->meta.vrx_max = INT_MIN;
  slot->meta.vrx_min = INT_MAX;
  slot->meta.ipt_max = INT_MIN;
  slot->meta.ipt_min = INT_MAX;
}

int rv_tp_uinit(struct st_rx_video_session_impl* s) {
  if (s->tp) {
    mt_rte_free(s->tp);
    s->tp = NULL;
  }

  return 0;
}

int rv_tp_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx, ret;
  struct st_rx_video_tp* tp;
  struct st20_rx_ops* ops = &s->ops;
  double frame_time = s->frame_time;
  double frame_time_s;
  struct st_fps_timing fps_tm;

  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }
  frame_time_s = (double)fps_tm.den / fps_tm.mul;

  int st20_total_pkts = s->detector.pkt_per_frame;
  info("%s(%d), st20_total_pkts %d\n", __func__, idx, st20_total_pkts);
  if (!st20_total_pkts) {
    err("%s(%d), can not get total packets number\n", __func__, idx);
    return -EINVAL;
  }

  tp = mt_rte_zmalloc_socket(sizeof(*tp), soc_id);
  if (!tp) {
    err("%s(%d), tp malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  s->tp = tp;

  double reactive = 1080.0 / 1125.0;
  if (ops->interlaced && ops->height <= 576) {
    reactive = (ops->height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  tp->trs = frame_time * reactive / st20_total_pkts;
  if (!ops->interlaced) {
    tp->pass.tr_offset =
        ops->height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 750.0);
  } else {
    if (ops->height == 480) {
      tp->pass.tr_offset = frame_time * (20.0 / 525.0) * 2;
    } else if (ops->height == 576) {
      tp->pass.tr_offset = frame_time * (26.0 / 625.0) * 2;
    } else {
      tp->pass.tr_offset = frame_time * (22.0 / 1125.0) * 2;
    }
  }

  tp->pass.cinst_max_narrow =
      RTE_MAX(4, (double)st20_total_pkts / (43200 * reactive * frame_time_s));
  tp->pass.cinst_max_wide = RTE_MAX(16, (double)st20_total_pkts / (21600 * frame_time_s));
  tp->pass.cinst_min = 0;
  tp->pass.vrx_max_narrow = RTE_MAX(8, st20_total_pkts / (27000 * frame_time_s));
  tp->pass.vrx_max_wide = RTE_MAX(720, st20_total_pkts / (300 * frame_time_s));
  tp->pass.vrx_min = 0;
  tp->pass.latency_max = 1000 * 1000; /* 1000 us */
  tp->pass.latency_min = 0;
  tp->pass.rtp_offset_max =
      ceil((double)tp->pass.tr_offset * fps_tm.sampling_clock_rate / NS_PER_S) + 1;
  tp->pass.rtp_offset_min = -1;
  int32_t sampling = s->frame_time_sampling;
  tp->pass.rtp_ts_delta_max = sampling + 1;
  tp->pass.rtp_ts_delta_min = sampling;

  rv_tp_stat_init(s, tp);

  info("%s[%02d], trs %f tr offset %d sampling %f\n", __func__, idx, tp->trs,
       tp->pass.tr_offset, s->frame_time_sampling);
  info(
      "%s[%02d], cinst_max_narrow %d cinst_max_wide %d vrx_max_narrow %d vrx_max_wide %d "
      "rtp_offset_max %d\n",
      __func__, idx, tp->pass.cinst_max_narrow, tp->pass.cinst_max_wide,
      tp->pass.vrx_max_narrow, tp->pass.vrx_max_wide, tp->pass.rtp_offset_max);
  return 0;
}

static void ra_tp_stat_init(struct st_rx_audio_tp* tp) {
  for (int s_port = 0; s_port < MTL_SESSION_PORT_MAX; s_port++) {
    struct st_ra_tp_stat* stat = &tp->stat[s_port];

    memset(stat, 0, sizeof(*stat));
    ra_tp_slot_init(&stat->slot);
    stat->tsdf_max = INT_MIN;
    stat->tsdf_min = INT_MAX;
  }
}

static void ra_tp_compliant_set_cause(struct st30_rx_tp_meta* meta, char* cause) {
  snprintf(meta->failed_cause, sizeof(meta->failed_cause), "%s", cause);
}

static enum st_rx_tp_compliant ra_tp_slot_compliant(struct st_rx_audio_tp* tp,
                                                    struct st_ra_tp_slot* slot,
                                                    int32_t tsdf) {
  /* dpvr and tsdf check */
  if (slot->meta.dpvr_min < 0) {
    ra_tp_compliant_set_cause(&slot->meta, "dpvr exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.dpvr_max > tp->dpvr_max_pass_wide) {
    ra_tp_compliant_set_cause(&slot->meta, "dpvr exceed max wide");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (tsdf < 0) {
    ra_tp_compliant_set_cause(&slot->meta, "tsdf exceed min");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (tsdf > tp->tsdf_max_pass_wide) {
    ra_tp_compliant_set_cause(&slot->meta, "tsdf exceed max wide");
    return ST_RX_TP_COMPLIANT_FAILED;
  }
  if (slot->meta.dpvr_max > tp->dpvr_max_pass_narrow) {
    ra_tp_compliant_set_cause(&slot->meta, "dpvr exceed max narrow");
    return ST_RX_TP_COMPLIANT_WIDE;
  }
  if (tsdf > tp->tsdf_max_pass_narrow) {
    ra_tp_compliant_set_cause(&slot->meta, "tsdf exceed max narrow");
    return ST_RX_TP_COMPLIANT_WIDE;
  }
  ra_tp_compliant_set_cause(&slot->meta, "narrow");
  return ST_RX_TP_COMPLIANT_NARROW;
}

void ra_tp_slot_parse_result(struct st_rx_audio_session_impl* s,
                             enum mtl_session_port s_port) {
  struct st_rx_audio_tp* tp = s->tp;
  struct st_ra_tp_slot* slot = &s->tp->slot[s_port];
  dbg("%s(%d,%d), start\n", __func__, s->idx, s_port);

  slot->meta.ipt_avg = rv_tp_calculate_avg(slot->meta.pkts_cnt, slot->ipt_sum);
  slot->meta.dpvr_avg = rv_tp_calculate_avg(slot->meta.pkts_cnt, slot->dpvr_sum);

  /* calculate tsdf */
  int32_t tsdf =
      (slot->meta.dpvr_max - slot->dpvr_first) - (slot->meta.dpvr_min - slot->dpvr_first);
  slot->meta.tsdf = tsdf;

  /* parse tp compliant for current frame */
  enum st_rx_tp_compliant compliant = ra_tp_slot_compliant(tp, slot, tsdf);
  slot->meta.compliant = compliant;

  /* update stat */
  struct st_ra_tp_stat* stat = &tp->stat[s_port];
  struct st_ra_tp_slot* stat_slot = &stat->slot;

  stat->stat_compliant_result[compliant]++;
  stat->tsdf_min = RTE_MIN(stat->tsdf_min, tsdf);
  stat->tsdf_max = RTE_MAX(stat->tsdf_max, tsdf);
  stat->tsdf_sum += tsdf;
  stat->tsdf_cnt++;

  stat_slot->dpvr_sum += slot->dpvr_sum;
  stat_slot->meta.dpvr_min = RTE_MIN(stat_slot->meta.dpvr_min, slot->meta.dpvr_min);
  stat_slot->meta.dpvr_max = RTE_MAX(stat_slot->meta.dpvr_max, slot->meta.dpvr_max);

  if (!stat_slot->dpvr_first) stat_slot->dpvr_first = slot->dpvr_first;

  stat_slot->ipt_sum += slot->ipt_sum;
  stat_slot->meta.ipt_min = RTE_MIN(stat_slot->meta.ipt_min, slot->meta.ipt_min);
  stat_slot->meta.ipt_max = RTE_MAX(stat_slot->meta.ipt_max, slot->meta.ipt_max);

  stat_slot->meta.pkts_cnt += slot->meta.pkts_cnt;
}

void ra_tp_on_packet(struct st_rx_audio_session_impl* s, enum mtl_session_port s_port,
                     uint32_t rtp_tmstamp, uint64_t pkt_time) {
  struct st_rx_audio_tp* tp = s->tp;
  struct st_ra_tp_slot* slot = &tp->slot[s_port];

  uint64_t epochs = (double)pkt_time / tp->pkt_time;
  uint64_t epoch_tmstamp = (double)epochs * tp->pkt_time;
  double fpt_delta = (double)pkt_time - epoch_tmstamp;
  uint64_t tmstamp64 = epochs * tp->pkt_time_sampling;
  uint32_t tmstamp32 = tmstamp64;
  double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;
  double diff_rtp_ts_ns = diff_rtp_ts * tp->pkt_time / tp->pkt_time_sampling;
  double latency = fpt_delta - diff_rtp_ts_ns;
  double dpvr = latency / NS_PER_US;

  slot->meta.pkts_cnt++;

  /* calculate Delta Packet vs RTP */
  slot->meta.dpvr_min = RTE_MIN(dpvr, slot->meta.dpvr_min);
  slot->meta.dpvr_max = RTE_MAX(dpvr, slot->meta.dpvr_max);
  slot->dpvr_sum += dpvr;

  if (!slot->dpvr_first) slot->dpvr_first = dpvr;

  if (tp->prev_pkt_time[s_port]) {
    double ipt = (double)pkt_time - tp->prev_pkt_time[s_port];

    slot->ipt_sum += ipt;
    slot->meta.ipt_min = RTE_MIN(ipt, slot->meta.ipt_min);
    slot->meta.ipt_max = RTE_MAX(ipt, slot->meta.ipt_max);
  }
  tp->prev_pkt_time[s_port] = pkt_time;
}

int ra_tp_init(struct mtl_main_impl* impl, struct st_rx_audio_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx;
  struct st_rx_audio_tp* tp;
  struct st30_rx_ops* ops = &s->ops;

  tp = mt_rte_zmalloc_socket(sizeof(*tp), soc_id);
  if (!tp) {
    err("%s(%d), tp malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  s->tp = tp;

  tp->pkt_time = st30_get_packet_time(ops->ptime);
  int sample_num = st30_get_sample_num(ops->ptime, ops->sampling);
  tp->pkt_time_sampling = (double)(sample_num * 1000) * 1 / 1000;

  tp->dpvr_max_pass_narrow = (1 + 1 + 1) * tp->pkt_time / NS_PER_US; /* in us */
  tp->dpvr_max_pass_wide = (1 + 1 + 17) * tp->pkt_time / NS_PER_US;  /* in us */
  tp->tsdf_max_pass_narrow = 1 * tp->pkt_time / NS_PER_US;           /* in us */
  tp->tsdf_max_pass_wide = 17 * tp->pkt_time / NS_PER_US;            /* in us */

  ra_tp_stat_init(tp);

  tp->last_parse_time = mt_get_tsc(impl);

  info("%s(%d), Delta Packet vs RTP Pass Criteria in us, narrow %d wide %d\n", __func__,
       idx, tp->dpvr_max_pass_narrow, tp->dpvr_max_pass_wide);
  info("%s(%d), Timestamped Delay Factor Pass Criteria in us, narrow %d wide %d\n",
       __func__, idx, tp->tsdf_max_pass_narrow, tp->tsdf_max_pass_wide);

  return 0;
}

int ra_tp_uinit(struct st_rx_audio_session_impl* s) {
  if (s->tp) {
    mt_rte_free(s->tp);
    s->tp = NULL;
  }

  return 0;
}

void ra_tp_stat(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_rx_audio_tp* tp = s->tp;
  if (!tp) return;

  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    struct st_ra_tp_stat* stat = &tp->stat[s_port];
    struct st_ra_tp_slot* stat_slot = &stat->slot;

    info("%s(%d,%d), COMPLIANT NARROW %d WIDE %d FAILED %d!\n", __func__, idx, s_port,
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_NARROW],
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_WIDE],
         stat->stat_compliant_result[ST_RX_TP_COMPLIANT_FAILED]);
    float dpvr_avg = rv_tp_calculate_avg(stat_slot->meta.pkts_cnt, stat_slot->dpvr_sum);
    info("%s(%d), dpvr(us) AVG %.2f MIN %d MAX %d, pkt_cnt %u\n", __func__, idx, dpvr_avg,
         stat_slot->meta.dpvr_min, stat_slot->meta.dpvr_max, stat_slot->meta.pkts_cnt);

    /* Maximum Timestamped Delay Factor */
    float tsdf_avg = rv_tp_calculate_avg(stat->tsdf_cnt, stat->tsdf_sum);
    info("%s(%d), tsdf(us) AVG %.2f MIN %d MAX %d\n", __func__, idx, tsdf_avg,
         stat->tsdf_min, stat->tsdf_max);

    float ipt_avg = rv_tp_calculate_avg(stat_slot->meta.pkts_cnt, stat_slot->ipt_sum);
    info("%s(%d), ipt(ns) AVG %.2f MIN %d MAX %d\n", __func__, idx, ipt_avg,
         stat_slot->meta.ipt_min, stat_slot->meta.ipt_max);

    if (tp->stat_bursted_cnt[s_port]) {
      info("%s(%d), untrusted bursted cnt %u\n", __func__, idx,
           tp->stat_bursted_cnt[s_port]);
      tp->stat_bursted_cnt[s_port] = 0;
    }
  }

  ra_tp_stat_init(tp);
}

void ra_tp_slot_init(struct st_ra_tp_slot* slot) {
  memset(slot, 0, sizeof(*slot));

  slot->meta.dpvr_max = INT_MIN;
  slot->meta.dpvr_min = INT_MAX;

  slot->meta.ipt_max = INT_MIN;
  slot->meta.ipt_min = INT_MAX;
}
