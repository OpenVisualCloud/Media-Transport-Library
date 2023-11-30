/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "st_rx_ebu.h"

#include "../mt_log.h"

static inline float rv_ebu_calculate_avg(uint32_t cnt, int64_t sum) {
  return cnt ? ((float)sum / cnt) : -1.0f;
}

void rv_ebu_on_packet(struct st_rx_video_session_impl* s, struct st_rv_ebu_slot* slot,
                      uint32_t rtp_tmstamp, uint64_t pkt_time, int pkt_idx) {
  struct st_rx_video_ebu* ebu = s->ebu;
  uint64_t epoch_tmstamp;
  double tvd, packet_delta_ns, trs = ebu->trs;

  if (!slot->cur_epochs) { /* the first packet */
    uint64_t epochs = (double)pkt_time / s->frame_time;
    uint64_t epoch_tmstamp = (double)epochs * s->frame_time;

    slot->cur_epochs = epochs;
    slot->rtp_tmstamp = rtp_tmstamp;
    slot->first_pkt_time = pkt_time;
    slot->fpt_to_epoch = pkt_time - epoch_tmstamp;

    uint64_t tmstamp64 = epochs * s->frame_time_sampling;
    uint32_t tmstamp32 = tmstamp64;
    double diff_rtp_ts = (double)rtp_tmstamp - tmstamp32;
    double diff_rtp_ts_ns = diff_rtp_ts * s->frame_time / s->frame_time_sampling;
    slot->latency = slot->fpt_to_epoch - diff_rtp_ts_ns;
    slot->rtp_offset = diff_rtp_ts;
    if (ebu->pre_rtp_tmstamp) {
      slot->rtp_ts_delta = rtp_tmstamp - ebu->pre_rtp_tmstamp;
    }
    ebu->pre_rtp_tmstamp = rtp_tmstamp;
  }

  epoch_tmstamp = (uint64_t)(slot->cur_epochs * s->frame_time);
  tvd = epoch_tmstamp + ebu->tr_offset;

  /* Calculate vrx */
  packet_delta_ns = (double)pkt_time - tvd;
  int32_t drained = (packet_delta_ns + trs) / trs;
  int32_t vrx_cur = slot->vrx_prev + 1 - (drained - slot->vrx_drained_prev);
  slot->vrx_sum += vrx_cur;
  slot->vrx_min = RTE_MIN(vrx_cur, slot->vrx_min);
  slot->vrx_max = RTE_MAX(vrx_cur, slot->vrx_max);
  slot->vrx_prev = vrx_cur;
  slot->vrx_drained_prev = drained;

  /* Calculate C-inst */
  int exp_cin_pkts =
      ((pkt_time - slot->first_pkt_time) / trs) * ST_EBU_CINST_DRAIN_FACTOR;
  int cinst = RTE_MAX(0, pkt_idx - exp_cin_pkts);
  slot->cinst_sum += cinst;
  slot->cinst_min = RTE_MIN(cinst, slot->cinst_min);
  slot->cinst_max = RTE_MAX(cinst, slot->cinst_max);

  /* calculate Inter-packet time */
  if (slot->prev_pkt_time) {
    double ipt = (double)pkt_time - slot->prev_pkt_time;

    slot->ipt_sum += ipt;
    slot->ipt_min = RTE_MIN(ipt, slot->ipt_min);
    slot->ipt_max = RTE_MAX(ipt, slot->ipt_max);
  }
  slot->prev_pkt_time = pkt_time;

  slot->pkt_cnt++;
}

