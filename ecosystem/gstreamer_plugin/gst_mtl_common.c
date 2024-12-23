/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gst_mtl_common.h"

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo* finfo,
                                          enum st_frame_fmt* fmt) {
  if (finfo->format == GST_VIDEO_FORMAT_v210) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (finfo->format == GST_VIDEO_FORMAT_I422_10LE) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else {
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_parse_fps_code(gint fps_code, enum st_fps* fps) {
  if (!fps) {
    GST_ERROR("Invalid fps pointer");
    return FALSE;
  }

  switch (fps_code) {
    case GST_MTL_SUPPORTED_FPS_120:
      *fps = ST_FPS_P120;
      break;
    case GST_MTL_SUPPORTED_FPS_119_88:
      *fps = ST_FPS_P119_88;
      break;
    case GST_MTL_SUPPORTED_FPS_100:
      *fps = ST_FPS_P100;
      break;
    case GST_MTL_SUPPORTED_FPS_60:
      *fps = ST_FPS_P60;
      break;
    case GST_MTL_SUPPORTED_FPS_59_94:
      *fps = ST_FPS_P59_94;
      break;
    case GST_MTL_SUPPORTED_FPS_50:
      *fps = ST_FPS_P50;
      break;
    case GST_MTL_SUPPORTED_FPS_30:
      *fps = ST_FPS_P30;
      break;
    case GST_MTL_SUPPORTED_FPS_29_97:
      *fps = ST_FPS_P29_97;
      break;
    case GST_MTL_SUPPORTED_FPS_25:
      *fps = ST_FPS_P25;
      break;
    case GST_MTL_SUPPORTED_FPS_24:
      *fps = ST_FPS_P24;
      break;
    case GST_MTL_SUPPORTED_FPS_23_98:
      *fps = ST_FPS_P23_98;
      break;
    default:
      return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_parse_fps(GstVideoInfo* info, enum st_fps* fps) {
  gint fps_div;
  if (info->fps_n <= 0 || info->fps_d <= 0) {
    return FALSE;
  }

  fps_div = info->fps_n / info->fps_d;

  switch (fps_div) {
    case 24:
      *fps = ST_FPS_P24;
      break;
    case 25:
      *fps = ST_FPS_P25;
      break;
    case 30:
      *fps = ST_FPS_P30;
      break;
    case 50:
      *fps = ST_FPS_P50;
      break;
    case 60:
      *fps = ST_FPS_P60;
      break;
    case 120:
      *fps = ST_FPS_P120;
      break;
    default:
      return FALSE;
  }

  return TRUE;
}

/* includes all formats supported by the library for future support */
gboolean gst_mtl_common_parse_pixel_format(const char* format, enum st_frame_fmt* fmt) {
  if (!fmt || !format) {
    GST_ERROR("%s, invalid input\n", __func__);
    return FALSE;
  }

  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "v210") == 0) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "Y210") == 0) {
    *fmt = ST_FRAME_FMT_Y210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "UYVY") == 0) {
    *fmt = ST_FRAME_FMT_UYVY;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    *fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "YUV422PLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR12LE;
  } else if (strcmp(format, "YUV422RFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE12;
  } else if (strcmp(format, "YUV444PLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV444PLANAR10LE;
  } else if (strcmp(format, "YUV444RFC4175PG4BE10") == 0) {
    *fmt = ST_FRAME_FMT_YUV444RFC4175PG4BE10;
  } else if (strcmp(format, "YUV444PLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_YUV444PLANAR12LE;
  } else if (strcmp(format, "YUV444RFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_YUV444RFC4175PG2BE12;
  } else if (strcmp(format, "YUV420CUSTOM8") == 0) {
    *fmt = ST_FRAME_FMT_YUV420CUSTOM8;
  } else if (strcmp(format, "YUV422CUSTOM8") == 0) {
    *fmt = ST_FRAME_FMT_YUV422CUSTOM8;
  } else if (strcmp(format, "YUV420PLANAR8") == 0) {
    *fmt = ST_FRAME_FMT_YUV420PLANAR8;
  } else if (strcmp(format, "ARGB") == 0) {
    *fmt = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    *fmt = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "RGB8") == 0) {
    *fmt = ST_FRAME_FMT_RGB8;
  } else if (strcmp(format, "GBRPLANAR10LE") == 0) {
    *fmt = ST_FRAME_FMT_GBRPLANAR10LE;
  } else if (strcmp(format, "RGBRFC4175PG4BE10") == 0) {
    *fmt = ST_FRAME_FMT_RGBRFC4175PG4BE10;
  } else if (strcmp(format, "GBRPLANAR12LE") == 0) {
    *fmt = ST_FRAME_FMT_GBRPLANAR12LE;
  } else if (strcmp(format, "RGBRFC4175PG2BE12") == 0) {
    *fmt = ST_FRAME_FMT_RGBRFC4175PG2BE12;
  } else {
    GST_ERROR("invalid output format %s\n", format);
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtl_common_parse_audio_format(const char* format, enum st30_fmt* audio) {
  if (!audio || !format) {
    GST_ERROR("%s, invalid input\n", __func__);
    return FALSE;
  }

  if (strcmp(format, "PCM8") == 0) {
    *audio = ST30_FMT_PCM8;
  } else if (strcmp(format, "PCM16") == 0) {
    *audio = ST30_FMT_PCM16;
  } else if (strcmp(format, "PCM24") == 0) {
    *audio = ST30_FMT_PCM24;
  } else if (strcmp(format, "AM824") == 0) {
    *audio = ST31_FMT_AM824;
  } else {
    GST_ERROR("%s, invalid audio format %s\n", __func__, format);
    return FALSE;
  }

  return TRUE;
}

gboolean gst_mtlst30tx_parse_sampling(gint sampling,
                                             enum st30_sampling* st_sampling) {
  if (!st_sampling) {
    GST_ERROR("Invalid st_sampling pointer");
    return FALSE;
  }

  switch (sampling) {
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_44_1K:
      *st_sampling = ST31_SAMPLING_44K;
      return TRUE;
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_48K:
      *st_sampling = ST30_SAMPLING_48K;
      return TRUE;
    case GST_MTL_SUPPORTED_AUDIO_SAMPLING_96K:
      *st_sampling = ST30_SAMPLING_96K;
      return TRUE;
    default:
      return FALSE;
  }
}
