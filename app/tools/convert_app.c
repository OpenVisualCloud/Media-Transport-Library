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

#include <assert.h>
#include <stdio.h>

#include "app_base.h"
#include "args.h"
#include "log.h"
#include "pack.h"

int conv_app_convert_yuv422p10_to_ycbcr422_10bit_pg(struct conv_app_context* ctx) {
  // check if the input param valid
  int ret = 0;
  if (strcmp(ctx->pix_fmt_in, "yuv422p10be") && strcmp(ctx->pix_fmt_in, "yuv422p10le")) {
    err("in this function, only yuv422p10be or yuv422p10le input is supported");
    return -1;
  }
  if (strcmp(ctx->pix_fmt_out, "yuv422YCBCR10be") &&
      strcmp(ctx->pix_fmt_out, "yuv422YCBCR10le")) {
    err("in this function, only yuv422YCBCR10be or yuv422YCBCR10le output is supported");
    return -1;
  }
  if (!ctx->w || !ctx->h) {
    err("w or h is 0\n");
    return -1;
  }

  int src_be = !strcmp(ctx->pix_fmt_in, "yuv422p10be");
  int frame_size_in = ctx->w * ctx->h * 2 * sizeof(uint16_t);
  int frame_size_out = ctx->w * ctx->h * 2 * 10 / 8;
  FILE *fp_in = NULL, *fp_out = NULL;
  fp_in = fopen(ctx->file_in, "rb");
  if (!fp_in) return -1;
  fp_out = fopen(ctx->file_out, "wb");
  if (!fp_out) {
    fclose(fp_in);
    return -1;
  }
  uint8_t *buf_in = NULL, *buf_out = NULL;
  buf_in = malloc(frame_size_in);
  if (!buf_in) {
    ret = -1;
    goto release;
  }
  buf_out = malloc(frame_size_out);
  if (!buf_out) {
    ret = -1;
    goto release;
  }
  // get the frameNum
  fseek(fp_in, 0, SEEK_END);
  long size = ftell(fp_in);
  int frameNum = size / frame_size_in;
  if (frameNum == 0) {
    err("no complete frame is contained, w or h may error\n");
    goto release;
  }
  info("file size:%ld, frame_num: %d\n", size, frameNum);

  fseek(fp_in, 0, SEEK_SET);
  int be = !strcmp(ctx->pix_fmt_out, "yuv422YCBCR10be");
  for (int i = 0; i < frameNum; i++) {
    int ret = fread(buf_in, 1, frame_size_in, fp_in);
    assert(ret == frame_size_in);
    uint16_t const* Y = (uint16_t*)(buf_in);
    uint16_t const* R = (uint16_t*)(buf_in + ctx->w * ctx->h * sizeof(uint16_t));
    uint16_t const* B = (uint16_t*)(buf_in + ctx->w * ctx->h * 3 / 2 * sizeof(uint16_t));
    rfc4175_422_10_pg2_t* p = (rfc4175_422_10_pg2_t*)buf_out;
    rfc4175_422_10_pg2_le_t* p_le = (rfc4175_422_10_pg2_le_t*)buf_out;
    for (int pg2 = 0; pg2 < ctx->w * ctx->h / 2; pg2++) {
      if (be) {
        if (src_be)
          Pack_422be10_PG2be(p++, *R++, Y[0], *B++, Y[1]);
        else
          Pack_422le10_PG2be(p++, *R++, Y[0], *B++, Y[1]);
      } else {
        if (src_be)
          Pack_422be10_PG2le(p_le++, *R++, Y[0], *B++, Y[1]);
        else
          Pack_422le10_PG2le(p_le++, *R++, Y[0], *B++, Y[1]);
      }
      Y += 2;
    }
    fwrite(buf_out, 1, frame_size_out, fp_out);
  }

release : {
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
}

int conv_app_convert_ycbcr422_10bit_pg_to_yuv422p10(struct conv_app_context* ctx) {
  // check if the input param valid
  if (strcmp(ctx->pix_fmt_in, "yuv422YCBCR10be") &&
      strcmp(ctx->pix_fmt_in, "yuv422YCBCR10le")) {
    err("in this function, only yuv422YCBCR10be or yuv422YCBCR10le input is supported");
    return -1;
  }
  int ret = 0;
  if (strcmp(ctx->pix_fmt_out, "yuv422p10be") &&
      strcmp(ctx->pix_fmt_out, "yuv422p10le")) {
    err("in this function, only yuv422p10be or yuv422p10le output is supported");
    return -1;
  }
  if (!ctx->w || !ctx->h) {
    err("w or h is 0\n");
    return -1;
  }
  int out_be = !strcmp(ctx->pix_fmt_out, "yuv422p10be");
  int frame_size_in = ctx->w * ctx->h * 2 * 10 / 8;
  int frame_size_out = ctx->w * ctx->h * 2 * sizeof(uint16_t);
  FILE *fp_in = NULL, *fp_out = NULL;
  fp_in = fopen(ctx->file_in, "rb");
  if (!fp_in) return -1;
  fp_out = fopen(ctx->file_out, "wb");
  if (!fp_out) {
    fclose(fp_in);
    return -1;
  }
  uint8_t *buf_in = NULL, *buf_out = NULL;
  buf_in = malloc(frame_size_in);
  if (!buf_in) {
    ret = -1;
    goto release;
  }
  buf_out = malloc(frame_size_out);
  if (!buf_out) {
    ret = -1;
    goto release;
  }
  // get the frameNum
  fseek(fp_in, 0, SEEK_END);
  long size = ftell(fp_in);
  int frameNum = size / frame_size_in;
  if (frameNum == 0) {
    err("no complete frame is contained, w or h may error\n");
    goto release;
  }
  info("file size:%ld, frame_num: %d\n", size, frameNum);
  fseek(fp_in, 0, SEEK_SET);
  int be = !strcmp(ctx->pix_fmt_in, "yuv422YCBCR10be");
  for (int i = 0; i < frameNum; i++) {
    int ret = fread(buf_in, 1, frame_size_in, fp_in);
    assert(ret == frame_size_in);
    uint16_t* Y = (uint16_t*)(buf_out);
    uint16_t* R = (uint16_t*)(buf_out + ctx->w * ctx->h * sizeof(uint16_t));
    uint16_t* B = (uint16_t*)(buf_out + ctx->w * ctx->h * 3 / 2 * sizeof(uint16_t));
    rfc4175_422_10_pg2_t* p = (rfc4175_422_10_pg2_t*)buf_in;
    rfc4175_422_10_pg2_le_t* p_le = (rfc4175_422_10_pg2_le_t*)buf_in;
    for (int pg2 = 0; pg2 < ctx->w * ctx->h / 2; pg2++) {
      if (be) {
        if (out_be)
          Unpack_PG2be_422be10(p++, R++, Y, B++, Y + 1);
        else
          Unpack_PG2be_422le10(p++, R++, Y, B++, Y + 1);
      } else {
        if (out_be)
          Unpack_PG2le_422be10(p_le++, R++, Y, B++, Y + 1);
        else
          Unpack_PG2le_422le10(p_le++, R++, Y, B++, Y + 1);
      }
      Y += 2;
    }
    fwrite(buf_out, 1, frame_size_out, fp_out);
  }

release : {
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
}

int main(int argc, char** argv) {
  int ret;
  struct conv_app_context* ctx;

  ctx = zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }

  ret = conv_app_parse_args(ctx, argc, argv);
  if (ret < 0) {
    err("%s, conv_app_parse_args fail %d\n", __func__, ret);
    conv_app_free(ctx);
    return 0;
  }

  if ((!strcmp(ctx->pix_fmt_in, "yuv422p10be") ||
       !strcmp(ctx->pix_fmt_in, "yuv422p10le")) &&
      (!strcmp(ctx->pix_fmt_out, "yuv422YCBCR10be") ||
       !strcmp(ctx->pix_fmt_out, "yuv422YCBCR10le"))) {
    ret = conv_app_convert_yuv422p10_to_ycbcr422_10bit_pg(ctx);
    if (ret < 0) {
      err("%s, conv_app_convert_yuv422p10_to_ycbcr422_10bit_pg fail %d\n", __func__, ret);
      conv_app_free(ctx);
      return 0;
    }
  }

  if ((!strcmp(ctx->pix_fmt_out, "yuv422p10be") ||
       !strcmp(ctx->pix_fmt_out, "yuv422p10le")) &&
      (!strcmp(ctx->pix_fmt_in, "yuv422YCBCR10be") ||
       !strcmp(ctx->pix_fmt_in, "yuv422YCBCR10le"))) {
    ret = conv_app_convert_ycbcr422_10bit_pg_to_yuv422p10(ctx);
    if (ret < 0) {
      err("%s, conv_app_convert_ycbcr422_10bit_pg_to_yuv422p10 fail %d\n", __func__, ret);
      conv_app_free(ctx);
      return 0;
    }
  }
  /* free */
  conv_app_free(ctx);

  return 0;
}
