/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_fmt.h"

#include "st_log.h"
#include "st_main.h"

#ifdef ST_HAS_AVX2
#include "st_avx2.h"
#endif

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
        /* ST_FPS_P120 */
        .fps = ST_FPS_P120,
        .sampling_clock_rate = 90 * 1000,
        .mul = 120,
        .den = 1,
    },
    {
        /* ST_FPS_P119_88 */
        .fps = ST_FPS_P119_88,
        .sampling_clock_rate = 90 * 1000,
        .mul = 60000 * 2,
        .den = 1001,
    },
    {
        /* ST_FPS_P100 */
        .fps = ST_FPS_P100,
        .sampling_clock_rate = 90 * 1000,
        .mul = 100,
        .den = 1,
    },
    {
        /* ST_FPS_P60 */
        .fps = ST_FPS_P60,
        .sampling_clock_rate = 90 * 1000,
        .mul = 60,
        .den = 1,
    },
    {
        /* ST_FPS_P59_94 */
        .fps = ST_FPS_P59_94,
        .sampling_clock_rate = 90 * 1000,
        .mul = 60000,
        .den = 1001,
    },
    {
        /* ST_FPS_P50 */
        .fps = ST_FPS_P50,
        .sampling_clock_rate = 90 * 1000,
        .mul = 50,
        .den = 1,
    },
    {
        /* ST_FPS_P30 */
        .fps = ST_FPS_P30,
        .sampling_clock_rate = 90 * 1000,
        .mul = 30,
        .den = 1,
    },
    {
        /* ST_FPS_P29_97 */
        .fps = ST_FPS_P29_97,
        .sampling_clock_rate = 90 * 1000,
        .mul = 30000,
        .den = 1001,
    },
    {
        /* ST_FPS_P25 */
        .fps = ST_FPS_P25,
        .sampling_clock_rate = 90 * 1000,
        .mul = 25,
        .den = 1,
    },
    {
        /* ST_FPS_P24 */
        .fps = ST_FPS_P24,
        .sampling_clock_rate = 90 * 1000,
        .mul = 24,
        .den = 1,
    },
    {
        /* ST_FPS_P23.98 */
        .fps = ST_FPS_P23_98,
        .sampling_clock_rate = 90 * 1000,
        .mul = 24000,
        .den = 1001,
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
        /* ST_FRAME_FMT_YUV422PACKED8 */
        .fmt = ST_FRAME_FMT_YUV422PACKED8,
        .name = "YUV422PACKED8",
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
};

