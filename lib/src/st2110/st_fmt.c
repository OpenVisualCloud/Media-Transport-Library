/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_fmt.h"

#include "../mt_log.h"
#include "st_main.h"

static const struct st20_pgroup st20_pgroups[] = {
    {
        /* ST20_FMT_YUV_422_10BIT */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .size = 5,
        .coverage = 2,
        .name = "ST20_FMT_YUV_422_10BIT",
    },
    {
        /* ST20_FMT_YUV_422_8BIT */
        .fmt = ST20_FMT_YUV_422_8BIT,
        .size = 4,
        .coverage = 2,
        .name = "ST20_FMT_YUV_422_8BIT",
    },
    {
        /* ST20_FMT_YUV_422_12BIT */
        .fmt = ST20_FMT_YUV_422_12BIT,
        .size = 6,
        .coverage = 2,
        .name = "ST20_FMT_YUV_422_12BIT",
    },
    {
        /* ST20_FMT_YUV_422_16BIT */
        .fmt = ST20_FMT_YUV_422_16BIT,
        .size = 8,
        .coverage = 2,
        .name = "ST20_FMT_YUV_422_16BIT",
    },
    {
        /* ST20_FMT_YUV_420_8BIT */
        .fmt = ST20_FMT_YUV_420_8BIT,
        .size = 6,
        .coverage = 4,
        .name = "ST20_FMT_YUV_420_8BIT",
    },
    {
        /* ST20_FMT_YUV_420_10BIT */
        .fmt = ST20_FMT_YUV_420_10BIT,
        .size = 15,
        .coverage = 8,
        .name = "ST20_FMT_YUV_420_10BIT",
    },
    {
        /* ST20_FMT_YUV_420_12BIT */
        .fmt = ST20_FMT_YUV_420_12BIT,
        .size = 9,
        .coverage = 4,
        .name = "ST20_FMT_YUV_420_12BIT",
    },
    {
        /* ST20_FMT_YUV_420_16BIT */
        .fmt = ST20_FMT_YUV_420_16BIT,
        .size = 12,
        .coverage = 4,
        .name = "ST20_FMT_YUV_420_16BIT",
    },
    {
        /* ST20_FMT_RGB_8BIT */
        .fmt = ST20_FMT_RGB_8BIT,
        .size = 3,
        .coverage = 1,
        .name = "ST20_FMT_RGB_8BIT",
    },
    {
        /* ST20_FMT_RGB_10BIT */
        .fmt = ST20_FMT_RGB_10BIT,
        .size = 15,
        .coverage = 4,
        .name = "ST20_FMT_RGB_10BIT",
    },
    {
        /* ST20_FMT_RGB_12BIT */
        .fmt = ST20_FMT_RGB_12BIT,
        .size = 9,
        .coverage = 2,
        .name = "ST20_FMT_RGB_12BIT",
    },
    {
        /* ST20_FMT_RGB_16BIT */
        .fmt = ST20_FMT_RGB_16BIT,
        .size = 6,
        .coverage = 1,
        .name = "ST20_FMT_RGB_16BIT",
    },
    {
        /* ST20_FMT_YUV_444_8BIT */
        .fmt = ST20_FMT_YUV_444_8BIT,
        .size = 3,
        .coverage = 1,
        .name = "ST20_FMT_YUV_444_8BIT",
    },
    {
        /* ST20_FMT_YUV_444_10BIT */
        .fmt = ST20_FMT_YUV_444_10BIT,
        .size = 15,
        .coverage = 4,
        .name = "ST20_FMT_YUV_444_10BIT",
    },
    {
        /* ST20_FMT_YUV_444_12BIT */
        .fmt = ST20_FMT_YUV_444_12BIT,
        .size = 9,
        .coverage = 2,
        .name = "ST20_FMT_YUV_444_12BIT",
    },
    {
        /* ST20_FMT_YUV_444_16BIT */
        .fmt = ST20_FMT_YUV_444_16BIT,
        .size = 6,
        .coverage = 1,
        .name = "ST20_FMT_YUV_444_16BIT",
    },
    {
        /* ST20_FMT_YUV_422_PLANAR10LE */
        .fmt = ST20_FMT_YUV_422_PLANAR10LE,
        .size = 4, /* assume PLANAR as packed now */
        .coverage = 1,
        .name = "ST20_FMT_YUV_422_PLANAR10LE",
    },
    {
        /* ST20_FMT_V210 */
        .fmt = ST20_FMT_V210,
        .size = 8,
        .coverage = 3,
        .name = "ST20_FMT_V210",
    },
};

