/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Wrapper that pulls lib/src/st2110/st_rx_video_session.c into the
 * test binary. The static helper rv_detector_calculate_fps is reached
 * through a small bridge below; --gc-sections drops the (much larger)
 * unused remainder of the TU at link time, so only externs that the
 * helper transitively touches need to be stubbed in fakes_mtl.c.
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_rx_video_session.c"

void test_calc_fps(int rtp_tm_0, int rtp_tm_1, int rtp_tm_2, int* out_fps) {
  struct st_rx_video_session_impl s;
  struct st_rx_video_detector d;
  memset(&s, 0, sizeof(s));
  memset(&d, 0, sizeof(d));
  s.idx = 0;
  d.rtp_tm[0] = rtp_tm_0;
  d.rtp_tm[1] = rtp_tm_1;
  d.rtp_tm[2] = rtp_tm_2;
  d.meta.fps = ST_FPS_MAX; /* sentinel — production leaves untouched on err */
  rv_detector_calculate_fps(&s, &d);
  *out_fps = (int)d.meta.fps;
}
