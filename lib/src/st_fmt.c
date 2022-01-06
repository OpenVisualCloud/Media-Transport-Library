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

#include "st_fmt.h"

#include "st_log.h"
#include "st_main.h"

static const struct st20_pgroup st20_pgroups[] = {
    {
        /* ST20_FMT_YUV_422_8BIT */
        .fmt = ST20_FMT_YUV_422_8BIT,
        .size = 4,
        .coverage = 2,
    },
    {
        /* ST20_FMT_YUV_422_10BIT */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .size = 5,
        .coverage = 2,
    },
    {
        /* ST20_FMT_YUV_422_12BIT */
        .fmt = ST20_FMT_YUV_422_12BIT,
        .size = 6,
        .coverage = 2,
    },
    {
        /* ST20_FMT_YUV_422_16BIT */
        .fmt = ST20_FMT_YUV_422_16BIT,
        .size = 8,
        .coverage = 2,
    },
    {
        /* ST20_FMT_YUV_420_8BIT */
        .fmt = ST20_FMT_YUV_420_8BIT,
        .size = 6,
        .coverage = 4,
    },
    {
        /* ST20_FMT_YUV_420_10BIT */
        .fmt = ST20_FMT_YUV_420_10BIT,
        .size = 15,
        .coverage = 8,
    },
    {
        /* ST20_FMT_YUV_420_12BIT */
        .fmt = ST20_FMT_YUV_420_12BIT,
        .size = 9,
        .coverage = 4,
    },
    {
        /* ST20_FMT_RGB_8BIT */
        .fmt = ST20_FMT_RGB_8BIT,
        .size = 3,
        .coverage = 1,
    },
    {
        /* ST20_FMT_RGB_10BIT */
        .fmt = ST20_FMT_RGB_10BIT,
        .size = 15,
        .coverage = 4,
    },
    {
        /* ST20_FMT_RGB_12BIT */
        .fmt = ST20_FMT_RGB_12BIT,
        .size = 9,
        .coverage = 2,
    },
    {
        /* ST20_FMT_RGB_16BIT */
        .fmt = ST20_FMT_RGB_16BIT,
        .size = 6,
        .coverage = 1,
    },

};

static const struct st_fps_timing st_fps_timings[] = {
    {
        /* ST_FPS_P59_94 */
        .fps = ST_FPS_P59_94,
        .sampling_clock_rate = 90 * 1000,
        .mul = 60000,
        .den = 1001,
        .frame_rate = 59.94,
    },
    {
        /* ST_FPS_P50 */
        .fps = ST_FPS_P50,
        .sampling_clock_rate = 90 * 1000,
        .mul = 50,
        .den = 1,
        .frame_rate = 50,
    },
    {
        /* ST_FPS_P29_97 */
        .fps = ST_FPS_P29_97,
        .sampling_clock_rate = 90 * 1000,
        .mul = 30000,
        .den = 1001,
        .frame_rate = 29.97,
    },
};

int st20_get_pgroup(enum st20_fmt fmt, struct st20_pgroup* pg) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st20_pgroups); i++) {
    if (fmt == st20_pgroups[i].fmt) {
      *pg = st20_pgroups[i];
      return 0;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return -EINVAL;
}

int st_get_fps_timing(enum st_fps fps, struct st_fps_timing* fps_tm) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st_fps_timings); i++) {
    if (fps == st_fps_timings[i].fps) {
      *fps_tm = st_fps_timings[i];
      return 0;
    }
  }

  err("%s, invalid fps %d\n", __func__, fps);
  return -EINVAL;
}

double st_frame_rate(enum st_fps fps) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st_fps_timings); i++) {
    if (fps == st_fps_timings[i].fps) {
      return st_fps_timings[i].frame_rate;
    }
  }

  err("%s, invalid fps %d\n", __func__, fps);
  return 0;
}

int st20_get_bandwidth_bps(int width, int height, enum st20_fmt fmt, enum st_fps fps,
                           uint64_t* bps) {
  struct st20_pgroup pg;
  struct st_fps_timing fps_tm;
  int ret;

  ret = st20_get_pgroup(fmt, &pg);
  if (ret < 0) return ret;

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double ractive = 1080.0 / 1125.0;
  *bps = (uint64_t)width * height * 8 * pg.size / pg.coverage * fps_tm.mul / fps_tm.den;
  *bps = (double)*bps / ractive;
  return 0;
}

int st30_get_sample_size(enum st30_fmt fmt, uint16_t c, enum st30_sampling s) {
  int pcm_size = 2;
  switch (fmt) {
    case ST30_FMT_PCM16:
      pcm_size = 2;
      break;
    case ST30_FMT_PCM24:
      pcm_size = 3;
      break;
    case ST30_FMT_PCM8:
      pcm_size = 1;
      break;
    default:
      err("%s, wrong fmt %d\n", __func__, fmt);
      return -EINVAL;
  }
  int sample_size = (s == ST30_SAMPLING_48K) ? 48 : 96;
  return pcm_size * c * sample_size;
}