static const struct st_fps_timing st_fps_timings[] = {
    {
        /* ST_FPS_P120 */
        .fps = ST_FPS_P120,
        .name = "120",
        .sampling_clock_rate = 90 * 1000,
        .mul = 120,
        .den = 1,
        .framerate = 120.00,
        .lower_limit = 0.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P119_88 */
        .fps = ST_FPS_P119_88,
        .name = "119.88",
        .sampling_clock_rate = 90 * 1000,
        .mul = 60000 * 2,
        .den = 1001,
        .framerate = 119.88,
        .lower_limit = 1.00,
        .upper_limit = 0.11,
    },
    {
        /* ST_FPS_P100 */
        .fps = ST_FPS_P100,
        .name = "100",
        .sampling_clock_rate = 90 * 1000,
        .mul = 100,
        .den = 1,
        .framerate = 100.00,
        .lower_limit = 1.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P60 */
        .fps = ST_FPS_P60,
        .name = "60",
        .sampling_clock_rate = 90 * 1000,
        .mul = 60,
        .den = 1,
        .framerate = 60.00,
        .lower_limit = 0.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P59_94 */
        .fps = ST_FPS_P59_94,
        .name = "59.94",
        .sampling_clock_rate = 90 * 1000,
        .mul = 60000,
        .den = 1001,
        .framerate = 59.94,
        .lower_limit = 1.00,
        .upper_limit = 0.06,
    },
    {
        /* ST_FPS_P50 */
        .fps = ST_FPS_P50,
        .name = "50",
        .sampling_clock_rate = 90 * 1000,
        .mul = 50,
        .den = 1,
        .framerate = 50.00,
        .lower_limit = 1.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P30 */
        .fps = ST_FPS_P30,
        .name = "30",
        .sampling_clock_rate = 90 * 1000,
        .mul = 30,
        .den = 1,
        .framerate = 30.00,
        .lower_limit = 0.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P29_97 */
        .fps = ST_FPS_P29_97,
        .name = "29.97",
        .sampling_clock_rate = 90 * 1000,
        .mul = 30000,
        .den = 1001,
        .framerate = 29.97,
        .lower_limit = 1.00,
        .upper_limit = 0.02,
    },
    {
        /* ST_FPS_P25 */
        .fps = ST_FPS_P25,
        .name = "25",
        .sampling_clock_rate = 90 * 1000,
        .mul = 25,
        .den = 1,
        .framerate = 25.00,
        .lower_limit = 0.00,
        .upper_limit = 1.00,
    },
    {
        /* ST_FPS_P24 */
        .fps = ST_FPS_P24,
        .name = "24",
        .sampling_clock_rate = 90 * 1000,
        .mul = 24,
        .den = 1,
        .framerate = 24.00,
        .lower_limit = 0.0,
        .upper_limit = 0.99,
    },
    {
        /* ST_FPS_P23.98 */
        .fps = ST_FPS_P23_98,
        .name = "23.98",
        .sampling_clock_rate = 90 * 1000,
        .mul = 24000,
        .den = 1001,
        .framerate = 23.98,
        .lower_limit = 1.00,
        .upper_limit = 0.01,
    },
};

static const struct st_frame_fmt_desc st_frame_fmt_descs[] = {
    {
        /* ST_FRAME_FMT_YUV422PLANAR10LE */
        .fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .name = "YUV422PLANAR10LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_V210 */
        .fmt = ST_FRAME_FMT_V210,
        .name = "V210",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_Y210 */
        .fmt = ST_FRAME_FMT_Y210,
        .name = "Y210",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV422PLANAR8 */
        .fmt = ST_FRAME_FMT_YUV422PLANAR8,
        .name = "YUV422PLANAR8",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_UYVY */
        .fmt = ST_FRAME_FMT_UYVY,
        .name = "UYVY",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV422PLANAR12LE */
        .fmt = ST_FRAME_FMT_YUV422PLANAR12LE,
        .name = "YUV422PLANAR12LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV444PLANAR10LE */
        .fmt = ST_FRAME_FMT_YUV444PLANAR10LE,
        .name = "YUV444PLANAR10LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_YUV444PLANAR12LE */
        .fmt = ST_FRAME_FMT_YUV444PLANAR12LE,
        .name = "YUV444PLANAR12LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_GBRPLANAR10LE */
        .fmt = ST_FRAME_FMT_GBRPLANAR10LE,
        .name = "GBRPLANAR10LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_GBRPLANAR12LE */
        .fmt = ST_FRAME_FMT_GBRPLANAR12LE,
        .name = "GBRPLANAR12LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_YUV422RFC4175PG2BE10 */
        .fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        .name = "YUV422RFC4175PG2BE10",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV422RFC4175PG2BE12 */
        .fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE12,
        .name = "YUV422RFC4175PG2BE12",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV444RFC4175PG4BE10 */
        .fmt = ST_FRAME_FMT_YUV444RFC4175PG4BE10,
        .name = "YUV444RFC4175PG4BE10",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_YUV444RFC4175PG2BE12 */
        .fmt = ST_FRAME_FMT_YUV444RFC4175PG2BE12,
        .name = "YUV444RFC4175PG2BE12",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_YUV420CUSTOM8 */
        .fmt = ST_FRAME_FMT_YUV420CUSTOM8,
        .name = "YUV420CUSTOM8",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_420,
    },
    {
        /* ST_FRAME_FMT_YUV422CUSTOM8 */
        .fmt = ST_FRAME_FMT_YUV422CUSTOM8,
        .name = "YUV422CUSTOM8",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_422,
    },
    {
        /* ST_FRAME_FMT_YUV420PLANAR8 */
        .fmt = ST_FRAME_FMT_YUV420PLANAR8,
        .name = "YUV420PLANAR8",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_420,
    },
    {
        /* ST_FRAME_FMT_RGBRFC4175PG4BE10 */
        .fmt = ST_FRAME_FMT_RGBRFC4175PG4BE10,
        .name = "RGBRFC4175PG4BE10",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_RGBRFC4175PG2BE12 */
        .fmt = ST_FRAME_FMT_RGBRFC4175PG2BE12,
        .name = "RGBRFC4175PG2BE12",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_ARGB */
        .fmt = ST_FRAME_FMT_ARGB,
        .name = "ARGB",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_BGRA */
        .fmt = ST_FRAME_FMT_BGRA,
        .name = "BGRA",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_RGB8 */
        .fmt = ST_FRAME_FMT_RGB8,
        .name = "RGB8",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_444,
    },
    {
        /* ST_FRAME_FMT_JPEGXS_CODESTREAM */
        .fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM,
        .name = "JPEGXS_CODESTREAM",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_MAX,
    },
    {
        /* ST_FRAME_FMT_H264_CBR_CODESTREAM */
        .fmt = ST_FRAME_FMT_H264_CBR_CODESTREAM,
        .name = "H264_CBR_CODESTREAM",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_MAX,
    },
    {
        /* ST_FRAME_FMT_H264_CODESTREAM */
        .fmt = ST_FRAME_FMT_H264_CODESTREAM,
        .name = "H264_CODESTREAM",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_MAX,
    },
    {
        /* ST_FRAME_FMT_H265_CBR_CODESTREAM */
        .fmt = ST_FRAME_FMT_H265_CBR_CODESTREAM,
        .name = "H265_CBR_CODESTREAM",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_MAX,
    },
    {
        /* ST_FRAME_FMT_H265_CODESTREAM */
        .fmt = ST_FRAME_FMT_H265_CODESTREAM,
        .name = "H265_CODESTREAM",
        .planes = 1,
        .sampling = ST_FRAME_SAMPLING_MAX,
    },
    {
        /* ST_FRAME_FMT_YUV422PLANAR16LE */
        .fmt = ST_FRAME_FMT_YUV422PLANAR16LE,
        .name = "YUV422PLANAR16LE",
        .planes = 3,
        .sampling = ST_FRAME_SAMPLING_422,
    },
};

