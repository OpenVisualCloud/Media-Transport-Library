/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_format.h - application formats, format descriptions and standalone conversion,
 * revision 0.2.
 *
 * mtl.h names the ST 2110-20 transport formats; a session's wire bandwidth is
 * mtl_session_info.wire_bps (mtl_session_query, mtl_session_get_info). An application that
 * wants frames in another layout sets mtl_video_config.app_format (or
 * mtl_cvideo_config.app_format for a codec plugin) to a value below and MTL converts, in
 * the caller or a library worker, never on a tasklet; the granted path is reported
 * (MTL_INFO_DIRECT, MTL_TXR_COPIED). mtl_convert() runs the same SIMD converters outside a
 * session (file tools, test pattern generators): one call replaces today's ~105 per-pair
 * functions; the pair, the CPU features and an optional DMA engine are chosen inside. Pure
 * functions on the caller's thread: no instance is needed unless DMA is wanted.
 */

#ifndef MTL_EXPERIMENTAL_MTL_FORMAT_H
#define MTL_EXPERIMENTAL_MTL_FORMAT_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Application layouts; 0 = the transport format itself. Values match today's
   enum st_frame_fmt + 1 where one exists, so migration tables are mechanical. */
enum mtl_app_format {
  MTL_APP_YUV422P10LE = 1,
  MTL_APP_V210 = 2,
  MTL_APP_Y210 = 3,
  MTL_APP_YUV422P8 = 4,
  MTL_APP_UYVY = 5,
  MTL_APP_YUV422_PG2_BE10 = 6, /* RFC 4175 pixel groups, as on the wire */
  MTL_APP_YUV422P12LE = 7,
  MTL_APP_YUV422_PG2_BE12 = 8,
  MTL_APP_YUV444P10LE = 9,
  MTL_APP_YUV444_PG4_BE10 = 10,
  MTL_APP_YUV444P12LE = 11,
  MTL_APP_YUV444_PG2_BE12 = 12,
  MTL_APP_YUV420_CUSTOM8 = 13, /* pass-through containers for app-defined 8-bit data */
  MTL_APP_YUV422_CUSTOM8 = 14,
  MTL_APP_YUV420P8 = 15, /* I420 */
  MTL_APP_YUV422P16LE = 16,
  MTL_APP_NV12 = 17,
  MTL_APP_ARGB = 33,
  MTL_APP_BGRA = 34,
  MTL_APP_RGB8 = 35,
  MTL_APP_GBRP10LE = 36,
  MTL_APP_RGB_PG4_BE10 = 37,
  MTL_APP_GBRP12LE = 38,
  MTL_APP_RGB_PG2_BE12 = 39,
  MTL_APP_RGBA = 40,
};

/* ST 2110-20 colorimetry, TCS and RANGE (§7.5, §7.6, §7.3), in mtl_video_config and
   mtl_cvideo_config: SDP and conversion matrices only, never on the wire. 0 = colorimetry
   UNSPECIFIED (rendered, since §7.2 requires the parameter), TCS SDR, RANGE NARROW.
   FULLPROTECT with BT2100 is -MTL_EINVAL at create. */
enum mtl_colorimetry {
  MTL_COLOR_UNSPECIFIED = 0,
  MTL_COLOR_BT601 = 1,
  MTL_COLOR_BT709 = 2,
  MTL_COLOR_BT2020 = 3,
  MTL_COLOR_BT2100 = 4,
  MTL_COLOR_ST2065_1 = 5,
  MTL_COLOR_ST2065_3 = 6,
  MTL_COLOR_XYZ = 7,
#if defined(MTL_LATER)
  MTL_COLOR_ALPHA = 8, /* only with KEY sampling (ST 2110-20 §6.2.6, §7.4.1), which has no
                          transport format here; until one exists, 8 is -MTL_EINVAL (later) */
#endif
};
enum mtl_tcs {
  MTL_TCS_SDR = 0,
  MTL_TCS_PQ = 1,
  MTL_TCS_HLG = 2,
  MTL_TCS_LINEAR = 3,
  MTL_TCS_BT2100LINPQ = 4,
  MTL_TCS_BT2100LINHLG = 5,
  MTL_TCS_ST2065_1 = 6,
  MTL_TCS_ST428_1 = 7,
  MTL_TCS_DENSITY = 8,
  MTL_TCS_ST2115LOGS3 = 9,
  MTL_TCS_UNSPECIFIED = 10,
};
enum mtl_range {
  MTL_RANGE_NARROW = 0,
  MTL_RANGE_FULLPROTECT = 1,
  MTL_RANGE_FULL = 2,
};

/* Transport layouts outside RFC 4175 (non-compliant on the wire; kept for existing
   deployments, reported with MTL_INFO_NON_COMPLIANT; their removal needs approval). */
enum mtl_video_format_ext { /* enum st20_fmt + 1, like mtl_video_format */
  MTL_YUV422P10LE_NONSTD = 17, /* ST20_FMT_YUV_422_PLANAR10LE */
  MTL_V210_NONSTD = 18,        /* ST20_FMT_V210 */
};

/* Conversion-only audio layout (mtl_convert); enum mtl_audio_format (mtl.h) has the rest. */
enum mtl_audio_format_ext {
  MTL_AES3 = 16, /* AES3 subframes, 32 bits each */
};

