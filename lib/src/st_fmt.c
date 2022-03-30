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

#ifdef ST_HAS_AVX512
#include "st_avx512.h"
#endif

#ifdef ST_HAS_AVX512_VBMI2
#include "st_avx512_vbmi.h"
#endif

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
    {
        /* ST20_FMT_YUV_444_8BIT */
        .fmt = ST20_FMT_YUV_444_8BIT,
        .size = 3,
        .coverage = 1,
    },
    {
        /* ST20_FMT_YUV_444_10BIT */
        .fmt = ST20_FMT_YUV_444_10BIT,
        .size = 15,
        .coverage = 4,
    },
    {
        /* ST20_FMT_YUV_444_12BIT */
        .fmt = ST20_FMT_YUV_444_12BIT,
        .size = 9,
        .coverage = 2,
    },
    {
        /* ST20_FMT_YUV_444_16BIT */
        .fmt = ST20_FMT_YUV_444_16BIT,
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
    {
        /* ST_FPS_P25 */
        .fps = ST_FPS_P25,
        .sampling_clock_rate = 90 * 1000,
        .mul = 25,
        .den = 1,
        .frame_rate = 25,
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

int st22_get_bandwidth_bps(uint32_t total_pkts, uint16_t pkt_size, enum st_fps fps,
                           uint64_t* bps) {
  struct st_fps_timing fps_tm;
  int ret;

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double ractive = 1080.0 / 1125.0;
  *bps = (uint64_t)total_pkts * pkt_size * fps_tm.mul / fps_tm.den;
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

int st20_yuv422p10le_to_rfc4175_422be10(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
                                        uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = *b++;
    y0 = *y++;
    cr = *r++;
    y1 = *y++;

    pg->Cb00 = cb >> 2;
    pg->Cb00_ = cb;
    pg->Y00 = y0 >> 4;
    pg->Y00_ = y0;
    pg->Cr00 = cr >> 6;
    pg->Cr00_ = cr;
    pg->Y01 = y1 >> 8;
    pg->Y01_ = y1;

    pg++;
  }

  return 0;
}

static int st20_rfc4175_422be10_to_yuv422p10le_scalar(
    struct st20_rfc4175_422_10_pg2_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = (pg->Cb00 << 2) + pg->Cb00_;
    y0 = (pg->Y00 << 4) + pg->Y00_;
    cr = (pg->Cr00 << 6) + pg->Cr00_;
    y1 = (pg->Y01 << 8) + pg->Y01_;

    *b++ = cb;
    *y++ = y0;
    *r++ = cr;
    *y++ = y1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_422be10_to_yuv422p10le_simd(struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint16_t* y, uint16_t* b, uint16_t* r,
                                             uint32_t w, uint32_t h,
                                             enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_yuv422p10le_scalar(pg, y, b, r, w, h);
}

int st20_yuv422p10le_to_rfc4175_422le10(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_10_pg2_le* pg, uint32_t w,
                                        uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = *b++;
    y0 = *y++;
    cr = *r++;
    y1 = *y++;

    pg->Cb00 = cb;
    pg->Cb00_ = cb >> 8;
    pg->Y00 = y0;
    pg->Y00_ = y0 >> 6;
    pg->Cr00 = cr;
    pg->Cr00_ = cr >> 4;
    pg->Y01 = y1;
    pg->Y01_ = y1 >> 2;

    pg++;
  }

  return 0;
}

int st20_rfc4175_422le10_to_yuv422p10le(struct st20_rfc4175_422_10_pg2_le* pg,
                                        uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
                                        uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = pg->Cb00 + (pg->Cb00_ << 8);
    y0 = pg->Y00 + (pg->Y00_ << 6);
    cr = pg->Cr00 + (pg->Cr00_ << 4);
    y1 = pg->Y01 + (pg->Y01_ << 2);

    *b++ = cb;
    *y++ = y0;
    *r++ = cr;
    *y++ = y1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le10_scalar(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                           struct st20_rfc4175_422_10_pg2_le* pg_le,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2;
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = (pg_be->Cb00 << 2) + pg_be->Cb00_;
    y0 = (pg_be->Y00 << 4) + pg_be->Y00_;
    cr = (pg_be->Cr00 << 6) + pg_be->Cr00_;
    y1 = (pg_be->Y01 << 8) + pg_be->Y01_;

    pg_le->Cb00 = cb;
    pg_le->Cb00_ = cb >> 8;
    pg_le->Y00 = y0;
    pg_le->Y00_ = y0 >> 6;
    pg_le->Cr00 = cr;
    pg_le->Cr00_ = cr >> 4;
    pg_le->Y01 = y1;
    pg_le->Y01_ = y1 >> 2;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le10_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512_vbmi(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_422le10_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_422le10_to_422be10(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                    struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
                                    uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = pg_le->Cb00 + (pg_le->Cb00_ << 8);
    y0 = pg_le->Y00 + (pg_le->Y00_ << 6);
    cr = pg_le->Cr00 + (pg_le->Cr00_ << 4);
    y1 = pg_le->Y01 + (pg_le->Y01_ << 2);

    pg_be->Cb00 = cb >> 2;
    pg_be->Cb00_ = cb;
    pg_be->Y00 = y0 >> 4;
    pg_be->Y00_ = y0;
    pg_be->Cr00 = cr >> 6;
    pg_be->Cr00_ = cr;
    pg_be->Y01 = y1 >> 8;
    pg_be->Y01_ = y1;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le8_scalar(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                          struct st20_rfc4175_422_8_pg2_le* pg_8,
                                          uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2;

  for (uint32_t i = 0; i < cnt; i++) {
    pg_8[i].Cb00 = pg_10[i].Cb00;
    pg_8[i].Y00 = (pg_10[i].Y00 << 2) + (pg_10[i].Y00_ >> 2);
    pg_8[i].Cr00 = (pg_10[i].Cr00 << 4) + (pg_10[i].Cr00_ >> 2);
    pg_8[i].Y01 = (pg_10[i].Y01 << 6) + (pg_10[i].Y01_ >> 2);
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le8_simd(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                        struct st20_rfc4175_422_8_pg2_le* pg_8,
                                        uint32_t w, uint32_t h,
                                        enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512_vbmi(pg_10, pg_8, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512(pg_10, pg_8, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_422le8_scalar(pg_10, pg_8, w, h);
}

int st20_rfc4175_422le10_to_v210_scalar(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                        uint32_t h) {
  uint32_t pg_count = w * h / 2;
  if (pg_count % 3 != 0) {
    err("%s, invalid pg_count %d, pixel group number must be multiple of 3!\n", __func__,
        pg_count);
    return -EINVAL;
  }

  uint32_t batch = pg_count / 3;
  for (uint32_t i = 0; i < batch; i++) {
    int j = i * 15;
    int k = i * 16;

    pg_v210[k] = pg_le[j];
    pg_v210[k + 1] = pg_le[j + 1];
    pg_v210[k + 2] = pg_le[j + 2];
    pg_v210[k + 3] = pg_le[j + 3] & 0x3F;

    pg_v210[k + 4] = (pg_le[j + 3] >> 6) | (pg_le[j + 4] << 2);
    pg_v210[k + 5] = (pg_le[j + 4] >> 6) | (pg_le[j + 5] << 2);
    pg_v210[k + 6] = (pg_le[j + 5] >> 6) | (pg_le[j + 6] << 2);
    pg_v210[k + 7] = ((pg_le[j + 6] >> 6) | (pg_le[j + 7] << 2)) & 0x3F;

    pg_v210[k + 8] = (pg_le[j + 7] >> 4) | (pg_le[j + 8] << 4);
    pg_v210[k + 9] = (pg_le[j + 8] >> 4) | (pg_le[j + 9] << 4);
    pg_v210[k + 10] = (pg_le[j + 9] >> 4) | (pg_le[j + 10] << 4);
    pg_v210[k + 11] = ((pg_le[j + 10] >> 4) | (pg_le[j + 11] << 4)) & 0x3F;

    pg_v210[k + 12] = (pg_le[j + 11] >> 2) | (pg_le[j + 12] << 6);
    pg_v210[k + 13] = (pg_le[j + 12] >> 2) | (pg_le[j + 13] << 6);
    pg_v210[k + 14] = (pg_le[j + 13] >> 2) | (pg_le[j + 14] << 6);
    pg_v210[k + 15] = pg_le[j + 14] >> 2;
  }

  return 0;
}

int st20_rfc4175_422le10_to_v210_simd(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                      uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422le10_to_v210_avx512_vbmi(pg_le, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_v210_avx512(pg_le, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422le10_to_v210_scalar(pg_le, pg_v210, w, h);
}

int st20_v210_to_rfc4175_422le10(uint8_t* pg_v210, uint8_t* pg_le, uint32_t w,
                                 uint32_t h) {
  uint32_t pg_count = w * h / 2;
  if (pg_count % 3 != 0) {
    err("%s, invalid pg_count %d, pixel group number must be multiple of 3!\n", __func__,
        pg_count);
    return -EINVAL;
  }

  uint32_t batch = pg_count / 3;
  for (uint32_t i = 0; i < batch; i++) {
    int j = i * 16;
    int k = i * 15;

    pg_le[k] = pg_v210[j];
    pg_le[k + 1] = pg_v210[j + 1];
    pg_le[k + 2] = pg_v210[j + 2];
    pg_le[k + 3] = pg_v210[j + 3] | (pg_v210[j + 4] << 6);
    pg_le[k + 4] = (pg_v210[j + 5] << 6) | (pg_v210[j + 4] >> 2);

    pg_le[k + 5] = (pg_v210[j + 6] << 6) | (pg_v210[j + 5] >> 2);
    pg_le[k + 6] = (pg_v210[j + 7] << 6) | (pg_v210[j + 6] >> 2);
    pg_le[k + 7] = (pg_v210[j + 8] << 4) | (pg_v210[j + 7] >> 2);
    pg_le[k + 8] = (pg_v210[j + 9] << 4) | (pg_v210[j + 8] >> 4);
    pg_le[k + 9] = (pg_v210[j + 10] << 4) | (pg_v210[j + 9] >> 4);

    pg_le[k + 10] = (pg_v210[j + 11] << 4) | (pg_v210[j + 10] >> 4);
    pg_le[k + 11] = (pg_v210[j + 12] << 2) | (pg_v210[j + 11] >> 4);
    pg_le[k + 12] = (pg_v210[j + 13] << 2) | (pg_v210[j + 12] >> 6);
    pg_le[k + 13] = (pg_v210[j + 14] << 2) | (pg_v210[j + 13] >> 6);
    pg_le[k + 14] = (pg_v210[j + 15] << 2) | (pg_v210[j + 14] >> 6);
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_scalar(uint8_t* pg_be, uint8_t* pg_v210, uint32_t w,
                                        uint32_t h) {
  uint32_t pg_count = w * h / 2;
  if (pg_count % 3 != 0) {
    err("%s, invalid pg_count %d, pixel group number must be multiple of 3!\n", __func__,
        pg_count);
    return -EINVAL;
  }

  uint32_t batch = pg_count / 3;
  for (uint32_t i = 0; i < batch; i++) {
    int j = i * 15;
    int k = i * 16;

    pg_v210[k] = pg_be[j] << 2 | pg_be[j + 1] >> 6;
    pg_v210[k + 1] = pg_be[j] >> 6 | pg_be[j + 1] << 6 | ((pg_be[j + 2] >> 2) & 0x3C);
    pg_v210[k + 2] = ((pg_be[j + 1] >> 2) & 0x0F) | ((pg_be[j + 3] << 2) & 0xF0);
    pg_v210[k + 3] = (pg_be[j + 2] << 2 | pg_be[j + 3] >> 6) & 0x3F;

    pg_v210[k + 4] = pg_be[j + 4];
    pg_v210[k + 5] =
        (pg_be[j + 5] << 4) | ((pg_be[j + 6] >> 4) & 0x0C) | (pg_be[j + 3] & 0x03);
    pg_v210[k + 6] = (pg_be[j + 5] >> 4) | (pg_be[j + 7] & 0xF0);
    pg_v210[k + 7] = (pg_be[j + 6]) & 0x3F;

    pg_v210[k + 8] = (pg_be[j + 7] << 6) | (pg_be[j + 8] >> 2);
    pg_v210[k + 9] = ((pg_be[j + 7] >> 2) & 0x03) | (pg_be[j + 9] << 2);
    pg_v210[k + 10] = ((pg_be[j + 8] << 2) & 0x0C) | (pg_be[j + 9] >> 6) |
                      (pg_be[j + 10] << 6) | ((pg_be[j + 11] >> 2) & 0x30);
    pg_v210[k + 11] = (pg_be[j + 10] >> 2);

    pg_v210[k + 12] = (pg_be[j + 12] >> 4) | (pg_be[j + 11] << 4);
    pg_v210[k + 13] = ((pg_be[j + 11] >> 4) & 0x03) | (pg_be[j + 13] & 0xFC);
    pg_v210[k + 14] = (pg_be[j + 12] & 0x0F) | (pg_be[j + 14] << 4);
    pg_v210[k + 15] = ((pg_be[j + 14] >> 4) | (pg_be[j + 13] << 4)) & 0x3F;
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_simd(uint8_t* pg_be, uint8_t* pg_v210, uint32_t w,
                                      uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512_vbmi(pg_be, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512(pg_be, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_v210_scalar(pg_be, pg_v210, w, h);
}