enum st_frame_fmt st_codec_codestream_fmt(enum st22_codec codec) {
  if (codec == ST22_CODEC_JPEGXS) {
    return ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else if (codec == ST22_CODEC_H264_CBR) {
    return ST_FRAME_FMT_H264_CBR_CODESTREAM;
  } else if (codec == ST22_CODEC_H264) {
    return ST_FRAME_FMT_H264_CODESTREAM;
  } else if (codec == ST22_CODEC_H265_CBR) {
    return ST_FRAME_FMT_H265_CBR_CODESTREAM;
  } else if (codec == ST22_CODEC_H265) {
    return ST_FRAME_FMT_H265_CODESTREAM;
  }

  err("%s, unknow codec %d\n", __func__, codec);
  return ST_FRAME_FMT_MAX;
}

static const char* st_pacing_way_names[ST21_TX_PACING_WAY_MAX] = {
    "auto", "ratelimit", "tsc", "tsn", "ptp", "be", "tsc_narrow",
};

const char* st_tx_pacing_way_name(enum st21_tx_pacing_way way) {
  return st_pacing_way_names[way];
}

size_t st_frame_least_linesize(enum st_frame_fmt fmt, uint32_t width, uint8_t plane) {
  size_t linesize = 0;

  if (st_frame_fmt_planes(fmt) == 1) {
    if (plane > 0)
      err("%s, invalid plane idx %u for packed fmt\n", __func__, plane);
    else
      linesize = st_frame_size(fmt, width, 1, false);
  } else {
    switch (st_frame_fmt_get_sampling(fmt)) {
      case ST_FRAME_SAMPLING_422:
        switch (plane) {
          case 0:
            linesize = st_frame_size(fmt, width, 1, false) / 2;
            break;
          case 1:
          case 2:
            linesize = st_frame_size(fmt, width, 1, false) / 4;
            break;
          default:
            err("%s, invalid plane idx %u for 422 planar fmt\n", __func__, plane);
            break;
        }
        break;
      case ST_FRAME_SAMPLING_444:
        switch (plane) {
          case 0:
          case 1:
          case 2:
            linesize = st_frame_size(fmt, width, 1, false) / 3;
            break;
          default:
            err("%s, invalid plane idx %u for 444 planar fmt\n", __func__, plane);
            break;
        }
        break;
      case ST_FRAME_SAMPLING_420:
        switch (plane) {
          case 0:
            linesize = st_frame_size(fmt, width, 1, false) * 4 / 6;
            break;
          case 1:
          case 2:
            linesize = st_frame_size(fmt, width, 1, false) / 6;
            break;
          default:
            err("%s, invalid plane idx %u for 422 planar fmt\n", __func__, plane);
            break;
        }
        break;
      default:
        err("%s, invalid sampling for fmt %d\n", __func__, fmt);
        break;
    }
  }

  return linesize;
}

size_t st_frame_size(enum st_frame_fmt fmt, uint32_t width, uint32_t height,
                     bool interlaced) {
  size_t size = 0;
  size_t pixels = (size_t)width * height;

  switch (fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
    case ST_FRAME_FMT_YUV422PLANAR12LE:
    case ST_FRAME_FMT_Y210:
      size = pixels * 2 * 2; /* 10/12bits in two bytes */
      break;
    case ST_FRAME_FMT_V210:
      if (pixels % 3) {
        err("%s, invalid width %u height %u for v210 fmt, not multiple of 3\n", __func__,
            width, height);
      } else {
        size = pixels * 8 / 3;
      }
      break;
    case ST_FRAME_FMT_YUV422PLANAR8:
    case ST_FRAME_FMT_YUV422CUSTOM8:
    case ST_FRAME_FMT_UYVY:
      size = pixels * 2;
      break;
    case ST_FRAME_FMT_YUV444PLANAR10LE:
    case ST_FRAME_FMT_YUV444PLANAR12LE:
    case ST_FRAME_FMT_GBRPLANAR10LE:
    case ST_FRAME_FMT_GBRPLANAR12LE:
      size = pixels * 2 * 3; /* 10bits in two bytes */
      break;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
      size = st20_frame_size(ST20_FMT_YUV_422_10BIT, width, height);
      break;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE12:
      size = st20_frame_size(ST20_FMT_YUV_422_12BIT, width, height);
      break;
    case ST_FRAME_FMT_YUV444RFC4175PG4BE10:
    case ST_FRAME_FMT_RGBRFC4175PG4BE10:
      size = st20_frame_size(ST20_FMT_YUV_444_10BIT, width, height);
      break;
    case ST_FRAME_FMT_YUV444RFC4175PG2BE12:
    case ST_FRAME_FMT_RGBRFC4175PG2BE12:
      size = st20_frame_size(ST20_FMT_YUV_444_12BIT, width, height);
      break;
    case ST_FRAME_FMT_ARGB:
    case ST_FRAME_FMT_BGRA:
      size = pixels * 4; /* 8 bits ARGB pixel in a 32 bits */
      break;
    case ST_FRAME_FMT_RGB8:
      size = pixels * 3; /* 8 bits RGB pixel in a 24 bits */
      break;
    case ST_FRAME_FMT_YUV420CUSTOM8:
    case ST_FRAME_FMT_YUV420PLANAR8:
      size = st20_frame_size(ST20_FMT_YUV_420_8BIT, width, height);
      break;
    case ST_FRAME_FMT_YUV422PLANAR16LE:
      size = st20_frame_size(ST20_FMT_YUV_422_16BIT, width, height);
      break;
    default:
      err("%s, invalid fmt %d\n", __func__, fmt);
      break;
  }

  if (interlaced) size /= 2; /* if all fmt support interlace? */
  return size;
}

int st_frame_sanity_check(struct st_frame* frame) {
  RTE_BUILD_BUG_ON(ST_FRAME_FMT_MAX > 64);

  int planes = st_frame_fmt_planes(frame->fmt);
  if (planes == 0) {
    err("%s, invalid frame fmt %d\n", __func__, frame->fmt);
    return -EINVAL;
  }
  for (int plane = 0; plane < planes; plane++) {
    /* check memory address */
    if (!frame->addr[plane]) {
      err("%s, invalid frame addr[%d]\n", __func__, plane);
      return -EINVAL;
    }
    /* IOVA not mandatory for st_frame */
    if (frame->iova[plane] == 0) {
      dbg("%s, this frame doesn't have IOVA\n", __func__);
    }

    /* check linesize */
    if (frame->linesize[plane] <
        st_frame_least_linesize(frame->fmt, frame->width, plane)) {
      err("%s, invalid frame linesize[%d]: %" PRIu64 "\n", __func__, plane,
          frame->linesize[plane]);
      return -EINVAL;
    }

    /* check data size */
    if (frame->data_size > frame->buffer_size) {
      err("%s, frame data size %" PRIu64 " exceeds buffer size %" PRIu64 "\n", __func__,
          frame->data_size, frame->buffer_size);
      return -EINVAL;
    }

    /* check data size is enough */
    size_t least_sz =
        st_frame_size(frame->fmt, frame->width, frame->height, frame->interlaced);
    if (frame->data_size < least_sz) {
      err("%s, frame data size %" PRIu64 " small then frame least_sz %" PRIu64 "\n",
          __func__, frame->data_size, least_sz);
      return -EINVAL;
    }
  }

  return 0;
}

int st20_get_pgroup(enum st20_fmt fmt, struct st20_pgroup* pg) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st20_pgroups); i++) {
    if (fmt == st20_pgroups[i].fmt) {
      *pg = st20_pgroups[i];
      return 0;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return -EINVAL;
}

size_t st20_frame_size(enum st20_fmt fmt, uint32_t width, uint32_t height) {
  struct st20_pgroup pg;
  memset(&pg, 0, sizeof(pg));

  int ret = st20_get_pgroup(fmt, &pg);
  if (ret < 0) {
    err("%s, st20_get_pgroup fail %d, fmt %d\n", __func__, ret, fmt);
    return 0;
  }

  size_t size = (size_t)width * height;
  if (size % pg.coverage) {
    err("%s, fmt %d, invalid w %u h %u, not multiple of %u\n", __func__, fmt, width,
        height, pg.coverage);
    return 0;
  }

  return size * pg.size / pg.coverage;
}

const char* st20_fmt_name(enum st20_fmt fmt) {
  struct st20_pgroup pg;
  memset(&pg, 0, sizeof(pg));

  int ret = st20_get_pgroup(fmt, &pg);
  if (ret < 0) {
    err("%s, st20_get_pgroup fail %d, fmt %d\n", __func__, ret, fmt);
    return "unknown";
  }
  return pg.name;
}

enum st20_fmt st20_name_to_fmt(const char* name) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st20_pgroups); i++) {
    if (!strcmp(name, st20_pgroups[i].name)) {
      return st20_pgroups[i].fmt;
    }
  }

  err("%s, invalid name %s\n", __func__, name);
  return ST20_FMT_MAX;
}

