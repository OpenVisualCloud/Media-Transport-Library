/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef WINDOWSENV
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
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
  /** YUV 422 planar 12bit little endian */
  CVT_FRAME_FMT_YUV422PLANAR12LE,
  /** YUV 422 packed, 3 samples on a 32-bit word, 10 bits per sample */
  CVT_FRAME_FMT_V210,
  /** YUV 422 packed, 16 bits per sample with least significant 6 paddings */
  CVT_FRAME_FMT_Y210,
  /** YUV 444 planar 10bit little endian */
  CVT_FRAME_FMT_YUV444PLANAR10LE,
  /** YUV 444 planar 12bit little endian */
  CVT_FRAME_FMT_YUV444PLANAR12LE,
  /** GBR planar 10bit little endian */
  CVT_FRAME_FMT_GBRPLANAR10LE,
  /** GBR planar 12bit little endian */
  CVT_FRAME_FMT_GBRPLANAR12LE,
  /** RFC4175 in ST2110, two YUV 422 10 bit pixel groups on 5 bytes, big endian
   */
  CVT_FRAME_FMT_YUV422RFC4175PG2BE10,
  /** RFC4175 in ST2110, two YUV 422 12 bit pixel groups on 6 bytes, big endian
   */
  CVT_FRAME_FMT_YUV422RFC4175PG2BE12,
  /** RFC4175 in ST2110, four YUV 444 10 bit pixel groups on 15 bytes, big
     endian */
  CVT_FRAME_FMT_YUV444RFC4175PG4BE10,
  /** RFC4175 in ST2110, two YUV 444 12 bit pixel groups on 9 bytes, big endian
   */
  CVT_FRAME_FMT_YUV444RFC4175PG2BE12,
  /** RFC4175 in ST2110, four RGB 10 bit pixel groups on 15 bytes, big endian */
  CVT_FRAME_FMT_RGBRFC4175PG4BE10,
  /** RFC4175 in ST2110, two RGB 12 bit pixel groups on 9 bytes, big endian */
  CVT_FRAME_FMT_RGBRFC4175PG2BE12,
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
  bool frame2field;
};

static inline void *conv_app_zmalloc(size_t sz) {
  void *p = malloc(sz);
  if (p)
    memset(p, 0x0, sz);
  return p;
}

static inline void conv_app_free(void *p) { free(p); }

int conv_app_parse_args(struct conv_app_context *ctx, int argc, char **argv);

#endif
