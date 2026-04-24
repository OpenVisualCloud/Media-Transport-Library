/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Wrapper that pulls the production translation unit
 *   lib/src/st2110/st_rx_timing_parser.c
 * into the unit-test binary (with mt_log shimmed out via -include
 * applied in the meson build) and exposes a tiny C bridge so the
 * gtest C++ TU never has to drag in DPDK / MTL C headers.
 */

#include <string.h>

#include "../../../lib/src/st2110/st_rx_timing_parser.c"

/* Persistent session + tp owned by the test bridge. */
static struct {
  struct st_rx_video_session_impl s;
  struct st_rx_video_tp tp;
} g;

void tp_test_reset(void) {
  memset(&g, 0, sizeof(g));
  g.s.tp = &g.tp;
  g.s.idx = 0;
  g.s.enable_timing_parser_stat = true;

  /* Match production: stat slots are initialised so min fields hold
   * INT_MAX and max fields hold INT_MIN, otherwise the running
   * RTE_MIN/MAX comparisons clamp at zero. */
  for (int p = 0; p < MTL_SESSION_PORT_MAX; p++) {
    rv_tp_slot_init(&g.tp.stat[p].slot);
  }

  /* Pass thresholds wide enough that no compliance branch short-
   * circuits before the stat-aggregation block runs. */
  g.tp.pass.tr_offset = 1000000000;
  g.tp.pass.rtp_ts_delta_min = INT32_MIN;
  g.tp.pass.rtp_ts_delta_max = INT32_MAX;
  g.tp.pass.rtp_offset_min = INT32_MIN;
  g.tp.pass.rtp_offset_max = INT32_MAX;
  g.tp.pass.latency_min = INT32_MIN;
  g.tp.pass.latency_max = INT32_MAX;
  g.tp.pass.vrx_min = INT32_MIN;
  g.tp.pass.vrx_max_wide = INT32_MAX;
  g.tp.pass.vrx_max_narrow = INT32_MAX;
  /* "slot.cinst_min > pass.cinst_min" must NOT trip — set high. */
  g.tp.pass.cinst_min = INT32_MAX;
  g.tp.pass.cinst_max_wide = INT32_MAX;
  g.tp.pass.cinst_max_narrow = INT32_MAX;
}

void tp_test_run_frame(int32_t vrx_min, int32_t vrx_max, int32_t ipt_min, int32_t ipt_max,
                       int32_t* out_stat_vrx_max, int32_t* out_stat_vrx_min,
                       int32_t* out_stat_ipt_max, int32_t* out_stat_ipt_min) {
  struct st_rv_tp_slot slot;
  rv_tp_slot_init(&slot);
  slot.meta.vrx_min = vrx_min;
  slot.meta.vrx_max = vrx_max;
  slot.meta.ipt_min = ipt_min;
  slot.meta.ipt_max = ipt_max;
  /* Keep the cinst path benign: cinst_min stays at the INT_MAX sentinel
   * (set by rv_tp_slot_init) so the "cinst_min > pass.cinst_min" branch
   * never trips, and cinst_max stays at INT_MIN which is below all pass
   * thresholds. */
  slot.meta.pkts_cnt = 1;
  slot.meta.fpt = 0;

  rv_tp_slot_parse_result(&g.s, MTL_SESSION_PORT_P, &slot);

  struct st_rv_tp_slot* stat_slot = &g.tp.stat[MTL_SESSION_PORT_P].slot;
  if (out_stat_vrx_max) *out_stat_vrx_max = stat_slot->meta.vrx_max;
  if (out_stat_vrx_min) *out_stat_vrx_min = stat_slot->meta.vrx_min;
  if (out_stat_ipt_max) *out_stat_ipt_max = stat_slot->meta.ipt_max;
  if (out_stat_ipt_min) *out_stat_ipt_min = stat_slot->meta.ipt_min;
}
