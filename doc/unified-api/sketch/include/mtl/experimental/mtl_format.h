/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_format.h - application pixel formats and format helpers, revision 0.2.
 *
 * mtl.h names the ST 2110-20 transport formats. An application that wants frames in
 * another layout sets mtl_video_config.app_format (or mtl_cvideo_config.app_format for a
 * codec plugin) to a value below and MTL converts, in the caller or a library worker,
 * never on a tasklet; the granted path is reported (info.direct, MTL_TXR_COPIED).
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
  MTL_COLOR_ALPHA = 8,
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

/* Static names of a transport or app format: SDP sampling/depth, FourCC, GStreamer and
   FFmpeg names; NULL where none exists. is_app: 0 transport, 1 app format. AS. */
struct mtl_format_names {
  const char* name;       /* "yuv422_10", "v210" */
  const char* sdp;        /* "YCbCr-4:2:2/10" */
  const char* gst;        /* "v210", "UYVY", ... */
  const char* av_pix_fmt; /* "yuv422p10le", ... */
  uint32_t fourcc;
  uint32_t planes;
};
MTL_API_AS int mtl_format_names(uint32_t format, int is_app, struct mtl_format_names* out,
                                size_t size);
/* The reverse: a name, SDP string or FFmpeg/GStreamer name to a format. AS. */
MTL_API_AS int mtl_format_parse(const char* name, uint32_t* format, int* is_app);
/* Whether MTL converts between them, and where (enum mtl_path in mtl_observe.h). AS. */
MTL_API_AS int mtl_format_convertible(uint32_t transport, uint32_t app);
/* Bytes of one frame in a format, per plane; the wire bandwidth of a video config. AS. */
MTL_API_AS int mtl_format_frame_bytes(uint32_t format, int is_app, uint32_t width,
                                      uint32_t height, uint64_t* plane_bytes,
                                      uint32_t max_planes);
MTL_API_AS int mtl_video_bandwidth(const struct mtl_video_config* v, uint64_t* bits_per_s);
/* Bytes per row and rows of one plane of a format, without a session. AS. */
MTL_API_AS int mtl_format_plane(uint32_t format, int is_app, uint32_t width, uint32_t height,
                                uint32_t plane, uint32_t* row_bytes, uint32_t* rows);
/* RFC 4175 pixel group of a transport format: bytes and pixels per group. AS. */
MTL_API_AS int mtl_format_pgroup(uint32_t format, uint32_t* bytes, uint32_t* pixels);
/* Codec names ("jpegxs", "h264", "h265") both ways. AS. */
MTL_API_AS int mtl_codec_parse(const char* name, uint32_t* codec);
MTL_API_AS const char* mtl_codec_name(uint32_t codec);
/* Bytes of `samples` samples of an audio config (all channels). AS. */
MTL_API_AS int mtl_audio_bytes(const struct mtl_audio_config* a, uint32_t samples,
                               uint64_t* bytes);

MTL_SIZE_CHECK(mtl_format_names, 40);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_FORMAT_H */