static enum st_rv_ebu_compliant rv_ebu_compliant(struct st_rx_video_session_impl* s,
                                                 struct st_rx_video_ebu* ebu,
                                                 struct st_rv_ebu_slot* slot) {
  /* fpt check */
  if (slot->fpt_to_epoch > ebu->tr_offset) return ST_RV_EBU_COMPLIANT_FAILED;
  /* rtp ts delta check */
  int32_t sampling = s->frame_time_sampling;
  if ((slot->rtp_ts_delta < sampling) || (slot->rtp_ts_delta > (sampling + 1)))
    return ST_RV_EBU_COMPLIANT_FAILED;
  /* rtp offset check */
  if ((slot->rtp_offset < ST_EBU_RTP_OFFSET_MIN) ||
      (slot->rtp_offset > ebu->rtp_offset_max_pass))
    return ST_RV_EBU_COMPLIANT_FAILED;
  /* latency check */
  if ((slot->latency < 0) || (slot->latency > ST_EBU_LATENCY_MAX_NS))
    return ST_RV_EBU_COMPLIANT_FAILED;
  /* vrx check */
  if ((slot->vrx_min < 0) || (slot->vrx_max > ebu->vrx_full_wide_pass))
    return ST_RV_EBU_COMPLIANT_FAILED;
  /* narrow or wide */
  if (slot->cinst_max > ebu->c_max_wide_pass) return ST_RV_EBU_COMPLIANT_FAILED;
  if (slot->cinst_max > ebu->c_max_narrow_pass) return ST_RV_EBU_COMPLIANT_WIDE;
  if (slot->vrx_max > ebu->vrx_full_narrow_pass) return ST_RV_EBU_COMPLIANT_WIDE;
  return ST_RV_EBU_COMPLIANT_NARROW;
}

void rv_ebu_slot_parse_result(struct st_rx_video_session_impl* s,
                              struct st_rv_ebu_slot* slot) {
  struct st_rx_video_ebu* ebu = s->ebu;
  float cinst_avg = rv_ebu_calculate_avg(slot->pkt_cnt, slot->cinst_sum);
  float vrx_avg = rv_ebu_calculate_avg(slot->pkt_cnt, slot->vrx_sum);
  float ipt_avg = rv_ebu_calculate_avg(slot->pkt_cnt, slot->ipt_sum);

  slot->cinst_avg = cinst_avg;
  slot->vrx_avg = vrx_avg;
  slot->ipt_avg = ipt_avg;
  dbg("%s(%d), Cinst AVG %.2f MIN %d MAX %d test %s!\n", __func__, s->idx, cinst_avg,
      slot->cinst_min, slot->cinst_max, rv_ebu_cinst_result(ebu, slot));
  dbg("%s(%d), VRX AVG %.2f MIN %d MAX %d test %s!\n", __func__, s->idx, vrx_avg,
      slot->vrx_min, slot->vrx_max, rv_ebu_vrx_result(ebu, slot));
  dbg("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n", __func__, s->idx,
      ipt_avg, slot->ipt_min, slot->ipt_max);

  /* parse ebu compliant for current frame */
  enum st_rv_ebu_compliant compliant = rv_ebu_compliant(s, ebu, slot);
  slot->compliant = compliant;

  /* update stat */
  struct st_rv_ebu_stat* stat = &ebu->stat;
  struct st_rv_ebu_slot* stat_slot = &stat->slot;

  stat->stat_compliant_result[compliant]++;

  stat_slot->vrx_sum += slot->vrx_sum;
  stat_slot->vrx_min = RTE_MIN(stat_slot->vrx_min, slot->vrx_min);
  stat_slot->vrx_max = RTE_MAX(stat_slot->vrx_min, slot->vrx_max);
  stat_slot->cinst_sum += slot->cinst_sum;
  stat_slot->cinst_min = RTE_MIN(stat_slot->cinst_min, slot->cinst_min);
  stat_slot->cinst_max = RTE_MAX(stat_slot->cinst_max, slot->cinst_max);
  stat_slot->ipt_sum += slot->ipt_sum;
  stat_slot->ipt_min = RTE_MIN(stat_slot->ipt_min, slot->ipt_min);
  stat_slot->ipt_max = RTE_MAX(stat_slot->ipt_min, slot->ipt_max);
  stat_slot->pkt_cnt += slot->pkt_cnt;

  stat->stat_fpt_min = RTE_MIN(stat->stat_fpt_min, slot->fpt_to_epoch);
  stat->stat_fpt_max = RTE_MAX(stat->stat_fpt_max, slot->fpt_to_epoch);
  stat->stat_fpt_sum += slot->fpt_to_epoch;
  stat->stat_latency_min = RTE_MIN(stat->stat_latency_min, slot->latency);
  stat->stat_latency_max = RTE_MAX(stat->stat_latency_max, slot->latency);
  stat->stat_latency_sum += slot->latency;
  stat->stat_rtp_offset_min = RTE_MIN(stat->stat_rtp_offset_min, slot->rtp_offset);
  stat->stat_rtp_offset_max = RTE_MAX(stat->stat_rtp_offset_max, slot->rtp_offset);
  stat->stat_rtp_offset_sum += slot->rtp_offset;
  if (slot->rtp_ts_delta) {
    stat->stat_rtp_ts_delta_min =
        RTE_MIN(stat->stat_rtp_ts_delta_min, slot->rtp_ts_delta);
    stat->stat_rtp_ts_delta_max =
        RTE_MAX(stat->stat_rtp_ts_delta_max, slot->rtp_ts_delta);
    stat->stat_rtp_ts_delta_sum += slot->rtp_ts_delta;
  }
  stat->stat_frame_cnt++;
}

