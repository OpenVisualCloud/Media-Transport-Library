/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_FMT_HEAD_H_
#define _ST_LIB_FMT_HEAD_H_

#include <st_convert_api.h>
#include <st_pipeline_api.h>

struct st_fps_timing {
  enum st_fps fps;
  int sampling_clock_rate; /* 90k of sampling clock rate */
  int mul;                 /* 60000 for ST_FPS_P59_94 */
  int den;                 /* 1001 for ST_FPS_P59_94 */
  double frame_rate;       /* fps number */
};

struct st_frame_fmt_desc {
  enum st_frame_fmt fmt;
  char* name;
};

int st_get_fps_timing(enum st_fps fps, struct st_fps_timing* fps_tm);

int st22_rtp_bandwidth_bps(uint32_t total_pkts, uint16_t pkt_size, enum st_fps fps,
                           uint64_t* bps);

int st22_frame_bandwidth_bps(size_t frame_size, enum st_fps fps, uint64_t* bps);

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