/* The number space a format value belongs to (values overlap between kinds). */
enum mtl_format_kind {
  MTL_FORMAT_TRANSPORT = 0, /* enum mtl_video_format, enum mtl_video_format_ext */
  MTL_FORMAT_APP = 1,       /* enum mtl_app_format */
  MTL_FORMAT_CODEC = 2,     /* enum mtl_codec */
  MTL_FORMAT_AUDIO = 3,     /* enum mtl_audio_format, enum mtl_audio_format_ext */
};

/* ---- Format descriptions ----------------------------------------------------------- */

/* Output; the size argument versions it. Names are static, NULL where none exists. */
struct mtl_format_desc {
  const char* name;       /* "yuv422_10", "v210", "jpegxs", "pcm24" */
  const char* sdp;        /* "YCbCr-4:2:2/10"; codecs: the encoding name, "jxsv" */
  const char* gst;        /* "v210", "UYVY", ... */
  const char* av_pix_fmt; /* "yuv422p10le", ... */
  uint32_t fourcc;
  uint32_t plane_count;
  uint32_t pg_bytes;  /* transport: RFC 4175 pixel group bytes; audio: bytes per sample */
  uint32_t pg_pixels; /* transport: pixels per pixel group; audio: 1 */
  uint32_t row_bytes[MTL_MAX_PLANES];   /* per plane, for `width`; 0 when width is 0 */
  uint32_t rows[MTL_MAX_PLANES];        /* per plane, for `height` */
  uint64_t plane_bytes[MTL_MAX_PLANES]; /* one frame, per plane */
};
/* A format of `kind` (enum mtl_format_kind): names, pixel group, and the layout of one
   frame of width x height (0 x 0: names and pixel group only). -MTL_EINVAL for an
   unknown format. AS. (MS1) */
MTL_API_AS(1) int mtl_format_describe(uint32_t format, uint32_t kind, uint32_t width,
                                      uint32_t height, struct mtl_format_desc* out,
                                      size_t size);
/* The reverse: a name, SDP string, codec name or FFmpeg/GStreamer name to a format and
   its kind. AS. (MS2) */
MTL_API_AS(2) int mtl_format_parse(const char* name, uint32_t* format, uint32_t* kind);
/* Bytes of `samples` samples of an audio config (all channels). */
static inline int mtl_audio_bytes(const struct mtl_audio_config* a, uint32_t samples,
                                  uint64_t* bytes) {
  uint32_t b = a->format == MTL_PCM8    ? 1
               : a->format == MTL_PCM16 ? 2
               : a->format == MTL_PCM24 ? 3
               : a->format == MTL_AM824 ? 4
                                        : 0;
  if (b == 0 || a->channels == 0) return -MTL_EINVAL;
  *bytes = (uint64_t)samples * a->channels * b;
  return 0;
}

/* ---- Standalone conversion ----------------------------------------------------------- */

struct mtl_convert_image {
  uint32_t format;   /* of `kind` */
  uint32_t kind;     /* enum mtl_format_kind: TRANSPORT, APP or AUDIO */
  uint32_t plane_count;
  uint32_t channels; /* audio: interleaved channels; video: 0 */
  struct mtl_plane plane[MTL_MAX_PLANES]; /* audio: one plane */
};
#define MTL_CONVERT_NO_SIMD 0x1u     /* scalar reference path (tests) */
#define MTL_CONVERT_FIELD_SPLIT 0x2u /* a frame in, two fields out (dst planes 0-1, 2-3) */
#define MTL_CONVERT_FIELD_MERGE 0x4u /* two fields in, one woven frame out */
#define MTL_CONVERT_HALF_SCALE 0x8u  /* downsample by 2 in both directions */
#define MTL_CONVERT_CHECK 0x10u      /* only whether MTL converts the pair (a session too):
                                        0 or -MTL_ENOTSUP; the planes are not read */
#define MTL_AUDIO_SILENT 0xffffu     /* channel_map: a silent channel */
/* Video: width x height pixels. Audio (AM824 <-> AES3, or a channel remap within one
   format): width = samples per channel, height = 1. */
struct mtl_convert_desc {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t reserved0;
  struct mtl_convert_image src;
  struct mtl_convert_image dst;
  uint64_t flags;    /* MTL_CONVERT_* */
  mtl_instance_h mt; /* null = CPU only; else may use the instance's DMA engines */
  /* Audio: dst channel i = src channel channel_map[i], or MTL_AUDIO_SILENT; dst.channels
     entries (IS-08 maps, applied by the application at a media index). NULL = the same
     order. A later capability: -MTL_ENOTSUP (NOT_IMPLEMENTED) until then. */
  const uint16_t* MTL_NULLABLE channel_map;
  uint64_t reserved[3];
};
/* -MTL_ENOTSUP for a pair MTL does not convert. DPC. (MS4) */
MTL_API_DPC(4) int mtl_convert(const struct mtl_convert_desc* d);

/* ST 2110-31 AM824 subframe: byte 0 = B (block start) bit 5, F (frame start) bit 4,
   V, U, C, P bits 3-0; bytes 1-3 = the 24-bit sample, big endian. */
struct mtl_am824_subframe {
  uint8_t flags;
  uint8_t sample[3];
};

MTL_SIZE_CHECK(mtl_format_desc, 112);
MTL_SIZE_CHECK(mtl_convert_image, 112);
MTL_SIZE_CHECK(mtl_convert_desc, 288);
MTL_SIZE_CHECK(mtl_am824_subframe, 4);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_FORMAT_H */