static void rv_ebu_stat_init(struct st_rx_video_ebu* ebu) {
  struct st_rv_ebu_stat* stat = &ebu->stat;

  memset(stat, 0, sizeof(*stat));
  rv_ebu_slot_init(&stat->slot);
  stat->stat_fpt_min = INT_MAX;
  stat->stat_fpt_max = INT_MIN;
  stat->stat_latency_min = INT_MAX;
  stat->stat_latency_max = INT_MIN;
  stat->stat_rtp_offset_min = INT_MAX;
  stat->stat_rtp_offset_max = INT_MIN;
  stat->stat_rtp_ts_delta_min = INT_MAX;
  stat->stat_rtp_ts_delta_max = INT_MIN;
}

void rv_ebu_stat(struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  struct st_rx_video_ebu* ebu = s->ebu;
  struct st_rv_ebu_stat* stat = &ebu->stat;
  struct st_rv_ebu_slot* stat_slot = &stat->slot;

  info("%s(%d), COMPLIANT NARROW %d WIDE %d FAILED %d!\n", __func__, idx,
       stat->stat_compliant_result[ST_RV_EBU_COMPLIANT_NARROW],
       stat->stat_compliant_result[ST_RV_EBU_COMPLIANT_WIDE],
       stat->stat_compliant_result[ST_RV_EBU_COMPLIANT_FAILED]);
  float cinst_avg = rv_ebu_calculate_avg(stat_slot->pkt_cnt, stat_slot->cinst_sum);
  float vrx_avg = rv_ebu_calculate_avg(stat_slot->pkt_cnt, stat_slot->vrx_sum);
  float ipt_avg = rv_ebu_calculate_avg(stat_slot->pkt_cnt, stat_slot->ipt_sum);
  info("%s(%d), Cinst AVG %.2f MIN %d MAX %d!\n", __func__, idx, cinst_avg,
       stat_slot->cinst_min, stat_slot->cinst_max);
  info("%s(%d), VRX AVG %.2f MIN %d MAX %d!\n", __func__, idx, vrx_avg,
       stat_slot->vrx_min, stat_slot->vrx_max);
  info("%s(%d), Inter-packet time(ns) AVG %.2f MIN %d MAX %d!\n", __func__, idx, ipt_avg,
       stat_slot->ipt_min, stat_slot->ipt_max);
  float fpt_avg = rv_ebu_calculate_avg(stat->stat_frame_cnt, stat->stat_fpt_sum);
  info("%s(%d), FPT AVG %.2f MIN %d MAX %d DIFF %d!\n", __func__, idx, fpt_avg,
       stat->stat_fpt_min, stat->stat_fpt_max, stat->stat_fpt_max - stat->stat_fpt_min);
  float latency_avg = rv_ebu_calculate_avg(stat->stat_frame_cnt, stat->stat_latency_sum);
  info("%s(%d), LATENCY AVG %.2f MIN %d MAX %d!\n", __func__, idx, latency_avg,
       stat->stat_latency_min, stat->stat_latency_max);
  float rtp_offset_avg =
      rv_ebu_calculate_avg(stat->stat_frame_cnt, stat->stat_rtp_offset_sum);
  info("%s(%d), RTP OFFSET AVG %.2f MIN %d MAX %d!\n", __func__, idx, rtp_offset_avg,
       stat->stat_rtp_offset_min, stat->stat_rtp_offset_max);
  float rtp_ts_delta_avg =
      rv_ebu_calculate_avg(stat->stat_frame_cnt, stat->stat_rtp_ts_delta_sum);
  info("%s(%d), RTP TS DELTA AVG %.2f MIN %d MAX %d!\n", __func__, idx, rtp_ts_delta_avg,
       stat->stat_rtp_ts_delta_min, stat->stat_rtp_ts_delta_max);
  rv_ebu_stat_init(ebu);
}

