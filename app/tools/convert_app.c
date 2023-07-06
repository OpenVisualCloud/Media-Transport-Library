/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <assert.h>
#include <inttypes.h>
#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>
#include <stdio.h>

#include "../src/app_platform.h"
#include "convert_app_base.h"
#include "log.h"

static enum st_frame_fmt fmt_cvt2frame(enum cvt_frame_fmt fmt) {
  switch (fmt) {
    case CVT_FRAME_FMT_YUV422PLANAR10LE:
      return ST_FRAME_FMT_YUV422PLANAR10LE;
    case CVT_FRAME_FMT_YUV422PLANAR12LE:
      return ST_FRAME_FMT_YUV422PLANAR12LE;
    case CVT_FRAME_FMT_V210:
      return ST_FRAME_FMT_V210;
    case CVT_FRAME_FMT_Y210:
      return ST_FRAME_FMT_Y210;
    case CVT_FRAME_FMT_YUV444PLANAR10LE:
      return ST_FRAME_FMT_YUV444PLANAR10LE;
    case CVT_FRAME_FMT_YUV444PLANAR12LE:
      return ST_FRAME_FMT_YUV444PLANAR12LE;
    case CVT_FRAME_FMT_GBRPLANAR10LE:
      return ST_FRAME_FMT_GBRPLANAR10LE;
    case CVT_FRAME_FMT_GBRPLANAR12LE:
      return ST_FRAME_FMT_GBRPLANAR12LE;
    case CVT_FRAME_FMT_YUV422RFC4175PG2BE10:
      return ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    case CVT_FRAME_FMT_YUV422RFC4175PG2BE12:
      return ST_FRAME_FMT_YUV422RFC4175PG2BE12;
    case CVT_FRAME_FMT_YUV444RFC4175PG4BE10:
      return ST_FRAME_FMT_YUV444RFC4175PG4BE10;
    case CVT_FRAME_FMT_YUV444RFC4175PG2BE12:
      return ST_FRAME_FMT_YUV444RFC4175PG2BE12;
    case CVT_FRAME_FMT_RGBRFC4175PG4BE10:
      return ST_FRAME_FMT_RGBRFC4175PG4BE10;
    case CVT_FRAME_FMT_RGBRFC4175PG2BE12:
      return ST_FRAME_FMT_RGBRFC4175PG2BE12;
    default:
      break;
  }

  err("%s, unknown fmt %d\n", __func__, fmt);
  return ST_FRAME_FMT_MAX;
}

