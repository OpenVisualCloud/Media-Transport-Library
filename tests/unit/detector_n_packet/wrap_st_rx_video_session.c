/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_rx_video_session.c"

void test_calc_n_packet(int p0, int p1, int p2, int* out_pkt_per_frame) {
  struct st_rx_video_session_impl s;
  struct st_rx_video_detector d;
  memset(&s, 0, sizeof(s));
  memset(&d, 0, sizeof(d));
  d.pkt_num[0] = p0;
  d.pkt_num[1] = p1;
  d.pkt_num[2] = p2;
  d.pkt_per_frame = -1; /* sentinel — production leaves untouched on err */
  rv_detector_calculate_n_packet(&s, &d);
  *out_pkt_per_frame = d.pkt_per_frame;
}