int st_get_fps_timing(enum st_fps fps, struct st_fps_timing* fps_tm) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_fps_timings); i++) {
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

  for (i = 0; i < MTL_ARRAY_SIZE(st_fps_timings); i++) {
    if (fps == st_fps_timings[i].fps) {
      return (double)st_fps_timings[i].mul / st_fps_timings[i].den;
    }
  }

  err("%s, invalid fps %d\n", __func__, fps);
  return 0;
}

enum st_fps st_frame_rate_to_st_fps(double framerate) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_fps_timings); i++) {
    if (framerate == st_fps_timings[i].framerate ||
        ((framerate >= st_fps_timings[i].framerate - st_fps_timings[i].lower_limit) &&
         (framerate <= st_fps_timings[i].framerate + st_fps_timings[i].upper_limit))) {
      return st_fps_timings[i].fps;
    }
  }

  err("%s, invalid fps %f\n", __func__, framerate);
  return ST_FPS_MAX;
}

enum st_fps st_name_to_fps(const char* name) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_fps_timings); i++) {
    if (!strcmp(name, st_fps_timings[i].name)) {
      return st_fps_timings[i].fps;
    }
  }

  err("%s, invalid name %s\n", __func__, name);
  return ST_FPS_MAX;
}

const char* st_frame_fmt_name(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (fmt == st_frame_fmt_descs[i].fmt) {
      return st_frame_fmt_descs[i].name;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return "unknown";
}

enum st_frame_fmt st_frame_name_to_fmt(const char* name) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (!strcmp(name, st_frame_fmt_descs[i].name)) {
      return st_frame_fmt_descs[i].fmt;
    }
  }

  err("%s, invalid name %s\n", __func__, name);
  return ST_FRAME_FMT_MAX;
}

