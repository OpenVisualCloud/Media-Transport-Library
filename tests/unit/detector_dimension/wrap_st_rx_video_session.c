/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bridge for rv_detector_calculate_dimension. Linker --gc-sections
 * drops the unused remainder of st_rx_video_session.c so we don't
 * need to stub the file's many DPDK / libmtl externs.
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_rx_video_session.c"

void test_calc_dim(int max_line_num, int interlaced, uint32_t* out_w, uint32_t* out_h) {
  struct st_rx_video_session_impl s;
  struct st_rx_video_detector d;
  memset(&s, 0, sizeof(s));
  memset(&d, 0, sizeof(d));
  d.meta.interlaced = interlaced ? true : false;
  rv_detector_calculate_dimension(&s, &d, max_line_num);
  *out_w = d.meta.width;
  *out_h = d.meta.height;
}
