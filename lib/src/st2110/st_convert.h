/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_FRAME_CONVERT_HEAD_H_
#define _ST_LIB_FRAME_CONVERT_HEAD_H_

#include <st_convert_api.h>
#include <st_pipeline_api.h>

struct st_frame_converter {
  enum st_frame_fmt src_fmt;
  enum st_frame_fmt dst_fmt;
  int (*convert_func)(struct st_frame* src, struct st_frame* dst);
  uint8_t nt_width_align; /* 0: no NT path, else st_frame_nt_supported() widths */
};

int st_frame_get_converter(enum st_frame_fmt src_fmt, enum st_frame_fmt dst_fmt,
                           struct st_frame_converter* converter);

/* whether converter can run on blocks of lines of this width */
bool st_frame_nt_supported(const struct st_frame_converter* converter, uint32_t width);
/* scratch bytes st_frame_convert_nt() needs to stage one block of dst fmt lines */
size_t st_frame_nt_scratch_size(enum st_frame_fmt fmt, uint32_t width);
/* convert_func(src, dst), via a 16-byte aligned scratch and NT stores if they fit */
int st_frame_convert_nt(const struct st_frame_converter* converter, struct st_frame* src,
                        struct st_frame* dst, void* scratch, size_t scratch_size);

#endif