enum st22_codec st_name_to_codec(const char* name) {
  if (!strcmp(name, "jpegxs"))
    return ST22_CODEC_JPEGXS;
  else if (!strcmp(name, "h264_cbr"))
    return ST22_CODEC_H264_CBR;
  else if (!strcmp(name, "h264"))
    return ST22_CODEC_H264;
  else if (!strcmp(name, "h265_cbr"))
    return ST22_CODEC_H265_CBR;
  else if (!strcmp(name, "h265"))
    return ST22_CODEC_H265;

  err("%s, invalid name %s\n", __func__, name);
  return ST22_CODEC_MAX;
}

uint8_t st_frame_fmt_planes(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (fmt == st_frame_fmt_descs[i].fmt) {
      return st_frame_fmt_descs[i].planes;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return 0;
}

enum st_frame_sampling st_frame_fmt_get_sampling(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < MTL_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (fmt == st_frame_fmt_descs[i].fmt) {
      return st_frame_fmt_descs[i].sampling;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return ST_FRAME_SAMPLING_MAX;
}

enum st20_fmt st_frame_fmt_to_transport(enum st_frame_fmt fmt) {
  switch (fmt) {
    case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
      return ST20_FMT_YUV_422_10BIT;
    case ST_FRAME_FMT_UYVY:
    case ST_FRAME_FMT_YUV422CUSTOM8:
      return ST20_FMT_YUV_422_8BIT;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE12:
      return ST20_FMT_YUV_422_12BIT;
    case ST_FRAME_FMT_YUV444RFC4175PG4BE10:
      return ST20_FMT_YUV_444_10BIT;
    case ST_FRAME_FMT_YUV444RFC4175PG2BE12:
      return ST20_FMT_YUV_444_12BIT;
    case ST_FRAME_FMT_YUV420CUSTOM8:
      return ST20_FMT_YUV_420_8BIT;
    case ST_FRAME_FMT_RGBRFC4175PG4BE10:
      return ST20_FMT_RGB_10BIT;
    case ST_FRAME_FMT_RGBRFC4175PG2BE12:
      return ST20_FMT_RGB_12BIT;
    case ST_FRAME_FMT_RGB8:
      return ST20_FMT_RGB_8BIT;
    default:
      err("%s, invalid fmt %d\n", __func__, fmt);
      return ST20_FMT_MAX;
  }
}

enum st_frame_fmt st_frame_fmt_from_transport(enum st20_fmt tfmt) {
  switch (tfmt) {
    case ST20_FMT_YUV_422_10BIT:
      return ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    case ST20_FMT_YUV_422_8BIT:
      return ST_FRAME_FMT_UYVY;
    case ST20_FMT_YUV_422_12BIT:
      return ST_FRAME_FMT_YUV422RFC4175PG2BE12;
    case ST20_FMT_YUV_444_10BIT:
      return ST_FRAME_FMT_YUV444RFC4175PG4BE10;
    case ST20_FMT_YUV_444_12BIT:
      return ST_FRAME_FMT_YUV444RFC4175PG2BE12;
    case ST20_FMT_YUV_420_8BIT:
      return ST_FRAME_FMT_YUV420CUSTOM8;
    case ST20_FMT_RGB_10BIT:
      return ST_FRAME_FMT_RGBRFC4175PG4BE10;
    case ST20_FMT_RGB_12BIT:
      return ST_FRAME_FMT_RGBRFC4175PG2BE12;
    case ST20_FMT_RGB_8BIT:
      return ST_FRAME_FMT_RGB8;
    case ST20_FMT_YUV_422_PLANAR10LE:
      return ST_FRAME_FMT_YUV422PLANAR10LE;
    case ST20_FMT_V210:
      return ST_FRAME_FMT_V210;
    default:
      err("%s, invalid tfmt %d\n", __func__, tfmt);
      return ST_FRAME_FMT_MAX;
  }
}

bool st_frame_fmt_equal_transport(enum st_frame_fmt fmt, enum st20_fmt tfmt) {
  if (fmt == ST_FRAME_FMT_YUV422CUSTOM8 || fmt == ST_FRAME_FMT_YUV420CUSTOM8) return true;

  enum st_frame_fmt to_fmt = st_frame_fmt_from_transport(tfmt);

  if (to_fmt == ST_FRAME_FMT_MAX) return false;

  return (fmt == to_fmt) ? true : false;
}

static uint64_t st_muldiv_u64_round_closest(uint64_t value, uint64_t multiplier,
                                            uint64_t divisor) {
  /* keep conversions reproducible without relying on floating point */
  __uint128_t product = (__uint128_t)value * multiplier;
  __uint128_t quotient = product / divisor;
  __uint128_t remainder = product - quotient * divisor;
  __uint128_t half = divisor / 2;

  if (remainder > half) quotient++; /* ties round down to keep jitter bounded */

  return (uint64_t)quotient;
}

uint32_t st10_tai_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate) {
  if (!sampling_rate) {
    err("%s, invalid sampling rate\n", __func__);
    return 0;
  }

  return (uint32_t)st_muldiv_u64_round_closest(tai_ns, sampling_rate, NS_PER_S);
}

uint64_t st10_media_clk_to_ns(uint32_t media_ts, uint32_t sampling_rate) {
  if (!sampling_rate) {
    err("%s, invalid sampling rate\n", __func__);
    return 0;
  }

  return st_muldiv_u64_round_closest(media_ts, NS_PER_S, sampling_rate);
}

int st_draw_logo(struct st_frame* frame, struct st_frame* logo, uint32_t x, uint32_t y) {
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
    void* dst = frame->addr[0] + (((col + y) * frame->width) + x) / 2 * 5;
    void* src = logo->addr[0] + (col * logo->width) / 2 * 5;
    mtl_memcpy(dst, src, logo_col_size);
  }

  return 0;
}

