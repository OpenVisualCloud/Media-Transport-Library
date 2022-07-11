/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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

static const struct st_frame_fmt_desc st_frame_fmt_descs[] = {
    {
        /* ST_FRAME_FMT_YUV422PLANAR10LE */
        .fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .name = "YUV422PLANAR10LE",
    },
    {
        /* ST_FRAME_FMT_V210 */
        .fmt = ST_FRAME_FMT_V210,
        .name = "v210",
    },
    {
        /* ST_FRAME_FMT_YUV422PLANAR8 */
        .fmt = ST_FRAME_FMT_YUV422PLANAR8,
        .name = "YUV422PLANAR8",
    },
    {
        /* ST_FRAME_FMT_YUV422PACKED8 */
        .fmt = ST_FRAME_FMT_YUV422PACKED8,
        .name = "YUV422PACKED8",
    },
    {
        /* ST_FRAME_FMT_YUV422RFC4175PG2BE10 */
        .fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .name = "YUV422RFC4175PG2BE10",
    },
    {
        /* ST_FRAME_FMT_ARGB */
        .fmt = ST_FRAME_FMT_ARGB,
        .name = "ARGB",
    },
    {
        /* ST_FRAME_FMT_BGRA */
        .fmt = ST_FRAME_FMT_BGRA,
        .name = "BGRA",
    },
    {
        /* ST_FRAME_FMT_RGB8 */
        .fmt = ST_FRAME_FMT_RGB8,
        .name = "RGB8",
    },
    {
        /* ST_FRAME_FMT_JPEGXS_CODESTREAM */
        .fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM,
        .name = "JPEGXS_CODESTREAM",
    },
};

size_t st_frame_size(enum st_frame_fmt fmt, uint32_t width, uint32_t height) {
  size_t size = 0;
  size_t pixels = width * height;

  switch (fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
      size = pixels * 2 * 2; /* 10bit in two bytes */
      break;
    case ST_FRAME_FMT_V210:
      size = pixels * 8 / 3;
      break;
    case ST_FRAME_FMT_YUV422PLANAR8:
    case ST_FRAME_FMT_YUV422PACKED8:
      size = pixels * 2;
      break;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
      size = pixels * 5 / 2;
      break;
    case ST_FRAME_FMT_ARGB:
    case ST_FRAME_FMT_BGRA:
      size = pixels * 4; /* 8 bits ARGB pixel in a 32 bits */
      break;
    case ST_FRAME_FMT_RGB8:
      size = pixels * 3; /* 8 bits RGB pixel in a 24 bits */
      break;
    default:
      err("%s, invalid fmt %d\n", __func__, fmt);
      break;
  }

  return size;
}

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

const char* st_frame_fmt_name(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (fmt == st_frame_fmt_descs[i].fmt) {
      return st_frame_fmt_descs[i].name;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return "unknown";
}

uint32_t st10_tai_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate) {
  double ts = (double)tai_ns * sampling_rate / NS_PER_S;
  uint64_t tmstamp64 = ts;
  uint32_t tmstamp32 = tmstamp64;
  return tmstamp32;
}

uint64_t st10_media_clk_to_ns(uint32_t media_ts, uint32_t sampling_rate) {
  double ts = (double)media_ts * NS_PER_S / sampling_rate;
  return ts;
}

