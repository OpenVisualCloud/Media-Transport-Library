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

/* staging for st_frame_convert_nt(), all zero when NT convert is off */
struct st_frame_nt_scratch {
  void* buf;
  size_t size;
  _Atomic uint32_t busy; /* one thread at a time converts through buf */
};

/* whether converter can run on blocks of lines of this width */
bool st_frame_nt_supported(const struct st_frame_converter* converter, uint32_t width);
/* scratch bytes st_frame_convert_nt() needs to stage one block of dst fmt lines */
size_t st_frame_nt_scratch_size(enum st_frame_fmt fmt, uint32_t width);
/* a block of dst_fmt lines if converter can run on blocks of width, else no buf */
int st_frame_nt_scratch_init(struct st_frame_nt_scratch* s,
                             const struct st_frame_converter* converter,
                             enum st_frame_fmt dst_fmt, uint32_t width, int socket_id);
void st_frame_nt_scratch_uinit(struct st_frame_nt_scratch* s);
/* convert_func(src, dst), via s and NT stores if s is free and they fit; s may be NULL */
int st_frame_convert_nt(const struct st_frame_converter* converter,
                        struct st_frame_nt_scratch* s, struct st_frame* src,
                        struct st_frame* dst);

#endif
