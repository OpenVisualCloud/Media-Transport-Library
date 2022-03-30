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

#ifndef _ST_LIB_FMT_HEAD_H_
#define _ST_LIB_FMT_HEAD_H_

#include <st_convert_api.h>

struct st_fps_timing {
  enum st_fps fps;
  int sampling_clock_rate; /* 90k of sampling clock rate */
  int mul;                 /* 60000 for ST_FPS_P59_94 */
  int den;                 /* 1001 for ST_FPS_P59_94 */
  double frame_rate;       /* fps number */
};

int st_get_fps_timing(enum st_fps fps, struct st_fps_timing* fps_tm);

int st22_get_bandwidth_bps(uint32_t total_pkts, uint16_t pkt_size, enum st_fps fps,
                           uint64_t* bps);

static inline void st20_unpack_pg2be_422le10(struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint16_t* cb00, uint16_t* y00,
                                             uint16_t* cr00, uint16_t* y01) {
  uint16_t cb, y0, cr, y1;

  cb = (pg->Cb00 << 2) + pg->Cb00_;
  y0 = (pg->Y00 << 4) + pg->Y00_;
  cr = (pg->Cr00 << 6) + pg->Cr00_;
  y1 = (pg->Y01 << 8) + pg->Y01_;

  *cb00 = cb;
  *y00 = y0;
  *cr00 = cr;
  *y01 = y1;
}

#endif
