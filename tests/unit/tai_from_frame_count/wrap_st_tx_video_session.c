/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bridges into st_tx_video_session.c. The static-inline helpers all
 * sit at the top of the file; --gc-sections drops the rest of the
 * 4985-line TU.
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_tx_video_session.c"

uint64_t test_tai(double frame_time, uint64_t frame_count) {
  struct st_tx_video_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time = frame_time;
  return tai_from_frame_count(&pacing, frame_count);
}

uint64_t test_trs_start(double frame_time, double tr_offset, double trs, uint32_t vrx,
                        uint64_t frame_count) {
  struct st_tx_video_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time = frame_time;
  pacing.tr_offset = tr_offset;
  pacing.trs = trs;
  pacing.vrx = vrx;
  return transmission_start_time(&pacing, frame_count);
}

uint64_t test_tv_rl_bps(uint16_t pkt_size, int total_pkts, int mul, int den,
                        int interlaced, int height) {
  struct st_tx_video_session_impl s;
  memset(&s, 0, sizeof(s));
  s.st20_pkt_size = pkt_size;
  s.st20_total_pkts = total_pkts;
  s.fps_tm.mul = mul;
  s.fps_tm.den = den;
  s.ops.interlaced = interlaced ? true : false;
  s.ops.height = height;
  return tv_rl_bps(&s);
}
