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
};

int st_frame_get_converter(enum st_frame_fmt src_fmt, enum st_frame_fmt dst_fmt,
                           struct st_frame_converter* converter);

#endif