int st_draw_logo(struct st_frame_meta* frame, struct st_frame_meta* logo, uint32_t x,
                 uint32_t y) {
  if (frame->fmt != logo->fmt) {
    err("%s, mismatch fmt %d %d\n", __func__, frame->fmt, logo->fmt);
    return -EINVAL;
  }

  if (frame->fmt != ST_FRAME_FMT_YUV422RFC4175PG2BE10) {
    err("%s, err fmt %d, only ST_FRAME_FMT_YUV422RFC4175PG2BE10\n", __func__, frame->fmt);
    return -EINVAL;
  }

  if ((x + logo->width) > frame->width) {
    err("%s, err w, x %u logo width %u frame width %u\n", __func__, x, logo->width,
        frame->width);
    return -EINVAL;
  }
  if ((y + logo->height) > frame->height) {
    err("%s, err h, y %u logo height %u frame height %u\n", __func__, y, logo->height,
        frame->height);
    return -EINVAL;
  }

  size_t logo_col_size = logo->width / 2 * 5;
  for (uint32_t col = 0; col < logo->height; col++) {
    void* dst = frame->addr + (((col + y) * frame->width) + x) / 2 * 5;
    void* src = logo->addr + (col * logo->width) / 2 * 5;
    st_memcpy(dst, src, logo_col_size);
  }

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

int st22_rtp_bandwidth_bps(uint32_t total_pkts, uint16_t pkt_size, enum st_fps fps,
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

int st22_frame_bandwidth_bps(size_t frame_size, enum st_fps fps, uint64_t* bps) {
  struct st_fps_timing fps_tm;
  int ret;

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double ractive = 1080.0 / 1125.0;
  *bps = frame_size * fps_tm.mul / fps_tm.den;
  *bps = (double)*bps / ractive;
  return 0;
}

double st30_get_packet_time(enum st30_ptime ptime) {
  double packet_time_ns = 0.0; /* in nanoseconds */
  switch (ptime) {
    case ST30_PTIME_1MS:
      packet_time_ns = (double)1000000000.0 * 1 / 1000;
      break;
    case ST30_PTIME_125US:
      packet_time_ns = (double)1000000000.0 * 1 / 8000;
      break;
    case ST30_PTIME_80US:
      packet_time_ns = (double)1000000000.0 * 1 / 12500;
      break;
    default:
      err("%s, wrong ptime %d\n", __func__, ptime);
      return -EINVAL;
  }
  return packet_time_ns;
}

int st30_get_sample_size(enum st30_fmt fmt) {
  int sample_size = 0;
  switch (fmt) {
    case ST30_FMT_PCM16:
      sample_size = 2;
      break;
    case ST30_FMT_PCM24:
      sample_size = 3;
      break;
    case ST30_FMT_PCM8:
      sample_size = 1;
      break;
    case ST31_FMT_AM824:
      sample_size = 4;
      break;
    default:
      err("%s, wrong fmt %d\n", __func__, fmt);
      return -EINVAL;
  }
  return sample_size;
}

int st30_get_sample_num(enum st30_ptime ptime, enum st30_sampling sampling) {
  int samples = 0;
  switch (sampling) {
    case ST30_SAMPLING_48K:
      switch (ptime) {
        case ST30_PTIME_1MS:
          samples = 48;
          break;
        case ST30_PTIME_125US:
          samples = 6;
          break;
        case ST30_PTIME_80US:
          samples = 4;
          break;
        default:
          err("%s, wrong ptime %d\n", __func__, ptime);
          return -EINVAL;
      }
      break;
    case ST30_SAMPLING_96K:
      switch (ptime) {
        case ST30_PTIME_1MS:
          samples = 96;
          break;
        case ST30_PTIME_125US:
          samples = 12;
          break;
        case ST30_PTIME_80US:
          samples = 8;
          break;
        default:
          err("%s, wrong ptime %d\n", __func__, ptime);
          return -EINVAL;
      }
      break;
    case ST31_SAMPLING_44K:
      switch (ptime) {
        case ST31_PTIME_1_09MS:
          samples = 48;
          break;
        case ST31_PTIME_0_14MS:
          samples = 6;
          break;
        case ST31_PTIME_0_09MS:
          samples = 4;
          break;
        default:
          err("%s, wrong ptime %d\n", __func__, ptime);
          return -EINVAL;
      }
      break;
    default:
      err("%s, wrong sampling %d\n", __func__, sampling);
      return -EINVAL;
  }
  return samples;
}

static int st20_yuv422p10le_to_rfc4175_422be10_scalar(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_422_10_pg2_be* pg,
    uint32_t w, uint32_t h) {
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

int st20_yuv422p10le_to_rfc4175_422be10_simd(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint32_t w, uint32_t h,
                                             enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_yuv422p10le_to_rfc4175_422be10_avx512(y, b, r, pg, w, h);
    if (ret == 0) return 0;
    err("%s, avx512 ways failed %d\n", __func__, ret);
  }
#endif

  /* the last option */
  return st20_yuv422p10le_to_rfc4175_422be10_scalar(y, b, r, pg, w, h);
}

int st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
    st_udma_handle udma, uint16_t* y, st_iova_t y_iova, uint16_t* b, st_iova_t b_iova,
    uint16_t* r, st_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
    uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_yuv422p10le_to_rfc4175_422be10_avx512_dma(udma, y, y_iova, b, b_iova, r,
                                                         r_iova, pg, w, h);
    if (ret == 0) return 0;
    err("%s, avx512 ways failed %d\n", __func__, ret);
  }
#endif

  /* the last option */
  return st20_yuv422p10le_to_rfc4175_422be10_scalar(y, b, r, pg, w, h);
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

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

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

int st20_rfc4175_422be10_to_yuv422p10le_simd_dma(st_udma_handle udma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 st_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma(udma, pg_be, pg_be_iova, y,
                                                              b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512_dma(udma, pg_be, pg_be_iova, y, b, r,
                                                         w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_yuv422p10le_scalar(pg_be, y, b, r, w, h);
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

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

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

int st20_rfc4175_422be10_to_422le10_simd_dma(st_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             st_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum st_simd_level level) {
  struct st_dma_lender_dev* dma = udma;
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);
  ST_MAY_UNUSED(dma);

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512_vbmi_dma(dma, pg_be, pg_be_iova, pg_le,
                                                          w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512_dma(dma, pg_be, pg_be_iova, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_422le10_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_422le10_to_422be10_scalar(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                           struct st20_rfc4175_422_10_pg2_be* pg_be,
                                           uint32_t w, uint32_t h) {
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

int st20_rfc4175_422le10_to_422be10_simd(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx512(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422le10_to_422be10_scalar(pg_le, pg_be, w, h);
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

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

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

int st20_rfc4175_422be10_to_422le8_simd_dma(st_udma_handle udma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_10,
                                            st_iova_t pg_10_iova,
                                            struct st20_rfc4175_422_8_pg2_le* pg_8,
                                            uint32_t w, uint32_t h,
                                            enum st_simd_level level) {
  struct st_dma_lender_dev* dma = udma;
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);
  ST_MAY_UNUSED(dma);

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512_vbmi_dma(dma, pg_10, pg_10_iova, pg_8, w,
                                                         h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512_dma(dma, pg_10, pg_10_iova, pg_8, w, h);
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

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

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

int st20_rfc4175_422be10_to_v210_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint8_t* pg_v210, uint32_t w, uint32_t h,
                                      enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

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
  return st20_rfc4175_422be10_to_v210_scalar((uint8_t*)pg_be, pg_v210, w, h);
}

int st20_rfc4175_422be10_to_v210_simd_dma(st_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          st_iova_t pg_be_iova, uint8_t* pg_v210,
                                          uint32_t w, uint32_t h,
                                          enum st_simd_level level) {
  struct st_dma_lender_dev* dma = udma;
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);
  ST_MAY_UNUSED(dma);

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512_vbmi_dma(dma, pg_be, pg_be_iova, pg_v210, w,
                                                       h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512_dma(dma, pg_be, pg_be_iova, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_v210_scalar((uint8_t*)pg_be, pg_v210, w, h);
}

int st20_v210_to_rfc4175_422be10_scalar(uint8_t* v210, uint8_t* be, uint32_t w,
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

    be[k + 0] = (v210[j + 1] << 6) | (v210[j + 0] >> 2);
    be[k + 1] = (v210[j + 0] << 6) | ((v210[j + 2] << 2) & 0x3C) | (v210[j + 1] >> 6);
    be[k + 2] = ((v210[j + 1] << 2) & 0xF0) | ((v210[j + 3] >> 2) & 0x0F);
    be[k + 3] = (v210[j + 5] & 0x03) | ((v210[j + 2] >> 2) & 0x3C) | (v210[j + 3] << 6);
    be[k + 4] = v210[j + 4];

    be[k + 5] = (v210[j + 6] << 4) | (v210[j + 5] >> 4);
    be[k + 6] = ((v210[j + 5] << 4) & 0xC0) | (v210[j + 7] & 0x3F);
    be[k + 7] = (v210[j + 6] & 0xF0) | ((v210[j + 9] << 2) & 0x0C) | (v210[j + 8] >> 6);
    be[k + 8] = (v210[j + 8] << 2) | ((v210[j + 10] >> 2) & 0x3);
    be[k + 9] = (v210[j + 10] << 6) | (v210[j + 9] >> 2);

    be[k + 10] = (v210[j + 11] << 2) | (v210[j + 10] >> 6);
    be[k + 11] =
        ((v210[j + 10] << 2) & 0xC0) | ((v210[j + 13] << 4) & 0x30) | (v210[j + 12] >> 4);
    be[k + 12] = (v210[j + 12] << 4) | (v210[j + 14] & 0x0F);
    be[k + 13] = (v210[j + 13] & 0xFC) | ((v210[j + 15] >> 4) & 0x03);
    be[k + 14] = (v210[j + 15] << 4) | (v210[j + 14] >> 4);
  }

  return 0;
}

int st20_v210_to_rfc4175_422be10_simd(uint8_t* pg_v210,
                                      struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint32_t w, uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

  /* the last option */
  return st20_v210_to_rfc4175_422be10_scalar(pg_v210, (uint8_t*)pg_be, w, h);
}
