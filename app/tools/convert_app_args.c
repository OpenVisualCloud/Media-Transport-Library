/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <getopt.h>

#include "convert_app_base.h"
// #define DEBUG
#include "log.h"

enum conv_args_cmd {
  CONV_ARG_UNKNOWN = 0,

  CONV_ARG_FORMAT_IN = 0x100, /* start from end of ascii */
  CONV_ARG_FORMAT_OUT,
  CONV_ARG_W,
  CONV_ARG_H,
  CONV_ARG_FILE_IN,
  CONV_ARG_FILE_OUT,
  CONV_ARG_FRAME2FIELD,
  CONV_ARG_HELP,
};

static struct option conv_app_args_options[] = {
    {"in_pix_fmt", required_argument, 0, CONV_ARG_FORMAT_IN},
    {"out_pix_fmt", required_argument, 0, CONV_ARG_FORMAT_OUT},
    {"width", required_argument, 0, CONV_ARG_W},
    {"height", required_argument, 0, CONV_ARG_H},
    {"i", required_argument, 0, CONV_ARG_FILE_IN},
    {"o", required_argument, 0, CONV_ARG_FILE_OUT},
    {"help", no_argument, 0, CONV_ARG_HELP},
    {"frame2field", no_argument, 0, CONV_ARG_FRAME2FIELD},
    {0, 0, 0, 0}};

static void conv_app_print_app() {
  printf("\n");
  printf("##### Usage: #####\n\n");
  printf(" Params:\n");
  printf(" --help        : print this help info\n");
  printf(" --width       : source width\n");
  printf(" --height      : source height\n");
  printf(" --in_pix_fmt  : yuv422p10le, v210, yuv422rfc4175be10\n");
  printf(" --out_pix_fmt : yuv422p10le, v210, yuv422rfc4175be10\n");
  printf(" --frame2field : convert frame to field\n");
  printf(" --i           : input file\n");
  printf(" --o           : output file\n");
  printf("\n");
}

static enum cvt_frame_fmt cvt_parse_fmt(const char *sfmt) {
  if (!strcmp(sfmt, "yuv422p10le")) {
    return CVT_FRAME_FMT_YUV422PLANAR10LE;
  }
  if (!strcmp(sfmt, "v210")) {
    return CVT_FRAME_FMT_V210;
  }
  if (!strcmp(sfmt, "y210")) {
    return CVT_FRAME_FMT_Y210;
  }
  if (!strcmp(sfmt, "yuv422p12le")) {
    return CVT_FRAME_FMT_YUV422PLANAR12LE;
  }
  if (!strcmp(sfmt, "yuv444p10le")) {
    return CVT_FRAME_FMT_YUV444PLANAR10LE;
  }
  if (!strcmp(sfmt, "yuv444p12le")) {
    return CVT_FRAME_FMT_YUV444PLANAR12LE;
  }
  if (!strcmp(sfmt, "gbrp10le")) {
    return CVT_FRAME_FMT_GBRPLANAR10LE;
  }
  if (!strcmp(sfmt, "gbrp12le")) {
    return CVT_FRAME_FMT_GBRPLANAR12LE;
  }
  if (!strcmp(sfmt, "yuv422rfc4175be10")) {
    return CVT_FRAME_FMT_YUV422RFC4175PG2BE10;
  }
  if (!strcmp(sfmt, "yuv422rfc4175be12")) {
    return CVT_FRAME_FMT_YUV422RFC4175PG2BE12;
  }
  if (!strcmp(sfmt, "yuv444rfc4175be10")) {
    return CVT_FRAME_FMT_YUV444RFC4175PG4BE10;
  }
  if (!strcmp(sfmt, "yuv444rfc4175be12")) {
    return CVT_FRAME_FMT_YUV444RFC4175PG2BE12;
  }
  if (!strcmp(sfmt, "rgbrfc4175be10")) {
    return CVT_FRAME_FMT_RGBRFC4175PG4BE10;
  }
  if (!strcmp(sfmt, "rgbrfc4175be12")) {
    return CVT_FRAME_FMT_RGBRFC4175PG2BE12;
  }

  err("%s, unknown sfmt %s\n", __func__, sfmt);
  return CVT_FRAME_FMT_MAX;
}

int conv_app_parse_args(struct conv_app_context *ctx, int argc, char **argv) {
  int cmd = -1, optIdx = 0;
  int ret = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", conv_app_args_options, &optIdx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case CONV_ARG_FORMAT_IN:
        ctx->fmt_in = cvt_parse_fmt(optarg);
        break;
      case CONV_ARG_FORMAT_OUT:
        ctx->fmt_out = cvt_parse_fmt(optarg);
        break;
      case CONV_ARG_W:
        ctx->w = atoi(optarg);
        break;
      case CONV_ARG_H:
        ctx->h = atoi(optarg);
        break;
      case CONV_ARG_FILE_IN:
        snprintf(ctx->file_in, sizeof(ctx->file_in), "%s", optarg);
        break;
      case CONV_ARG_FILE_OUT:
        snprintf(ctx->file_out, sizeof(ctx->file_out), "%s", optarg);
        break;
      case CONV_ARG_FRAME2FIELD:
        ctx->frame2field = true;
        break;
      case CONV_ARG_HELP:
        conv_app_print_app();
        ret = -EIO;
        break;
      default:
        conv_app_print_app();
        ret = -EIO;
        break;
    }
  };

  return ret;
}