static const char* st_pacing_way_names[ST21_TX_PACING_WAY_MAX] = {
    "auto", "ratelimit", "tsc", "tsn", "ptp",
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
      linesize = st_frame_size(fmt, width, 1);
  } else {
    switch (st_frame_fmt_get_sampling(fmt)) {
      case ST_FRAME_SAMPLING_422:
        switch (plane) {
          case 0:
            linesize = st_frame_size(fmt, width, 1) / 2;
            break;
          case 1:
          case 2:
            linesize = st_frame_size(fmt, width, 1) / 4;
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
            linesize = st_frame_size(fmt, width, 1) / 3;
            break;
          default:
            err("%s, invalid plane idx %u for 444 planar fmt\n", __func__, plane);
            break;
        }
        break;
      case ST_FRAME_SAMPLING_420:
        switch (plane) {
          case 0:
            linesize = st_frame_size(fmt, width, 1) * 4 / 6;
            break;
          case 1:
          case 2:
            linesize = st_frame_size(fmt, width, 1) / 6;
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

size_t st_frame_size(enum st_frame_fmt fmt, uint32_t width, uint32_t height) {
  size_t size = 0;
  size_t pixels = width * height;

  switch (fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
    case ST_FRAME_FMT_YUV422PLANAR12LE:
    case ST_FRAME_FMT_Y210:
      size = pixels * 2 * 2; /* 10/12bits in two bytes */
      break;
    case ST_FRAME_FMT_V210:
      size = pixels * 8 / 3;
      break;
    case ST_FRAME_FMT_YUV422PLANAR8:
    case ST_FRAME_FMT_YUV422PACKED8:
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
    default:
      err("%s, invalid fmt %d\n", __func__, fmt);
      break;
  }

  return size;
}

int st_frame_sanity_check(struct st_frame* frame) {
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
  }

  return 0;
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

size_t st20_frame_size(enum st20_fmt fmt, uint32_t width, uint32_t height) {
  struct st20_pgroup pg;
  int ret = st20_get_pgroup(fmt, &pg);
  if (ret < 0) {
    err("%s, st20_get_pgroup fail %d\n", __func__, ret);
    return 0;
  }

  return (size_t)width * height * pg.size / pg.coverage;
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
      return (double)st_fps_timings[i].mul / st_fps_timings[i].den;
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

uint8_t st_frame_fmt_planes(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st_frame_fmt_descs); i++) {
    if (fmt == st_frame_fmt_descs[i].fmt) {
      return st_frame_fmt_descs[i].planes;
    }
  }

  err("%s, invalid fmt %d\n", __func__, fmt);
  return 0;
}

enum st_frame_sampling st_frame_fmt_get_sampling(enum st_frame_fmt fmt) {
  int i;

  for (i = 0; i < ST_ARRAY_SIZE(st_frame_fmt_descs); i++) {
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
    case ST_FRAME_FMT_YUV422PACKED8:
      return ST20_FMT_YUV_422_8BIT;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE12:
      return ST20_FMT_YUV_422_12BIT;
    case ST_FRAME_FMT_YUV444RFC4175PG4BE10:
      return ST20_FMT_YUV_444_10BIT;
    case ST_FRAME_FMT_YUV444RFC4175PG2BE12:
      return ST20_FMT_YUV_444_12BIT;
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
      return ST_FRAME_FMT_YUV422PACKED8;
    case ST20_FMT_YUV_422_12BIT:
      return ST_FRAME_FMT_YUV422RFC4175PG2BE12;
    case ST20_FMT_YUV_444_10BIT:
      return ST_FRAME_FMT_YUV444RFC4175PG4BE10;
    case ST20_FMT_YUV_444_12BIT:
      return ST_FRAME_FMT_YUV444RFC4175PG2BE12;
    case ST20_FMT_RGB_10BIT:
      return ST_FRAME_FMT_RGBRFC4175PG4BE10;
    case ST20_FMT_RGB_12BIT:
      return ST_FRAME_FMT_RGBRFC4175PG2BE12;
    case ST20_FMT_RGB_8BIT:
      return ST_FRAME_FMT_RGB8;
    default:
      err("%s, invalid tfmt %d\n", __func__, tfmt);
      return ST_FRAME_FMT_MAX;
  }
}

bool st_frame_fmt_equal_transport(enum st_frame_fmt fmt, enum st20_fmt tfmt) {
  enum st_frame_fmt to_fmt = st_frame_fmt_from_transport(tfmt);

  if (to_fmt == ST_FRAME_FMT_MAX) return false;

  return (fmt == to_fmt) ? true : false;
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

enum st_pmd_type st_pmd_by_port_name(const char* port) {
  char* bdf = strstr(port, ":");
  return bdf ? ST_PMD_DPDK_USER : ST_PMD_DPDK_AF_XDP;
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

#ifdef ST_HAS_AVX2
  if ((level >= ST_SIMD_LEVEL_AVX2) && (cpu_level >= ST_SIMD_LEVEL_AVX2)) {
    dbg("%s, avx2 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_422le10_avx2(pg_be, pg_le, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx2 ways failed\n", __func__);
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

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_vbmi(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx512(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX2
  if ((level >= ST_SIMD_LEVEL_AVX2) && (cpu_level >= ST_SIMD_LEVEL_AVX2)) {
    dbg("%s, avx2 ways\n", __func__);
    ret = st20_rfc4175_422le10_to_422be10_avx2(pg_le, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx2 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422le10_to_422be10_scalar(pg_le, pg_be, w, h);
}

int st20_rfc4175_422le10_to_422be10_simd_dma(st_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             st_iova_t pg_le_iova,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
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
    ret = st20_rfc4175_422le10_to_422be10_avx512_vbmi_dma(dma, pg_le, pg_le_iova, pg_be,
                                                          w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
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

#ifdef ST_HAS_AVX512_VBMI2
  if ((level >= ST_SIMD_LEVEL_AVX512_VBMI2) &&
      (cpu_level >= ST_SIMD_LEVEL_AVX512_VBMI2)) {
    dbg("%s, avx512_vbmi ways\n", __func__);
    ret = st20_v210_to_rfc4175_422be10_avx512_vbmi(pg_v210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_v210_to_rfc4175_422be10_avx512(pg_v210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_v210_to_rfc4175_422be10_scalar(pg_v210, (uint8_t*)pg_be, w, h);
}

int st20_v210_to_rfc4175_422be10_simd_dma(st_udma_handle udma, uint8_t* pg_v210,
                                          st_iova_t pg_v210_iova,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
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
    ret = st20_v210_to_rfc4175_422be10_avx512_vbmi_dma(udma, pg_v210, pg_v210_iova, pg_be,
                                                       w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512_vbmi ways failed\n", __func__);
  }
#endif

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
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
                                      enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_rfc4175_422be10_to_y210_avx512(pg_be, pg_y210, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_rfc4175_422be10_to_y210_scalar(pg_be, pg_y210, w, h);
}

int st20_rfc4175_422be10_to_y210_simd_dma(st_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          st_iova_t pg_be_iova, uint16_t* pg_y210,
                                          uint32_t w, uint32_t h,
                                          enum st_simd_level level) {
  struct st_dma_lender_dev* dma = udma;
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);
  ST_MAY_UNUSED(dma);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
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
                                      uint32_t w, uint32_t h, enum st_simd_level level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  int ret;

  ST_MAY_UNUSED(cpu_level);
  ST_MAY_UNUSED(ret);

#ifdef ST_HAS_AVX512
  if ((level >= ST_SIMD_LEVEL_AVX512) && (cpu_level >= ST_SIMD_LEVEL_AVX512)) {
    dbg("%s, avx512 ways\n", __func__);
    ret = st20_y210_to_rfc4175_422be10_avx512(pg_y210, pg_be, w, h);
    if (ret == 0) return 0;
    dbg("%s, avx512 ways failed\n", __func__);
  }
#endif

  /* the last option */
  return st20_y210_to_rfc4175_422be10_scalar(pg_y210, pg_be, w, h);
}

int st20_y210_to_rfc4175_422be10_simd_dma(st_udma_handle udma, uint16_t* pg_y210,
                                          st_iova_t pg_y210_iova,
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
                                             enum st_simd_level level) {
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
                                             enum st_simd_level level) {
  /* the only option */
  return st20_rfc4175_422be12_to_yuv422p12le_scalar(pg, y, b, r, w, h);
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
                                         enum st_simd_level level) {
  /* the only option */
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
                                         enum st_simd_level level) {
  /* the only option */
  return st20_rfc4175_422le12_to_422be12_scalar(pg_le, pg_be, w, h);
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
                                          enum st_simd_level level) {
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
                                          enum st_simd_level level) {
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
                                         enum st_simd_level level) {
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
                                         enum st_simd_level level) {
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
                                          enum st_simd_level level) {
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
                                          enum st_simd_level level) {
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
                                         enum st_simd_level level) {
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
                                         enum st_simd_level level) {
  /* the only option */
  return st20_rfc4175_444le12_to_444be12_scalar(pg_le, pg_be, w, h);
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
