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
#ifndef WINDOWSENV
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef _CONV_APP_BASE_HEAD_H_
#define _CONV_APP_BASE_HEAD_H_

#define MAX_FILE_NAME_LEN 128

enum cvt_frame_fmt {
  /** YUV 422 planar 10bit little endian */
  CVT_FRAME_FMT_YUV422PLANAR10LE = 0,
  /** YUV 422 packed, 3 samples on a 32-bit word, 10 bits per sample */
  CVT_FRAME_FMT_V210,
  /** RFC4175 in ST2110, two YUV 422 10 bit pixel gruops on 5 bytes, big endian */
  CVT_FRAME_FMT_YUV422RFC4175PG2BE10,
  /** max value of this enum */
  CVT_FRAME_FMT_MAX,
};

struct conv_app_context {
  int w;
  int h;
  enum cvt_frame_fmt fmt_in;
  enum cvt_frame_fmt fmt_out;
  char file_in[MAX_FILE_NAME_LEN];
  char file_out[MAX_FILE_NAME_LEN];
};

static inline void* conv_app_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void conv_app_free(void* p) { free(p); }

int conv_app_parse_args(struct conv_app_context* ctx, int argc, char** argv);

#endif