int st20_get_bandwidth_bps(int width, int height, enum st20_fmt fmt, enum st_fps fps,
                           bool interlaced, uint64_t* bps) {
  struct st20_pgroup pg;
  struct st_fps_timing fps_tm;
  int ret;
  double traffic;

  memset(&pg, 0, sizeof(pg));
  memset(&fps_tm, 0, sizeof(fps_tm));

  ret = st20_get_pgroup(fmt, &pg);
  if (ret < 0) return ret;

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double reactive = 1080.0 / 1125.0;
  traffic =
      (uint64_t)width * height * 8 * pg.size / pg.coverage * fps_tm.mul / fps_tm.den;
  if (interlaced) traffic /= 2;
  traffic = traffic / reactive;

  *bps = traffic;
  return 0;
}

int st22_rtp_bandwidth_bps(uint32_t total_pkts, uint16_t pkt_size, enum st_fps fps,
                           uint64_t* bps) {
  struct st_fps_timing fps_tm;
  int ret;

  memset(&fps_tm, 0, sizeof(fps_tm));

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double reactive = 1080.0 / 1125.0;
  *bps = (uint64_t)total_pkts * pkt_size * fps_tm.mul / fps_tm.den;
  *bps = (double)*bps / reactive;
  return 0;
}

int st22_frame_bandwidth_bps(size_t frame_size, enum st_fps fps, uint64_t* bps) {
  struct st_fps_timing fps_tm;
  int ret;

  memset(&fps_tm, 0, sizeof(fps_tm));

  ret = st_get_fps_timing(fps, &fps_tm);
  if (ret < 0) return ret;

  double reactive = 1080.0 / 1125.0;
  *bps = frame_size * fps_tm.mul / fps_tm.den;
  *bps = (double)*bps / reactive;
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
    case ST30_PTIME_250US:
      packet_time_ns = (double)1000000000.0 * 1 / 4000;
      break;
    case ST30_PTIME_333US:
      packet_time_ns = (double)1000000000.0 * 1 / 3000;
      break;
    case ST30_PTIME_4MS:
      packet_time_ns = (double)1000000000.0 * 4 / 1000;
      break;
    case ST31_PTIME_80US:
      packet_time_ns = (double)1000000000.0 * 1 / 12500;
      break;
    case ST31_PTIME_1_09MS:
      packet_time_ns = (double)1000000000.0 * 48 / 44100;
      break;
    case ST31_PTIME_0_14MS:
      packet_time_ns = (double)1000000000.0 * 6 / 44100;
      break;
    case ST31_PTIME_0_09MS:
      packet_time_ns = (double)1000000000.0 * 4 / 44100;
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
        case ST30_PTIME_250US:
          samples = 12;
          break;
        case ST30_PTIME_333US:
          samples = 16;
          break;
        case ST30_PTIME_4MS:
          samples = 192;
          break;
        case ST31_PTIME_80US:
          samples = 4;
          break;
        default:
          err("%s, wrong ptime %d for 48k\n", __func__, ptime);
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
        case ST30_PTIME_250US:
          samples = 24;
          break;
        case ST30_PTIME_333US:
          samples = 32;
          break;
        case ST30_PTIME_4MS:
          samples = 384;
          break;
        case ST31_PTIME_80US:
          samples = 8;
          break;
        default:
          err("%s, wrong ptime %d for 96k\n", __func__, ptime);
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
          err("%s, wrong ptime %d for 44k\n", __func__, ptime);
          return -EINVAL;
      }
      break;
    default:
      err("%s, wrong sampling %d\n", __func__, sampling);
      return -EINVAL;
  }
  return samples;
}