void rv_ebu_slot_init(struct st_rv_ebu_slot* slot) {
  memset(slot, 0, sizeof(*slot));

  slot->cinst_max = INT_MIN;
  slot->cinst_min = INT_MAX;
  slot->vrx_max = INT_MIN;
  slot->vrx_min = INT_MAX;
  slot->ipt_max = INT_MIN;
  slot->ipt_min = INT_MAX;
}

int rv_ebu_uinit(struct st_rx_video_session_impl* s) {
  if (s->ebu) {
    mt_rte_free(s->ebu);
    s->ebu = NULL;
  }

  return 0;
}

int rv_ebu_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx, ret;
  struct st_rx_video_ebu* ebu;
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

  ebu = mt_rte_zmalloc_socket(sizeof(*ebu), soc_id);
  if (!ebu) {
    err("%s(%d), ebu malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  s->ebu = ebu;

  rv_ebu_stat_init(ebu);

  double reactive = 1080.0 / 1125.0;
  if (ops->interlaced && ops->height <= 576) {
    reactive = (ops->height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }

  ebu->trs = frame_time * reactive / st20_total_pkts;
  if (!ops->interlaced) {
    ebu->tr_offset =
        ops->height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 750.0);
  } else {
    if (ops->height == 480) {
      ebu->tr_offset = frame_time * (20.0 / 525.0) * 2;
    } else if (ops->height == 576) {
      ebu->tr_offset = frame_time * (26.0 / 625.0) * 2;
    } else {
      ebu->tr_offset = frame_time * (22.0 / 1125.0) * 2;
    }
  }

  ebu->c_max_narrow_pass =
      RTE_MAX(4, (double)st20_total_pkts / (43200 * reactive * frame_time_s));
  ebu->c_max_wide_pass = RTE_MAX(16, (double)st20_total_pkts / (21600 * frame_time_s));
  ebu->vrx_full_narrow_pass = RTE_MAX(8, st20_total_pkts / (27000 * frame_time_s));
  ebu->vrx_full_wide_pass = RTE_MAX(720, st20_total_pkts / (300 * frame_time_s));
  ebu->rtp_offset_max_pass =
      ceil((ebu->tr_offset / NS_PER_S) * fps_tm.sampling_clock_rate) + 1;

  rv_ebu_stat_init(ebu);

  info("%s[%02d], trs %f tr offset %f sampling %f\n", __func__, idx, ebu->trs,
       ebu->tr_offset, s->frame_time_sampling);
  info(
      "%s[%02d], cmax_narrow %d cmax_wide %d vrx_full_narrow %d vrx_full_wide %d "
      "rtp_offset_max %d\n",
      __func__, idx, ebu->c_max_narrow_pass, ebu->c_max_wide_pass,
      ebu->vrx_full_narrow_pass, ebu->vrx_full_wide_pass, ebu->rtp_offset_max_pass);
  return 0;
}