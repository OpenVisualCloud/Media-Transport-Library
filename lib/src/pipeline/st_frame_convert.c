/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_frame_convert.h"

#include "../st_log.h"

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
        .dst_fmt = ST_FRAME_FMT_YUV422PACKED8,
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
  for (int i = 0; i < ST_ARRAY_SIZE(converters); i++) {
    if (src_fmt == converters[i].src_fmt && dst_fmt == converters[i].dst_fmt) {
      *converter = converters[i];
      return 0;
    }
  }

  err("%s, format not supported, source: %s, dest: %s\n", __func__,
      st_frame_fmt_name(src_fmt), st_frame_fmt_name(dst_fmt));
  return -EINVAL;
}