int st30_get_sample_rate(enum st30_sampling sampling) {
  switch (sampling) {
    case ST30_SAMPLING_48K:
      return 48000;
    case ST30_SAMPLING_96K:
      return 96000;
    case ST31_SAMPLING_44K:
      return 44100;
    default:
      err("%s, wrong sampling %d\n", __func__, sampling);
      return -EINVAL;
  }
}

int st30_get_packet_size(enum st30_fmt fmt, enum st30_ptime ptime,
                         enum st30_sampling sampling, uint16_t channel) {
  int ret;
  int sample_size;
  int sample_num;

  ret = st30_get_sample_size(fmt);
  if (ret < 0) return ret;
  sample_size = ret;

  ret = st30_get_sample_num(ptime, sampling);
  if (ret < 0) return ret;
  sample_num = ret;

  if (!channel) {
    err("%s, invalid channel %u\n", __func__, channel);
    return -EINVAL;
  }

  return sample_size * sample_num * channel;
}

int st30_calculate_framebuff_size(enum st30_fmt fmt, enum st30_ptime ptime,
                                  enum st30_sampling sampling, uint16_t channel,
                                  uint64_t desired_frame_time_ns, double* fps) {
  /* count frame size */
  int pkt_per_frame = 1;
  int pkt_len = st30_get_packet_size(fmt, ptime, sampling, channel);
  double pkt_time = st30_get_packet_time(ptime);
  double frame_time = desired_frame_time_ns;
  /* set frame time to desired frame time */
  if (pkt_time < frame_time) {
    pkt_per_frame = frame_time / pkt_time;
  }
  if (fps) {
    *fps = (double)NS_PER_S / pkt_time / pkt_per_frame;
  }
  uint32_t framebuff_size = pkt_per_frame * pkt_len;
  return framebuff_size;
}

void st_frame_init_plane_single_src(struct st_frame* frame, void* addr, mtl_iova_t iova) {
  uint8_t planes = st_frame_fmt_planes(frame->fmt);

  for (uint8_t plane = 0; plane < planes; plane++) {
    frame->linesize[plane] = st_frame_least_linesize(frame->fmt, frame->width, plane);
    if (plane == 0) {
      frame->addr[plane] = addr;
      frame->iova[plane] = iova;
    } else {
      frame->addr[plane] = frame->addr[plane - 1] +
                           frame->linesize[plane - 1] * st_frame_data_height(frame);
      frame->iova[plane] = frame->iova[plane - 1] +
                           frame->linesize[plane - 1] * st_frame_data_height(frame);
    }
  }
}

struct st_frame* st_frame_create(mtl_handle mt, enum st_frame_fmt fmt, uint32_t w,
                                 uint32_t h, bool interlaced) {
  struct mtl_main_impl* impl = mt;
  int soc_id = mt_socket_id(impl, MTL_PORT_P);
  struct st_frame* frame = mt_rte_zmalloc_socket(sizeof(*frame), soc_id);
  if (!frame) {
    err("%s, frame malloc fail\n", __func__);
    return NULL;
  }
  frame->fmt = fmt;
  frame->interlaced = interlaced;
  frame->width = w;
  frame->height = h;
  frame->flags = ST_FRAME_FLAG_SINGLE_MALLOC | ST_FRAME_FLAG_RTE_MALLOC;

  size_t data_sz = st_frame_size(fmt, w, h, interlaced);
  void* data = mt_rte_zmalloc_socket(data_sz, soc_id);
  if (!data) {
    err("%s, data malloc fail, size %" PRIu64 "\n", __func__, data_sz);
    st_frame_free(frame);
    return NULL;
  }
  frame->buffer_size = data_sz;
  frame->data_size = data_sz;
  /* init plane */
  st_frame_init_plane_single_src(frame, data, mtl_hp_virt2iova(impl, data));
  return frame;
}

struct st_frame* st_frame_create_by_malloc(enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                                           bool interlaced) {
  struct st_frame* frame = mt_zmalloc(sizeof(*frame));
  if (!frame) {
    err("%s, frame malloc fail\n", __func__);
    return NULL;
  }
  frame->fmt = fmt;
  frame->interlaced = interlaced;
  frame->width = w;
  frame->height = h;
  frame->flags = ST_FRAME_FLAG_SINGLE_MALLOC;

  size_t data_sz = st_frame_size(fmt, w, h, interlaced);
  void* data = mt_zmalloc(data_sz);
  if (!data) {
    err("%s, data malloc fail, size %" PRIu64 "\n", __func__, data_sz);
    st_frame_free(frame);
    return NULL;
  }
  frame->buffer_size = data_sz;
  frame->data_size = data_sz;
  /* init plane */
  st_frame_init_plane_single_src(frame, data, 0);
  return frame;
}

