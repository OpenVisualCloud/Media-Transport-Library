/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_rx_video_session.c"

int test_calc_packing(int bpm, int single_line) {
  struct st_rx_video_detector d;
  memset(&d, 0, sizeof(d));
  d.bpm = bpm ? true : false;
  d.single_line = single_line ? true : false;
  rv_detector_calculate_packing(&d);
  return (int)d.meta.packing;
}
