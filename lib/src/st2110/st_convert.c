/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_convert.h"

#include "../mt_log.h"
#include "st_main.h"

#ifdef MTL_HAS_AVX2
#include "st_avx2.h"
#endif

#ifdef MTL_HAS_AVX512
#include "st_avx512.h"
#endif

#ifdef MTL_HAS_AVX512_VBMI2
#include "st_avx512_vbmi.h"
#endif

static bool has_lines_padding(struct st_frame* src, struct st_frame* dst) {
  int planes = 0;

  planes = st_frame_fmt_planes(src->fmt);
  for (int plane = 0; plane < planes; plane++) {
    if (src->linesize[plane] > st_frame_least_linesize(src->fmt, src->width, plane))
      return true;
  }

  planes = st_frame_fmt_planes(dst->fmt);
  for (int plane = 0; plane < planes; plane++) {
    if (dst->linesize[plane] > st_frame_least_linesize(dst->fmt, dst->width, plane))
      return true;
  }

  return false;
}

static int convert_rfc4175_422be10_to_yuv422p10le(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    y = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_422be10_to_yuv422p10le(be10, y, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      y = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_422be10_to_yuv422p10le(be10, y, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_422be10_to_422le8(struct st_frame* src, struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  struct st20_rfc4175_422_8_pg2_le* le8 = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    le8 = dst->addr[0];
    ret = st20_rfc4175_422be10_to_422le8(be10, le8, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      le8 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_rfc4175_422be10_to_422le8(be10, le8, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_422be10_to_v210(struct st_frame* src, struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint8_t* v210 = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    v210 = dst->addr[0];
    ret = st20_rfc4175_422be10_to_v210(be10, v210, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      v210 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_rfc4175_422be10_to_v210(be10, v210, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_422be10_to_y210(struct st_frame* src, struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint16_t* y210 = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    y210 = dst->addr[0];
    ret = st20_rfc4175_422be10_to_y210(be10, y210, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      y210 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_rfc4175_422be10_to_y210(be10, y210, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_422be12_to_yuv422p12le(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_12_pg2_be* be12 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = src->addr[0];
    y = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_422be12_to_yuv422p12le(be12, y, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = src->addr[0] + src->linesize[0] * line;
      y = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_422be12_to_yuv422p12le(be12, y, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_444be10_to_yuv444p10le(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_10_pg4_be* be10 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    y = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_444be10_to_yuv444p10le(be10, y, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      y = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_444be10_to_yuv444p10le(be10, y, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_444be10_to_gbrp10le(struct st_frame* src,
                                               struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_10_pg4_be* be10 = NULL;
  uint16_t* g = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = src->addr[0];
    g = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_444be10_to_gbrp10le(be10, g, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = src->addr[0] + src->linesize[0] * line;
      g = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_444be10_to_gbrp10le(be10, g, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_444be12_to_yuv444p12le(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_12_pg2_be* be12 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = src->addr[0];
    y = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_444be12_to_yuv444p12le(be12, y, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = src->addr[0] + src->linesize[0] * line;
      y = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_444be12_to_yuv444p12le(be12, y, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_rfc4175_444be12_to_gbrp12le(struct st_frame* src,
                                               struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_12_pg2_be* be12 = NULL;
  uint16_t* g = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = src->addr[0];
    g = dst->addr[0];
    b = dst->addr[1];
    r = dst->addr[2];
    ret = st20_rfc4175_444be12_to_gbrp12le(be12, g, b, r, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = src->addr[0] + src->linesize[0] * line;
      g = dst->addr[0] + dst->linesize[0] * line;
      b = dst->addr[1] + dst->linesize[1] * line;
      r = dst->addr[2] + dst->linesize[2] * line;
      ret = st20_rfc4175_444be12_to_gbrp12le(be12, g, b, r, dst->width, 1);
    }
  }
  return ret;
}

static int convert_yuv422p10le_to_rfc4175_422be10(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    y = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    be10 = dst->addr[0];
    ret = st20_yuv422p10le_to_rfc4175_422be10(y, b, r, be10, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      y = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      be10 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_yuv422p10le_to_rfc4175_422be10(y, b, r, be10, dst->width, 1);
    }
  }
  return ret;
}

static int convert_v210_to_rfc4175_422be10(struct st_frame* src, struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint8_t* v210 = NULL;
  if (!has_lines_padding(src, dst)) {
    v210 = src->addr[0];
    be10 = dst->addr[0];
    ret = st20_v210_to_rfc4175_422be10(v210, be10, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      v210 = src->addr[0] + src->linesize[0] * line;
      be10 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_v210_to_rfc4175_422be10(v210, be10, dst->width, 1);
    }
  }
  return ret;
}

static int convert_y210_to_rfc4175_422be10(struct st_frame* src, struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* be10 = NULL;
  uint16_t* y210 = NULL;
  if (!has_lines_padding(src, dst)) {
    y210 = src->addr[0];
    be10 = dst->addr[0];
    ret = st20_y210_to_rfc4175_422be10(y210, be10, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      y210 = src->addr[0] + src->linesize[0] * line;
      be10 = dst->addr[0] + dst->linesize[0] * line;
      ret = st20_y210_to_rfc4175_422be10(y210, be10, dst->width, 1);
    }
  }
  return ret;
}

static int convert_yuv422p12le_to_rfc4175_422be12(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_422_12_pg2_be* be12 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = dst->addr[0];
    y = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    ret = st20_yuv422p12le_to_rfc4175_422be12(y, b, r, be12, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = dst->addr[0] + dst->linesize[0] * line;
      y = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      ret = st20_yuv422p12le_to_rfc4175_422be12(y, b, r, be12, dst->width, 1);
    }
  }
  return ret;
}

static int convert_yuv444p10le_to_rfc4175_444be10(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_10_pg4_be* be10 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = dst->addr[0];
    y = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    ret = st20_yuv444p10le_to_rfc4175_444be10(y, b, r, be10, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = dst->addr[0] + dst->linesize[0] * line;
      y = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      ret = st20_yuv444p10le_to_rfc4175_444be10(y, b, r, be10, dst->width, 1);
    }
  }
  return ret;
}

static int convert_gbrp10le_to_rfc4175_444be10(struct st_frame* src,
                                               struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_10_pg4_be* be10 = NULL;
  uint16_t* g = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be10 = dst->addr[0];
    g = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    ret = st20_gbrp10le_to_rfc4175_444be10(g, b, r, be10, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be10 = dst->addr[0] + dst->linesize[0] * line;
      g = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      ret = st20_gbrp10le_to_rfc4175_444be10(g, b, r, be10, dst->width, 1);
    }
  }
  return ret;
}

static int convert_yuv444p12le_to_rfc4175_444be12(struct st_frame* src,
                                                  struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_12_pg2_be* be12 = NULL;
  uint16_t* y = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = dst->addr[0];
    y = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    ret = st20_yuv444p12le_to_rfc4175_444be12(y, b, r, be12, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = dst->addr[0] + dst->linesize[0] * line;
      y = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      ret = st20_yuv444p12le_to_rfc4175_444be12(y, b, r, be12, dst->width, 1);
    }
  }
  return ret;
}

static int convert_gbrp12le_to_rfc4175_444be12(struct st_frame* src,
                                               struct st_frame* dst) {
  int ret = 0;
  struct st20_rfc4175_444_12_pg2_be* be12 = NULL;
  uint16_t* g = NULL;
  uint16_t* b = NULL;
  uint16_t* r = NULL;
  if (!has_lines_padding(src, dst)) {
    be12 = dst->addr[0];
    g = src->addr[0];
    b = src->addr[1];
    r = src->addr[2];
    ret = st20_gbrp12le_to_rfc4175_444be12(g, b, r, be12, dst->width, dst->height);
  } else {
    for (uint32_t line = 0; line < dst->height; line++) {
      be12 = dst->addr[0] + dst->linesize[0] * line;
      g = src->addr[0] + src->linesize[0] * line;
      b = src->addr[1] + src->linesize[1] * line;
      r = src->addr[2] + src->linesize[2] * line;
      ret = st20_gbrp12le_to_rfc4175_444be12(g, b, r, be12, dst->width, 1);
    }
  }
  return ret;
}

static const struct st_frame_converter converters[] = {
    {
        .src_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .dst_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .convert_func = convert_rfc4175_422be10_to_yuv422p10le,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .dst_fmt = ST_FRAME_FMT_UYVY,
        .convert_func = convert_rfc4175_422be10_to_422le8,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .dst_fmt = ST_FRAME_FMT_V210,
        .convert_func = convert_rfc4175_422be10_to_v210,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .dst_fmt = ST_FRAME_FMT_Y210,
        .convert_func = convert_rfc4175_422be10_to_y210,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE12,
        .dst_fmt = ST_FRAME_FMT_YUV422PLANAR12LE,
        .convert_func = convert_rfc4175_422be12_to_yuv422p12le,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV444RFC4175PG4BE10,
        .dst_fmt = ST_FRAME_FMT_YUV444PLANAR10LE,
        .convert_func = convert_rfc4175_444be10_to_yuv444p10le,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV444RFC4175PG2BE12,
        .dst_fmt = ST_FRAME_FMT_YUV444PLANAR12LE,
        .convert_func = convert_rfc4175_444be12_to_yuv444p12le,
    },
    {
        .src_fmt = ST_FRAME_FMT_RGBRFC4175PG4BE10,
        .dst_fmt = ST_FRAME_FMT_GBRPLANAR10LE,
        .convert_func = convert_rfc4175_444be10_to_gbrp10le,
    },
    {
        .src_fmt = ST_FRAME_FMT_RGBRFC4175PG2BE12,
        .dst_fmt = ST_FRAME_FMT_GBRPLANAR12LE,
        .convert_func = convert_rfc4175_444be12_to_gbrp12le,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .dst_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .convert_func = convert_yuv422p10le_to_rfc4175_422be10,
    },
    {
        .src_fmt = ST_FRAME_FMT_V210,
        .dst_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .convert_func = convert_v210_to_rfc4175_422be10,
    },
    {
        .src_fmt = ST_FRAME_FMT_Y210,
        .dst_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .convert_func = convert_y210_to_rfc4175_422be10,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV422PLANAR12LE,
        .dst_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE12,
        .convert_func = convert_yuv422p12le_to_rfc4175_422be12,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV444PLANAR10LE,
        .dst_fmt = ST_FRAME_FMT_YUV444RFC4175PG4BE10,
        .convert_func = convert_yuv444p10le_to_rfc4175_444be10,
    },
    {
        .src_fmt = ST_FRAME_FMT_YUV444PLANAR12LE,
        .dst_fmt = ST_FRAME_FMT_YUV444RFC4175PG2BE12,
        .convert_func = convert_yuv444p12le_to_rfc4175_444be12,
    },
    {
        .src_fmt = ST_FRAME_FMT_GBRPLANAR10LE,
        .dst_fmt = ST_FRAME_FMT_RGBRFC4175PG4BE10,
        .convert_func = convert_gbrp10le_to_rfc4175_444be10,
    },
    {
        .src_fmt = ST_FRAME_FMT_GBRPLANAR12LE,
        .dst_fmt = ST_FRAME_FMT_RGBRFC4175PG2BE12,
        .convert_func = convert_gbrp12le_to_rfc4175_444be12,
    },
};

int st_frame_convert(struct st_frame* src, struct st_frame* dst) {
  if (src->width != dst->width || src->height != dst->height) {
    err("%s, width/height mismatch, source: %u x %u, dest: %u x %u\n", __func__,
        src->width, src->height, dst->width, dst->height);
    return -EINVAL;
  }
  struct st_frame_converter converter;
  if (st_frame_get_converter(src->fmt, dst->fmt, &converter) < 0) {
    err("%s, get converter fail\n", __func__);
    return -EINVAL;
  }
  return converter.convert_func(src, dst);
}

int st_frame_get_converter(enum st_frame_fmt src_fmt, enum st_frame_fmt dst_fmt,
                           struct st_frame_converter* converter) {
  for (int i = 0; i < MTL_ARRAY_SIZE(converters); i++) {
    if (src_fmt == converters[i].src_fmt && dst_fmt == converters[i].dst_fmt) {
      *converter = converters[i];
      return 0;
    }
  }

  err("%s, format not supported, source: %s, dest: %s\n", __func__,
      st_frame_fmt_name(src_fmt), st_frame_fmt_name(dst_fmt));
  return -EINVAL;
}

static int downsample_rfc4175_wh_half(struct st_frame* old_frame,
                                      struct st_frame* new_frame, int idx) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret = 0;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

  enum st20_fmt t_fmt = st_frame_fmt_to_transport(new_frame->fmt);
  struct st20_pgroup st20_pg;
  st20_get_pgroup(t_fmt, &st20_pg);

  uint32_t width = new_frame->width;
  uint32_t height = new_frame->height;
  uint32_t src_linesize = old_frame->linesize[0];
  uint32_t dst_linesize = new_frame->linesize[0];
  uint8_t* src_start = old_frame->addr[0];
  uint8_t* dst_start = new_frame->addr[0];
  /* check the idx and set src offset */
  switch (idx) {
    case 0:
      break;
    case 1:
      src_start += st20_pg.size;
      break;
    case 2:
      src_start += src_linesize;
      break;
    case 3:
      src_start += src_linesize + st20_pg.size;
      break;
    default:
      err("%s, wrong sample idx %d\n", __func__, idx);
      return -EINVAL;
  }

#ifdef MTL_HAS_AVX512_VBMI2
  if (t_fmt == ST20_FMT_YUV_422_10BIT) { /* temp only 422be10 implemented */
    if (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2) {
      dbg("%s, avx512_vbmi way\n", __func__);
      ret = st20_downsample_rfc4175_422be10_wh_half_avx512_vbmi(
          src_start, dst_start, width, height, src_linesize, dst_linesize);
      if (ret == 0) return 0;
      err("%s, avx512_vbmi way failed %d\n", __func__, ret);
    }
  }
#endif

  /* scalar fallback */
  for (int line = 0; line < height; line++) {
    uint8_t* src = src_start + src_linesize * line * 2;
    uint8_t* dst = dst_start + dst_linesize * line;
    for (int pg = 0; pg < width / st20_pg.coverage; pg++) {
      mtl_memcpy(dst, src, st20_pg.size);
      src += 2 * st20_pg.size;
      dst += st20_pg.size;
    }
  }
  return 0;
}

int st_frame_downsample(struct st_frame* src, struct st_frame* dst, int idx) {
  if (src->fmt == dst->fmt) {
    if (st_frame_fmt_to_transport(src->fmt) != ST20_FMT_MAX) {
      if (src->width == dst->width * 2 && src->height == dst->height * 2) {
        return downsample_rfc4175_wh_half(src, dst, idx);
      }
    }
  }

  err("%s, downsample not supported, source: %s %ux%u, dest: %s %ux%u\n", __func__,
      st_frame_fmt_name(src->fmt), src->width, src->height, st_frame_fmt_name(dst->fmt),
      dst->width, dst->height);
  return -EINVAL;
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
                                             enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
    mtl_udma_handle udma, uint16_t* y, mtl_iova_t y_iova, uint16_t* b, mtl_iova_t b_iova,
    uint16_t* r, mtl_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
    uint32_t h, enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                             enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_yuv422p10le_scalar(pg, y, b, r, w, h);
}

int st20_rfc4175_422be10_to_yuv422p10le_simd_dma(mtl_udma_handle udma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma(udma, pg_be, pg_be_iova, y,
                                                              b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                         enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512_vbmi(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX2
  if ((level >= MTL_SIMD_LEVEL_AVX2) && (cpu_level >= MTL_SIMD_LEVEL_AVX2)) {
    dbg("%s, avx2 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx2(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx2 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_422le10_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_422be10_to_422le10_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx512_vbmi_dma(dma, pg_be, pg_be_iova, pg_le,
                                                          w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                         enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_vbmi(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx512(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX2
  if ((level >= MTL_SIMD_LEVEL_AVX2) && (cpu_level >= MTL_SIMD_LEVEL_AVX2)) {
    dbg("%s, avx2 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx2(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx2 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422le10_to_422be10_scalar(pg_le, pg_be, w, h);
}

int st20_rfc4175_422le10_to_422be10_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             mtl_iova_t pg_le_iova,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx512_vbmi_dma(dma, pg_le, pg_le_iova, pg_be,
                                                          w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx512_dma(dma, pg_le, pg_le_iova, pg_be, w, h);
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
                                        enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512_vbmi(pg_10, pg_8, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512(pg_10, pg_8, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_422le8_scalar(pg_10, pg_8, w, h);
}

int st20_rfc4175_422be10_to_422le8_simd_dma(mtl_udma_handle udma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_10,
                                            mtl_iova_t pg_10_iova,
                                            struct st20_rfc4175_422_8_pg2_le* pg_8,
                                            uint32_t w, uint32_t h,
                                            enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le8_avx512_vbmi_dma(dma, pg_10, pg_10_iova, pg_8, w,
                                                         h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                      uint32_t h, enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422le10_to_v210_avx512_vbmi(pg_le, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                      enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512_vbmi(pg_be, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512(pg_be, pg_v210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_v210_scalar((uint8_t*)pg_be, pg_v210, w, h);
}

int st20_rfc4175_422be10_to_v210_simd_dma(mtl_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          mtl_iova_t pg_be_iova, uint8_t* pg_v210,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be10_to_v210_avx512_vbmi_dma(dma, pg_be, pg_be_iova, pg_v210, w,
                                                       h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
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
                                      uint32_t w, uint32_t h, enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_v210_to_rfc4175_422be10_avx512_vbmi(pg_v210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_v210_to_rfc4175_422be10_avx512(pg_v210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_v210_to_rfc4175_422be10_scalar(pg_v210, (uint8_t*)pg_be, w, h);
}

int st20_v210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint8_t* pg_v210,
                                          mtl_iova_t pg_v210_iova,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_v210_to_rfc4175_422be10_avx512_vbmi_dma(udma, pg_v210, pg_v210_iova, pg_be,
                                                       w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret =
        st20_v210_to_rfc4175_422be10_avx512_dma(udma, pg_v210, pg_v210_iova, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_v210_to_rfc4175_422be10_scalar(pg_v210, (uint8_t*)pg_be, w, h);
}

int st20_rfc4175_422be10_to_y210_scalar(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint16_t* pg_y210, uint32_t w, uint32_t h) {
  uint32_t pg_count = w * h / 2;

  for (uint32_t i = 0; i < pg_count; i++) {
    uint32_t j = i * 4;
    pg_y210[j] = (pg_be->Y00 << 10) + (pg_be->Y00_ << 6);
    pg_y210[j + 1] = (pg_be->Cb00 << 8) + (pg_be->Cb00_ << 6);
    pg_y210[j + 2] = (pg_be->Y01 << 14) + (pg_be->Y01_ << 6);
    pg_y210[j + 3] = (pg_be->Cr00 << 12) + (pg_be->Cr00_ << 6);
    pg_be++;
  }

  return 0;
}

int st20_rfc4175_422be10_to_y210_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint16_t* pg_y210, uint32_t w, uint32_t h,
                                      enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_y210_avx512(pg_be, pg_y210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_y210_scalar(pg_be, pg_y210, w, h);
}

int st20_rfc4175_422be10_to_y210_simd_dma(mtl_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          mtl_iova_t pg_be_iova, uint16_t* pg_y210,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_y210_avx512_dma(dma, pg_be, pg_be_iova, pg_y210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_y210_scalar(pg_be, pg_y210, w, h);
}

int st20_y210_to_rfc4175_422be10_scalar(uint16_t* pg_y210,
                                        struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint32_t w, uint32_t h) {
  uint32_t pg_count = w * h / 2;

  for (uint32_t i = 0; i < pg_count; i++) {
    int j = i * 4;
    pg_be->Cb00 = pg_y210[j + 1] >> 8;
    pg_be->Cb00_ = (pg_y210[j + 1] >> 6) & 0x3;
    pg_be->Y00 = pg_y210[j] >> 10;
    pg_be->Y00_ = (pg_y210[j] >> 6) & 0xF;
    pg_be->Cr00 = pg_y210[j + 3] >> 12;
    pg_be->Cr00_ = (pg_y210[j + 3] >> 6) & 0x3F;
    pg_be->Y01 = pg_y210[j + 2] >> 14;
    pg_be->Y01_ = (pg_y210[j + 2] >> 6) & 0xFF;

    pg_be++;
  }

  return 0;
}

int st20_y210_to_rfc4175_422be10_simd(uint16_t* pg_y210,
                                      struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint32_t w, uint32_t h, enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_y210_to_rfc4175_422be10_avx512(pg_y210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_y210_to_rfc4175_422be10_scalar(pg_y210, pg_be, w, h);
}

int st20_y210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint16_t* pg_y210,
                                          mtl_iova_t pg_y210_iova,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret =
        st20_y210_to_rfc4175_422be10_avx512_dma(udma, pg_y210, pg_y210_iova, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_y210_to_rfc4175_422be10_scalar(pg_y210, pg_be, w, h);
}

static int st20_yuv422p12le_to_rfc4175_422be12_scalar(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_422_12_pg2_be* pg,
    uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = *b++;
    y0 = *y++;
    cr = *r++;
    y1 = *y++;

    pg->Cb00 = cb >> 4;
    pg->Cb00_ = cb;
    pg->Y00 = y0 >> 8;
    pg->Y00_ = y0;
    pg->Cr00 = cr >> 4;
    pg->Cr00_ = cr;
    pg->Y01 = y1 >> 8;
    pg->Y01_ = y1;

    pg++;
  }

  return 0;
}

int st20_yuv422p12le_to_rfc4175_422be12_simd(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_12_pg2_be* pg,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_yuv422p12le_to_rfc4175_422be12_scalar(y, b, r, pg, w, h);
}

static int st20_rfc4175_422be12_to_yuv422p12le_scalar(
    struct st20_rfc4175_422_12_pg2_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = (pg->Cb00 << 4) + pg->Cb00_;
    y0 = (pg->Y00 << 8) + pg->Y00_;
    cr = (pg->Cr00 << 4) + pg->Cr00_;
    y1 = (pg->Y01 << 8) + pg->Y01_;

    *b++ = cb;
    *y++ = y0;
    *r++ = cr;
    *y++ = y1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_422be12_to_yuv422p12le_simd(struct st20_rfc4175_422_12_pg2_be* pg,
                                             uint16_t* y, uint16_t* b, uint16_t* r,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512_VBMI2
  if ((level >= MTL_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422be12_to_yuv422p12le_avx512_vbmi(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be12_to_yuv422p12le_avx512(pg, y, b, r, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be12_to_yuv422p12le_scalar(pg, y, b, r, w, h);
}

int st20_rfc4175_422be12_to_yuv422p12le_simd_dma(mtl_udma_handle udma,
                                                 struct st20_rfc4175_422_12_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be12_to_yuv422p12le_avx512_dma(udma, pg_be, pg_be_iova, y, b, r,
                                                         w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be12_to_yuv422p12le_scalar(pg_be, y, b, r, w, h);
}

int st20_yuv422p12le_to_rfc4175_422le12(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_12_pg2_le* pg, uint32_t w,
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
    pg->Y00_ = y0 >> 4;
    pg->Cr00 = cr;
    pg->Cr00_ = cr >> 8;
    pg->Y01 = y1;
    pg->Y01_ = y1 >> 4;

    pg++;
  }

  return 0;
}

int st20_rfc4175_422le12_to_yuv422p12le(struct st20_rfc4175_422_12_pg2_le* pg,
                                        uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
                                        uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = pg->Cb00 + (pg->Cb00_ << 8);
    y0 = pg->Y00 + (pg->Y00_ << 4);
    cr = pg->Cr00 + (pg->Cr00_ << 8);
    y1 = pg->Y01 + (pg->Y01_ << 4);

    *b++ = cb;
    *y++ = y0;
    *r++ = cr;
    *y++ = y1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_422be12_to_422le12_scalar(struct st20_rfc4175_422_12_pg2_be* pg_be,
                                           struct st20_rfc4175_422_12_pg2_le* pg_le,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2;
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = (pg_be->Cb00 << 4) + pg_be->Cb00_;
    y0 = (pg_be->Y00 << 8) + pg_be->Y00_;
    cr = (pg_be->Cr00 << 4) + pg_be->Cr00_;
    y1 = (pg_be->Y01 << 8) + pg_be->Y01_;

    pg_le->Cb00 = cb;
    pg_le->Cb00_ = cb >> 8;
    pg_le->Y00 = y0;
    pg_le->Y00_ = y0 >> 4;
    pg_le->Cr00 = cr;
    pg_le->Cr00_ = cr >> 8;
    pg_le->Y01 = y1;
    pg_le->Y01_ = y1 >> 4;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_422be12_to_422le12_simd(struct st20_rfc4175_422_12_pg2_be* pg_be,
                                         struct st20_rfc4175_422_12_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be12_to_422le12_avx512(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be12_to_422le12_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_422be12_to_422le12_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_12_pg2_be* pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_12_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level) {
  struct mtl_dma_lender_dev* dma = udma;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  int ret;

  MTL_MAY_UNUSED(cpu_level);
  MTL_MAY_UNUSED(ret);
  MTL_MAY_UNUSED(dma);

#ifdef MTL_HAS_AVX512
  if ((level >= MTL_SIMD_LEVEL_AVX512) && (cpu_level >= MTL_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be12_to_422le12_avx512_dma(dma, pg_be, pg_be_iova, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be12_to_422le12_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_422le12_to_422be12_scalar(struct st20_rfc4175_422_12_pg2_le* pg_le,
                                           struct st20_rfc4175_422_12_pg2_be* pg_be,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb = pg_le->Cb00 + (pg_le->Cb00_ << 8);
    y0 = pg_le->Y00 + (pg_le->Y00_ << 4);
    cr = pg_le->Cr00 + (pg_le->Cr00_ << 8);
    y1 = pg_le->Y01 + (pg_le->Y01_ << 4);

    pg_be->Cb00 = cb >> 4;
    pg_be->Cb00_ = cb;
    pg_be->Y00 = y0 >> 8;
    pg_be->Y00_ = y0;
    pg_be->Cr00 = cr >> 4;
    pg_be->Cr00_ = cr;
    pg_be->Y01 = y1 >> 8;
    pg_be->Y01_ = y1;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_422le12_to_422be12_simd(struct st20_rfc4175_422_12_pg2_le* pg_le,
                                         struct st20_rfc4175_422_12_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_422le12_to_422be12_scalar(pg_le, pg_be, w, h);
}

static int st20_444p10le_to_rfc4175_444be10_scalar(uint16_t* y_g, uint16_t* b_r,
                                                   uint16_t* r_b,
                                                   struct st20_rfc4175_444_10_pg4_be* pg,
                                                   uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 4; /* four pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = *b_r++;
    y_g0 = *y_g++;
    cr_b0 = *r_b++;
    cb_r1 = *b_r++;
    y_g1 = *y_g++;
    cr_b1 = *r_b++;
    cb_r2 = *b_r++;
    y_g2 = *y_g++;
    cr_b2 = *r_b++;
    cb_r3 = *b_r++;
    y_g3 = *y_g++;
    cr_b3 = *r_b++;

    pg->Cb_R00 = cb_r0 >> 2;
    pg->Cb_R00_ = cb_r0;
    pg->Y_G00 = y_g0 >> 4;
    pg->Y_G00_ = y_g0;
    pg->Cr_B00 = cr_b0 >> 6;
    pg->Cr_B00_ = cr_b0;
    pg->Cb_R01 = cb_r1 >> 8;
    pg->Cb_R01_ = cb_r1;
    pg->Y_G01 = y_g1 >> 2;
    pg->Y_G01_ = y_g1;
    pg->Cr_B01 = cr_b1 >> 4;
    pg->Cr_B01_ = cr_b1;
    pg->Cb_R02 = cb_r2 >> 6;
    pg->Cb_R02_ = cb_r2;
    pg->Y_G02 = y_g2 >> 8;
    pg->Y_G02_ = y_g2;
    pg->Cr_B02 = cr_b2 >> 2;
    pg->Cr_B02_ = cr_b2;
    pg->Cb_R03 = cb_r3 >> 4;
    pg->Cb_R03_ = cb_r3;
    pg->Y_G03 = y_g3 >> 6;
    pg->Y_G03_ = y_g3;
    pg->Cr_B03 = cr_b3 >> 8;
    pg->Cr_B03_ = cr_b3;

    pg++;
  }

  return 0;
}

int st20_444p10le_to_rfc4175_444be10_simd(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          struct st20_rfc4175_444_10_pg4_be* pg,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_444p10le_to_rfc4175_444be10_scalar(y_g, b_r, r_b, pg, w, h);
}

static int st20_rfc4175_444be10_to_444p10le_scalar(struct st20_rfc4175_444_10_pg4_be* pg,
                                                   uint16_t* y_g, uint16_t* b_r,
                                                   uint16_t* r_b, uint32_t w,
                                                   uint32_t h) {
  uint32_t cnt = w * h / 4; /* four pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = (pg->Cb_R00 << 2) + pg->Cb_R00_;
    y_g0 = (pg->Y_G00 << 4) + pg->Y_G00_;
    cr_b0 = (pg->Cr_B00 << 6) + pg->Cr_B00_;
    cb_r1 = (pg->Cb_R01 << 8) + pg->Cb_R01_;
    y_g1 = (pg->Y_G01 << 2) + pg->Y_G01_;
    cr_b1 = (pg->Cr_B01 << 4) + pg->Cr_B01_;
    cb_r2 = (pg->Cb_R02 << 6) + pg->Cb_R02_;
    y_g2 = (pg->Y_G02 << 8) + pg->Y_G02_;
    cr_b2 = (pg->Cr_B02 << 2) + pg->Cr_B02_;
    cb_r3 = (pg->Cb_R03 << 4) + pg->Cb_R03_;
    y_g3 = (pg->Y_G03 << 6) + pg->Y_G03_;
    cr_b3 = (pg->Cr_B03 << 8) + pg->Cr_B03_;

    *b_r++ = cb_r0;
    *y_g++ = y_g0;
    *r_b++ = cr_b0;
    *b_r++ = cb_r1;
    *y_g++ = y_g1;
    *r_b++ = cr_b1;
    *b_r++ = cb_r2;
    *y_g++ = y_g2;
    *r_b++ = cr_b2;
    *b_r++ = cb_r3;
    *y_g++ = y_g3;
    *r_b++ = cr_b3;
    pg++;
  }

  return 0;
}

int st20_rfc4175_444be10_to_444p10le_simd(struct st20_rfc4175_444_10_pg4_be* pg,
                                          uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444be10_to_444p10le_scalar(pg, y_g, b_r, r_b, w, h);
}

int st20_444p10le_to_rfc4175_444le10(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                     struct st20_rfc4175_444_10_pg4_le* pg, uint32_t w,
                                     uint32_t h) {
  uint32_t cnt = w * h / 4; /* four pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = *b_r++;
    y_g0 = *y_g++;
    cr_b0 = *r_b++;
    cb_r1 = *b_r++;
    y_g1 = *y_g++;
    cr_b1 = *r_b++;
    cb_r2 = *b_r++;
    y_g2 = *y_g++;
    cr_b2 = *r_b++;
    cb_r3 = *b_r++;
    y_g3 = *y_g++;
    cr_b3 = *r_b++;

    pg->Cb_R00 = cb_r0;
    pg->Cb_R00_ = cb_r0 >> 8;
    pg->Y_G00 = y_g0;
    pg->Y_G00_ = y_g0 >> 6;
    pg->Cr_B00 = cr_b0;
    pg->Cr_B00_ = cr_b0 >> 4;
    pg->Cb_R01 = cb_r1;
    pg->Cb_R01_ = cb_r1 >> 2;
    pg->Y_G01 = y_g1;
    pg->Y_G01_ = y_g1 >> 8;
    pg->Cr_B01 = cr_b1;
    pg->Cr_B01_ = cr_b1 >> 6;
    pg->Cb_R02 = cb_r2;
    pg->Cb_R02_ = cb_r2 >> 4;
    pg->Y_G02 = y_g2;
    pg->Y_G02_ = y_g2 >> 2;
    pg->Cr_B02 = cr_b2;
    pg->Cr_B02_ = cr_b2 >> 8;
    pg->Cb_R03 = cb_r3;
    pg->Cb_R03_ = cb_r3 >> 6;
    pg->Y_G03 = y_g3;
    pg->Y_G03_ = y_g3 >> 4;
    pg->Cr_B03 = cr_b3;
    pg->Cr_B03_ = cr_b3 >> 2;

    pg++;
  }

  return 0;
}

int st20_rfc4175_444le10_to_444p10le(struct st20_rfc4175_444_10_pg4_le* pg, uint16_t* y_g,
                                     uint16_t* b_r, uint16_t* r_b, uint32_t w,
                                     uint32_t h) {
  uint32_t cnt = w * h / 4; /* four pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = pg->Cb_R00 + (pg->Cb_R00_ << 8);
    y_g0 = pg->Y_G00 + (pg->Y_G00_ << 6);
    cr_b0 = pg->Cr_B00 + (pg->Cr_B00_ << 4);
    cb_r1 = pg->Cb_R01 + (pg->Cb_R01_ << 2);
    y_g1 = pg->Y_G01 + (pg->Y_G01_ << 8);
    cr_b1 = pg->Cr_B01 + (pg->Cr_B01_ << 6);
    cb_r2 = pg->Cb_R02 + (pg->Cb_R02_ << 4);
    y_g2 = pg->Y_G02 + (pg->Y_G02_ << 2);
    cr_b2 = pg->Cr_B02 + (pg->Cr_B02_ << 8);
    cb_r3 = pg->Cb_R03 + (pg->Cb_R03_ << 6);
    y_g3 = pg->Y_G03 + (pg->Y_G03_ << 4);
    cr_b3 = pg->Cr_B03 + (pg->Cr_B03_ << 2);

    *b_r++ = cb_r0;
    *y_g++ = y_g0;
    *r_b++ = cr_b0;
    *b_r++ = cb_r1;
    *y_g++ = y_g1;
    *r_b++ = cr_b1;
    *b_r++ = cb_r2;
    *y_g++ = y_g2;
    *r_b++ = cr_b2;
    *b_r++ = cb_r3;
    *y_g++ = y_g3;
    *r_b++ = cr_b3;
    pg++;
  }

  return 0;
}

int st20_rfc4175_444be10_to_444le10_scalar(struct st20_rfc4175_444_10_pg4_be* pg_be,
                                           struct st20_rfc4175_444_10_pg4_le* pg_le,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 4;
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = (pg_be->Cb_R00 << 2) + pg_be->Cb_R00_;
    y_g0 = (pg_be->Y_G00 << 4) + pg_be->Y_G00_;
    cr_b0 = (pg_be->Cr_B00 << 6) + pg_be->Cr_B00_;
    cb_r1 = (pg_be->Cb_R01 << 8) + pg_be->Cb_R01_;
    y_g1 = (pg_be->Y_G01 << 2) + pg_be->Y_G01_;
    cr_b1 = (pg_be->Cr_B01 << 4) + pg_be->Cr_B01_;
    cb_r2 = (pg_be->Cb_R02 << 6) + pg_be->Cb_R02_;
    y_g2 = (pg_be->Y_G02 << 8) + pg_be->Y_G02_;
    cr_b2 = (pg_be->Cr_B02 << 2) + pg_be->Cr_B02_;
    cb_r3 = (pg_be->Cb_R03 << 4) + pg_be->Cb_R03_;
    y_g3 = (pg_be->Y_G03 << 6) + pg_be->Y_G03_;
    cr_b3 = (pg_be->Cr_B03 << 8) + pg_be->Cr_B03_;

    pg_le->Cb_R00 = cb_r0;
    pg_le->Cb_R00_ = cb_r0 >> 8;
    pg_le->Y_G00 = y_g0;
    pg_le->Y_G00_ = y_g0 >> 6;
    pg_le->Cr_B00 = cr_b0;
    pg_le->Cr_B00_ = cr_b0 >> 4;
    pg_le->Cb_R01 = cb_r1;
    pg_le->Cb_R01_ = cb_r1 >> 2;
    pg_le->Y_G01 = y_g1;
    pg_le->Y_G01_ = y_g1 >> 8;
    pg_le->Cr_B01 = cr_b1;
    pg_le->Cr_B01_ = cr_b1 >> 6;
    pg_le->Cb_R02 = cb_r2;
    pg_le->Cb_R02_ = cb_r2 >> 4;
    pg_le->Y_G02 = y_g2;
    pg_le->Y_G02_ = y_g2 >> 2;
    pg_le->Cr_B02 = cr_b2;
    pg_le->Cr_B02_ = cr_b2 >> 8;
    pg_le->Cb_R03 = cb_r3;
    pg_le->Cb_R03_ = cb_r3 >> 6;
    pg_le->Y_G03 = y_g3;
    pg_le->Y_G03_ = y_g3 >> 4;
    pg_le->Cr_B03 = cr_b3;
    pg_le->Cr_B03_ = cr_b3 >> 2;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_444be10_to_444le10_simd(struct st20_rfc4175_444_10_pg4_be* pg_be,
                                         struct st20_rfc4175_444_10_pg4_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444be10_to_444le10_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_444le10_to_444be10_scalar(struct st20_rfc4175_444_10_pg4_le* pg_le,
                                           struct st20_rfc4175_444_10_pg4_be* pg_be,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 4; /* four pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1, cb_r2, y_g2, cr_b2, cb_r3, y_g3, cr_b3;

  for (uint32_t pg4 = 0; pg4 < cnt; pg4++) {
    cb_r0 = pg_le->Cb_R00 + (pg_le->Cb_R00_ << 8);
    y_g0 = pg_le->Y_G00 + (pg_le->Y_G00_ << 6);
    cr_b0 = pg_le->Cr_B00 + (pg_le->Cr_B00_ << 4);
    cb_r1 = pg_le->Cb_R01 + (pg_le->Cb_R01_ << 2);
    y_g1 = pg_le->Y_G01 + (pg_le->Y_G01_ << 8);
    cr_b1 = pg_le->Cr_B01 + (pg_le->Cr_B01_ << 6);
    cb_r2 = pg_le->Cb_R02 + (pg_le->Cb_R02_ << 4);
    y_g2 = pg_le->Y_G02 + (pg_le->Y_G02_ << 2);
    cr_b2 = pg_le->Cr_B02 + (pg_le->Cr_B02_ << 8);
    cb_r3 = pg_le->Cb_R03 + (pg_le->Cb_R03_ << 6);
    y_g3 = pg_le->Y_G03 + (pg_le->Y_G03_ << 4);
    cr_b3 = pg_le->Cr_B03 + (pg_le->Cr_B03_ << 2);

    pg_be->Cb_R00 = cb_r0 >> 2;
    pg_be->Cb_R00_ = cb_r0;
    pg_be->Y_G00 = y_g0 >> 4;
    pg_be->Y_G00_ = y_g0;
    pg_be->Cr_B00 = cr_b0 >> 6;
    pg_be->Cr_B00_ = cr_b0;
    pg_be->Cb_R01 = cb_r1 >> 8;
    pg_be->Cb_R01_ = cb_r1;
    pg_be->Y_G01 = y_g1 >> 2;
    pg_be->Y_G01_ = y_g1;
    pg_be->Cr_B01 = cr_b1 >> 4;
    pg_be->Cr_B01_ = cr_b1;
    pg_be->Cb_R02 = cb_r2 >> 6;
    pg_be->Cb_R02_ = cb_r2;
    pg_be->Y_G02 = y_g2 >> 8;
    pg_be->Y_G02_ = y_g2;
    pg_be->Cr_B02 = cr_b2 >> 2;
    pg_be->Cr_B02_ = cr_b2;
    pg_be->Cb_R03 = cb_r3 >> 4;
    pg_be->Cb_R03_ = cb_r3;
    pg_be->Y_G03 = y_g3 >> 6;
    pg_be->Y_G03_ = y_g3;
    pg_be->Cr_B03 = cr_b3 >> 8;
    pg_be->Cr_B03_ = cr_b3;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_444le10_to_444be10_simd(struct st20_rfc4175_444_10_pg4_le* pg_le,
                                         struct st20_rfc4175_444_10_pg4_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444le10_to_444be10_scalar(pg_le, pg_be, w, h);
}

static int st20_444p12le_to_rfc4175_444be12_scalar(uint16_t* y_g, uint16_t* b_r,
                                                   uint16_t* r_b,
                                                   struct st20_rfc4175_444_12_pg2_be* pg,
                                                   uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = *b_r++;
    y_g0 = *y_g++;
    cr_b0 = *r_b++;
    cb_r1 = *b_r++;
    y_g1 = *y_g++;
    cr_b1 = *r_b++;

    pg->Cb_R00 = cb_r0 >> 4;
    pg->Cb_R00_ = cb_r0;
    pg->Y_G00 = y_g0 >> 8;
    pg->Y_G00_ = y_g0;
    pg->Cr_B00 = cr_b0 >> 4;
    pg->Cr_B00_ = cr_b0;
    pg->Cb_R01 = cb_r1 >> 8;
    pg->Cb_R01_ = cb_r1;
    pg->Y_G01 = y_g1 >> 4;
    pg->Y_G01_ = y_g1;
    pg->Cr_B01 = cr_b1 >> 8;
    pg->Cr_B01_ = cr_b1;

    pg++;
  }

  return 0;
}

int st20_444p12le_to_rfc4175_444be12_simd(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          struct st20_rfc4175_444_12_pg2_be* pg,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_444p12le_to_rfc4175_444be12_scalar(y_g, b_r, r_b, pg, w, h);
}

static int st20_rfc4175_444be12_to_444p12le_scalar(struct st20_rfc4175_444_12_pg2_be* pg,
                                                   uint16_t* y_g, uint16_t* b_r,
                                                   uint16_t* r_b, uint32_t w,
                                                   uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = (pg->Cb_R00 << 4) + pg->Cb_R00_;
    y_g0 = (pg->Y_G00 << 8) + pg->Y_G00_;
    cr_b0 = (pg->Cr_B00 << 4) + pg->Cr_B00_;
    cb_r1 = (pg->Cb_R01 << 8) + pg->Cb_R01_;
    y_g1 = (pg->Y_G01 << 4) + pg->Y_G01_;
    cr_b1 = (pg->Cr_B01 << 8) + pg->Cr_B01_;

    *b_r++ = cb_r0;
    *y_g++ = y_g0;
    *r_b++ = cr_b0;
    *b_r++ = cb_r1;
    *y_g++ = y_g1;
    *r_b++ = cr_b1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_444be12_to_444p12le_simd(struct st20_rfc4175_444_12_pg2_be* pg,
                                          uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444be12_to_444p12le_scalar(pg, y_g, b_r, r_b, w, h);
}

int st20_444p12le_to_rfc4175_444le12(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                     struct st20_rfc4175_444_12_pg2_le* pg, uint32_t w,
                                     uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = *b_r++;
    y_g0 = *y_g++;
    cr_b0 = *r_b++;
    cb_r1 = *b_r++;
    y_g1 = *y_g++;
    cr_b1 = *r_b++;

    pg->Cb_R00 = cb_r0;
    pg->Cb_R00_ = cb_r0 >> 8;
    pg->Y_G00 = y_g0;
    pg->Y_G00_ = y_g0 >> 4;
    pg->Cr_B00 = cr_b0;
    pg->Cr_B00_ = cr_b0 >> 8;
    pg->Cb_R01 = cb_r1;
    pg->Cb_R01_ = cb_r1 >> 4;
    pg->Y_G01 = y_g1;
    pg->Y_G01_ = y_g1 >> 8;
    pg->Cr_B01 = cr_b1;
    pg->Cr_B01_ = cr_b1 >> 4;

    pg++;
  }

  return 0;
}

int st20_rfc4175_444le12_to_444p12le(struct st20_rfc4175_444_12_pg2_le* pg, uint16_t* y_g,
                                     uint16_t* b_r, uint16_t* r_b, uint32_t w,
                                     uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = pg->Cb_R00 + (pg->Cb_R00_ << 8);
    y_g0 = pg->Y_G00 + (pg->Y_G00_ << 4);
    cr_b0 = pg->Cr_B00 + (pg->Cr_B00_ << 8);
    cb_r1 = pg->Cb_R01 + (pg->Cb_R01_ << 4);
    y_g1 = pg->Y_G01 + (pg->Y_G01_ << 8);
    cr_b1 = pg->Cr_B01 + (pg->Cr_B01_ << 4);

    *b_r++ = cb_r0;
    *y_g++ = y_g0;
    *r_b++ = cr_b0;
    *b_r++ = cb_r1;
    *y_g++ = y_g1;
    *r_b++ = cr_b1;
    pg++;
  }

  return 0;
}

int st20_rfc4175_444be12_to_444le12_scalar(struct st20_rfc4175_444_12_pg2_be* pg_be,
                                           struct st20_rfc4175_444_12_pg2_le* pg_le,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2;
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = (pg_be->Cb_R00 << 4) + pg_be->Cb_R00_;
    y_g0 = (pg_be->Y_G00 << 8) + pg_be->Y_G00_;
    cr_b0 = (pg_be->Cr_B00 << 4) + pg_be->Cr_B00_;
    cb_r1 = (pg_be->Cb_R01 << 8) + pg_be->Cb_R01_;
    y_g1 = (pg_be->Y_G01 << 4) + pg_be->Y_G01_;
    cr_b1 = (pg_be->Cr_B01 << 8) + pg_be->Cr_B01_;

    pg_le->Cb_R00 = cb_r0;
    pg_le->Cb_R00_ = cb_r0 >> 8;
    pg_le->Y_G00 = y_g0;
    pg_le->Y_G00_ = y_g0 >> 4;
    pg_le->Cr_B00 = cr_b0;
    pg_le->Cr_B00_ = cr_b0 >> 8;
    pg_le->Cb_R01 = cb_r1;
    pg_le->Cb_R01_ = cb_r1 >> 4;
    pg_le->Y_G01 = y_g1;
    pg_le->Y_G01_ = y_g1 >> 8;
    pg_le->Cr_B01 = cr_b1;
    pg_le->Cr_B01_ = cr_b1 >> 4;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_444be12_to_444le12_simd(struct st20_rfc4175_444_12_pg2_be* pg_be,
                                         struct st20_rfc4175_444_12_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444be12_to_444le12_scalar(pg_be, pg_le, w, h);
}

int st20_rfc4175_444le12_to_444be12_scalar(struct st20_rfc4175_444_12_pg2_le* pg_le,
                                           struct st20_rfc4175_444_12_pg2_be* pg_be,
                                           uint32_t w, uint32_t h) {
  uint32_t cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb_r0, y_g0, cr_b0, cb_r1, y_g1, cr_b1;

  for (uint32_t pg2 = 0; pg2 < cnt; pg2++) {
    cb_r0 = pg_le->Cb_R00 + (pg_le->Cb_R00_ << 8);
    y_g0 = pg_le->Y_G00 + (pg_le->Y_G00_ << 4);
    cr_b0 = pg_le->Cr_B00 + (pg_le->Cr_B00_ << 8);
    cb_r1 = pg_le->Cb_R01 + (pg_le->Cb_R01_ << 4);
    y_g1 = pg_le->Y_G01 + (pg_le->Y_G01_ << 8);
    cr_b1 = pg_le->Cr_B01 + (pg_le->Cr_B01_ << 4);

    pg_be->Cb_R00 = cb_r0 >> 4;
    pg_be->Cb_R00_ = cb_r0;
    pg_be->Y_G00 = y_g0 >> 8;
    pg_be->Y_G00_ = y_g0;
    pg_be->Cr_B00 = cr_b0 >> 4;
    pg_be->Cr_B00_ = cr_b0;
    pg_be->Cb_R01 = cb_r1 >> 8;
    pg_be->Cb_R01_ = cb_r1;
    pg_be->Y_G01 = y_g1 >> 4;
    pg_be->Y_G01_ = y_g1;
    pg_be->Cr_B01 = cr_b1 >> 8;
    pg_be->Cr_B01_ = cr_b1;

    pg_be++;
    pg_le++;
  }

  return 0;
}

int st20_rfc4175_444le12_to_444be12_simd(struct st20_rfc4175_444_12_pg2_le* pg_le,
                                         struct st20_rfc4175_444_12_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level) {
  MTL_MAY_UNUSED(level);
  /* the only option */
  return st20_rfc4175_444le12_to_444be12_scalar(pg_le, pg_be, w, h);
}

int st31_am824_to_aes3(struct st31_am824* sf_am824, struct st31_aes3* sf_aes3,
                       uint16_t subframes) {
  for (int i = 0; i < subframes; ++i) {
    /* preamble bits definition refer to
     * https://www.intel.com/content/www/us/en/docs/programmable/683333/22-2-19-1-2/sdi-audio-fpga-ip-overview.html
     */
    if (sf_am824->b) {
      /* block start, set "Z" preamble */
      sf_aes3->preamble = 0x2;
    } else if (sf_am824->f) {
      /* frame start, set "X" preamble */
      sf_aes3->preamble = 0x0;
    } else {
      /* second subframe, set "Y" preamble */
      sf_aes3->preamble = 0x1;
    }

    /* copy p,c,u,v bits */
    sf_aes3->p = sf_am824->p;
    sf_aes3->c = sf_am824->c;
    sf_aes3->u = sf_am824->u;
    sf_aes3->v = sf_am824->v;

    /* copy audio data */
    sf_aes3->data_0 = sf_am824->data[0];
    sf_aes3->data_1 = ((uint16_t)sf_am824->data[0] >> 4) |
                      ((uint16_t)sf_am824->data[1] << 4) |
                      ((uint16_t)sf_am824->data[2] << 12);
    sf_aes3->data_2 = sf_am824->data[2] >> 4;

    sf_aes3++;
    sf_am824++;
  }

  return 0;
}

int st31_aes3_to_am824(struct st31_aes3* sf_aes3, struct st31_am824* sf_am824,
                       uint16_t subframes) {
  for (int i = 0; i < subframes; ++i) {
    /* preamble bits definition refer to
     * https://www.intel.com/content/www/us/en/docs/programmable/683333/22-2-19-1-2/sdi-audio-fpga-ip-overview.html
     */
    if (sf_aes3->preamble == 0x2) {
      /* block start */
      sf_am824->b = 1;
      sf_am824->f = 1;
      sf_am824->unused = 0;
    } else if (sf_aes3->preamble == 0x0) {
      /* frame start */
      sf_am824->f = 1;
      sf_am824->b = 0;
      sf_am824->unused = 0;
    } else {
      /* second subframe */
      sf_am824->b = 0;
      sf_am824->f = 0;
      sf_am824->unused = 0;
    }

    /* copy p,c,u,v bits */
    sf_am824->p = sf_aes3->p;
    sf_am824->c = sf_aes3->c;
    sf_am824->u = sf_aes3->u;
    sf_am824->v = sf_aes3->v;

    /* copy audio data */
    sf_am824->data[0] = sf_aes3->data_0 | (sf_aes3->data_1 << 4);
    sf_am824->data[1] = sf_aes3->data_1 >> 4;
    sf_am824->data[2] = (sf_aes3->data_2 << 4) | ((uint16_t)sf_aes3->data_1 >> 12);

    sf_aes3++;
    sf_am824++;
  }

  return 0;
}
