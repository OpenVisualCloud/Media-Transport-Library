/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_FFMPEG_TEST_COMPAT_H
#define TESTS_UNIT_FFMPEG_TEST_COMPAT_H

#include <errno.h>
#include <stdint.h>

typedef struct AVFormatContext AVFormatContext;
typedef struct AVRational {
  int num;
  int den;
} AVRational;

typedef union AVOptionDefaultVal {
  int64_t i64;
  const char* str;
} AVOptionDefaultVal;

typedef struct AVOption {
  const char* name;
  const char* help;
  int offset;
  int type;
  AVOptionDefaultVal default_val;
  double min;
  double max;
  int flags;
} AVOption;

#define AV_LOG_DEBUG 48
#define AV_LOG_INFO 32
#define AV_LOG_WARNING 24
#define AV_LOG_ERROR 16
#define AVERROR(e) (-(e))
#define AV_OPT_TYPE_STRING 5
#define AV_OPT_TYPE_INT 1
#define AV_OPT_TYPE_BOOL 18

void av_log(void* avcl, int level, const char* fmt, ...);

#endif