static int convert(struct conv_app_context* ctx) {
  enum cvt_frame_fmt fmt_in = ctx->fmt_in;
  enum cvt_frame_fmt fmt_out = ctx->fmt_out;
  uint32_t w = ctx->w;
  uint32_t h = ctx->h;
  size_t frame_size_in = st_frame_size(fmt_cvt2frame(fmt_in), w, h, false);
  size_t frame_size_out = st_frame_size(fmt_cvt2frame(fmt_out), w, h, false);
  FILE *fp_in = NULL, *fp_out = NULL;
  void *buf_in = NULL, *buf_out = NULL;
  int ret = -EIO;

  if (!frame_size_in || !frame_size_out) return -EIO;

  fp_in = st_fopen(ctx->file_in, "rb");
  if (!fp_in) {
    err("%s, open %s fail\n", __func__, ctx->file_in);
    ret = -EIO;
    goto out;
  }
  fp_out = st_fopen(ctx->file_out, "wb");
  if (!fp_out) {
    err("%s, open %s fail\n", __func__, ctx->file_out);
    ret = -EIO;
    goto out;
  }

  buf_in = conv_app_zmalloc(frame_size_in);
  if (!buf_in) {
    ret = -EIO;
    goto out;
  }
  buf_out = conv_app_zmalloc(frame_size_out);
  if (!buf_out) {
    ret = -EIO;
    goto out;
  }

  // get the frame num
  fseek(fp_in, 0, SEEK_END);
  long size = ftell(fp_in);
  int frame_num = size / frame_size_in;
  if (frame_num < 0) {
    err("%s, err size %ld\n", __func__, size);
    ret = -EIO;
    goto out;
  }
  info("%s, file size:%ld, %d frames(%ux%u), in %s(%d) out %s(%d)\n", __func__, size,
       frame_num, w, h, ctx->file_in, fmt_in, ctx->file_out, fmt_out);

  fseek(fp_in, 0, SEEK_SET);
  for (int i = 0; i < frame_num; i++) {
    int ret = fread(buf_in, 1, frame_size_in, fp_in);
    if (ret < frame_size_in) {
      err("%s, fread fail %d\n", __func__, ret);
      ret = -EIO;
      goto out;
    }

    if (fmt_in == CVT_FRAME_FMT_YUV422PLANAR10LE) {
      if (fmt_out == CVT_FRAME_FMT_YUV422RFC4175PG2BE10) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_in;
        b = y + w * h;
        r = b + w * h / 2;
        st20_yuv422p10le_to_rfc4175_422be10(
            y, b, r, (struct st20_rfc4175_422_10_pg2_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV422PLANAR12LE) {
      if (fmt_out == CVT_FRAME_FMT_YUV422RFC4175PG2BE12) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_in;
        b = y + w * h;
        r = b + w * h / 2;
        st20_yuv422p12le_to_rfc4175_422be12(
            y, b, r, (struct st20_rfc4175_422_12_pg2_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_V210) {
      if (fmt_out == CVT_FRAME_FMT_YUV422RFC4175PG2BE10) {
        st20_v210_to_rfc4175_422be10(buf_in, (struct st20_rfc4175_422_10_pg2_be*)buf_out,
                                     w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_Y210) {
      if (fmt_out == CVT_FRAME_FMT_YUV422RFC4175PG2BE10) {
        st20_y210_to_rfc4175_422be10(buf_in, (struct st20_rfc4175_422_10_pg2_be*)buf_out,
                                     w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV444PLANAR10LE) {
      if (fmt_out == CVT_FRAME_FMT_YUV444RFC4175PG4BE10) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_in;
        b = y + w * h;
        r = b + w * h;
        st20_yuv444p10le_to_rfc4175_444be10(
            y, b, r, (struct st20_rfc4175_444_10_pg4_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV444PLANAR12LE) {
      if (fmt_out == CVT_FRAME_FMT_YUV444RFC4175PG2BE12) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_in;
        b = y + w * h;
        r = b + w * h;
        st20_yuv444p12le_to_rfc4175_444be12(
            y, b, r, (struct st20_rfc4175_444_12_pg2_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_GBRPLANAR10LE) {
      if (fmt_out == CVT_FRAME_FMT_RGBRFC4175PG4BE10) {
        uint16_t *g, *b, *r;
        g = (uint16_t*)buf_in;
        b = g + w * h;
        r = b + w * h;
        st20_gbrp10le_to_rfc4175_444be10(
            g, b, r, (struct st20_rfc4175_444_10_pg4_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_GBRPLANAR12LE) {
      if (fmt_out == CVT_FRAME_FMT_RGBRFC4175PG2BE12) {
        uint16_t *g, *b, *r;
        g = (uint16_t*)buf_in;
        b = g + w * h;
        r = b + w * h;
        st20_gbrp12le_to_rfc4175_444be12(
            g, b, r, (struct st20_rfc4175_444_12_pg2_be*)buf_out, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV422RFC4175PG2BE10) {
      if (fmt_out == CVT_FRAME_FMT_YUV422PLANAR10LE) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_out;
        b = y + w * h;
        r = b + w * h / 2;
        st20_rfc4175_422be10_to_yuv422p10le((struct st20_rfc4175_422_10_pg2_be*)buf_in, y,
                                            b, r, w, h);
      } else if (fmt_out == CVT_FRAME_FMT_V210) {
        st20_rfc4175_422be10_to_v210((struct st20_rfc4175_422_10_pg2_be*)buf_in, buf_out,
                                     w, h);
      } else if (fmt_out == CVT_FRAME_FMT_Y210) {
        st20_rfc4175_422be10_to_y210((struct st20_rfc4175_422_10_pg2_be*)buf_in, buf_out,
                                     w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV422RFC4175PG2BE12) {
      if (fmt_out == CVT_FRAME_FMT_YUV422PLANAR12LE) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_out;
        b = y + w * h;
        r = b + w * h / 2;
        st20_rfc4175_422be12_to_yuv422p12le((struct st20_rfc4175_422_12_pg2_be*)buf_in, y,
                                            b, r, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV444RFC4175PG4BE10) {
      if (fmt_out == CVT_FRAME_FMT_YUV444PLANAR10LE) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_out;
        b = y + w * h;
        r = b + w * h;
        st20_rfc4175_444be10_to_yuv444p10le((struct st20_rfc4175_444_10_pg4_be*)buf_in, y,
                                            b, r, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_YUV444RFC4175PG2BE12) {
      if (fmt_out == CVT_FRAME_FMT_YUV444PLANAR12LE) {
        uint16_t *y, *b, *r;
        y = (uint16_t*)buf_out;
        b = y + w * h;
        r = b + w * h;
        st20_rfc4175_444be12_to_yuv444p12le((struct st20_rfc4175_444_12_pg2_be*)buf_in, y,
                                            b, r, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_RGBRFC4175PG4BE10) {
      if (fmt_out == CVT_FRAME_FMT_GBRPLANAR10LE) {
        uint16_t *g, *b, *r;
        g = (uint16_t*)buf_out;
        b = g + w * h;
        r = b + w * h;
        st20_rfc4175_444be10_to_gbrp10le((struct st20_rfc4175_444_10_pg4_be*)buf_in, g, b,
                                         r, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else if (fmt_in == CVT_FRAME_FMT_RGBRFC4175PG2BE12) {
      if (fmt_out == CVT_FRAME_FMT_GBRPLANAR12LE) {
        uint16_t *g, *b, *r;
        g = (uint16_t*)buf_out;
        b = g + w * h;
        r = b + w * h;
        st20_rfc4175_444be12_to_gbrp12le((struct st20_rfc4175_444_12_pg2_be*)buf_in, g, b,
                                         r, w, h);
      } else {
        err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
        ret = -EIO;
        goto out;
      }
    } else {
      err("%s, err fmt in %d out %d\n", __func__, fmt_in, fmt_out);
      ret = -EIO;
      goto out;
    }

    /* write out */
    fwrite(buf_out, 1, frame_size_out, fp_out);
  }

out:
  if (fp_in) {
    fclose(fp_in);
    fp_in = NULL;
  }
  if (fp_out) {
    fclose(fp_out);
    fp_out = NULL;
  }
  if (buf_in) {
    free(buf_in);
    buf_in = NULL;
  }
  if (buf_out) {
    free(buf_out);
    buf_out = NULL;
  }
  return ret;
}

int main(int argc, char** argv) {
  int ret = -EIO;
  struct conv_app_context* ctx;

  ctx = conv_app_zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }
  ctx->fmt_in = CVT_FRAME_FMT_MAX;
  ctx->fmt_out = CVT_FRAME_FMT_MAX;

  ret = conv_app_parse_args(ctx, argc, argv);
  if (ret < 0) {
    err("%s, conv_app_parse_args fail %d\n", __func__, ret);
    conv_app_free(ctx);
    return -EIO;
  }

  if ((ctx->fmt_in == CVT_FRAME_FMT_MAX) || (ctx->fmt_out == CVT_FRAME_FMT_MAX)) {
    err("%s, invalid fmt in %d out %d\n", __func__, ctx->fmt_in, ctx->fmt_out);
    conv_app_free(ctx);
    return -EIO;
  }

  if ((ctx->w > (1920 * 8)) || (ctx->w <= 0)) {
    err("%s, invalid w %d\n", __func__, ctx->w);
    conv_app_free(ctx);
    return -EIO;
  }
  if ((ctx->h > (1080 * 8)) || (ctx->h <= 0)) {
    err("%s, invalid h %d\n", __func__, ctx->h);
    conv_app_free(ctx);
    return -EIO;
  }

  convert(ctx);

  /* free */
  conv_app_free(ctx);

  return 0;
}