int st_frame_free(struct st_frame* frame) {
  if (!(frame->flags & ST_FRAME_FLAG_SINGLE_MALLOC)) {
    err("%s, frame %p is not created by ST_FRAME_FLAG_SINGLE_MALLOC\n", __func__, frame);
    return -EINVAL;
  }
  if (frame->flags & ST_FRAME_FLAG_RTE_MALLOC) {
    if (frame->addr[0]) {
      mt_rte_free(frame->addr[0]);
    }
    mt_rte_free(frame);
  } else {
    if (frame->addr[0]) {
      mt_free(frame->addr[0]);
    }
    mt_free(frame);
  }
  return 0;
}

/* the reference rl pad interval table for CVL NIC */
struct cvl_pad_table {
  enum st20_fmt fmt;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st20_packing packing;
  bool interlaced;
  uint16_t pad_interval;
};

static const struct cvl_pad_table g_cvl_static_pad_tables[] = {
    {
        /* 1080i50 gpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
        .packing = ST20_PACKING_GPM,
        .interlaced = true,
        .pad_interval = 155, /* measured with VERO avg vrx: 6.0 */
    },
    {
        /* 1080i50 bpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
        .packing = ST20_PACKING_BPM,
        .interlaced = true,
        .pad_interval = 268, /* measured with VERO avg vrx: 6.0 */
    },
    {
        /* 1080p50 gpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
        .packing = ST20_PACKING_GPM,
        .interlaced = false,
        .pad_interval = 156, /* measured with VERO avg vrx: 6.0 */
    },
    {
        /* 1080p50 bpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P50,
        .packing = ST20_PACKING_BPM,
        .interlaced = false,
        .pad_interval = 254, /* measured with VERO avg vrx: 6.0 */
    },
    {
        /* 1080p59 gpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
        .packing = ST20_PACKING_GPM,
        .interlaced = false,
        .pad_interval = 160, /* measured with VERO avg vrx: 6.0 */
    },
    {
        /* 1080p59 bpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
        .packing = ST20_PACKING_BPM,
        .interlaced = false,
        .pad_interval = 262, /* measured with VERO avg vrx: 7.0, narrow vrx: 9 */
    },
    {
        /* 4kp50 gpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920 * 2,
        .height = 1080 * 2,
        .fps = ST_FPS_P50,
        .packing = ST20_PACKING_GPM,
        .interlaced = false,
        .pad_interval = 144, /* measured with VERO uniform distribution */
    },
    {
        /* 4kp50 bpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920 * 2,
        .height = 1080 * 2,
        .fps = ST_FPS_P59_94,
        .packing = ST20_PACKING_BPM,
        .interlaced = false,
        .pad_interval = 215, /* measured with VERO uniform distribution */
    },
    {
        /* 4kp59 gpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920 * 2,
        .height = 1080 * 2,
        .fps = ST_FPS_P59_94,
        .packing = ST20_PACKING_GPM,
        .interlaced = false,
        .pad_interval = 145, /* measured with VERO uniform distribution */
    },
    {
        /* 4kp59 bpm */
        .fmt = ST20_FMT_YUV_422_10BIT,
        .width = 1920 * 2,
        .height = 1080 * 2,
        .fps = ST_FPS_P59_94,
        .packing = ST20_PACKING_BPM,
        .interlaced = false,
        .pad_interval = 217, /* measured with VERO uniform distribution */
    },
};

uint16_t st20_pacing_static_profiling(struct mtl_main_impl* impl,
                                      struct st_tx_video_session_impl* s,
                                      enum mtl_session_port s_port) {
  const struct cvl_pad_table* refer;
  struct st20_tx_ops* ops = &s->ops;
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(s_port);

  if (s->s_type == MT_ST22_HANDLE_TX_VIDEO) return 0; /* no for st22 */

  for (int i = 0; i < MTL_ARRAY_SIZE(g_cvl_static_pad_tables); i++) {
    refer = &g_cvl_static_pad_tables[i];
    if ((ops->fmt == refer->fmt) && (ops->width == refer->width) &&
        (ops->height == refer->height) && (ops->fps == refer->fps) &&
        (ops->packing == refer->packing) && (ops->interlaced == refer->interlaced)) {
      dbg("%s(%d), reference pad_interval %u\n", __func__, s->idx, refer->pad_interval);
      return refer->pad_interval;
    }
  }

  return 0; /* not found */
}

int st_rxp_para_port_set(struct st_rx_port* p, enum mtl_session_port port, char* name) {
  return snprintf(p->port[port], MTL_PORT_MAX_LEN, "%s", name);
}

int st_rxp_para_ip_set(struct st_rx_port* p, enum mtl_port port, char* ip) {
  int ret = inet_pton(AF_INET, ip, p->ip_addr[port]);
  if (ret == 1) return 0;
  err("%s, fail to inet_pton for %s\n", __func__, ip);
  return -EIO;
}

int st_txp_para_port_set(struct st_tx_port* p, enum mtl_session_port port, char* name) {
  return snprintf(p->port[port], MTL_PORT_MAX_LEN, "%s", name);
}

int st_txp_para_dip_set(struct st_tx_port* p, enum mtl_port port, char* ip) {
  int ret = inet_pton(AF_INET, ip, p->dip_addr[port]);
  if (ret == 1) return 0;
  err("%s, fail to inet_pton for %s\n", __func__, ip);
  return -EIO;
}
