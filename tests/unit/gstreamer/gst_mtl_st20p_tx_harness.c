/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Drives gst_mtl_st20p_tx_session_create() with the production source compiled in
 * place, the way ffmpeg/mtl_common_harness.c drives the FFmpeg plugin.
 *
 * Four seams, all by #define rename so they stay inside this translation unit and
 * cannot interpose on the rest of UnitTest:
 *   - gst_video_info_new_from_caps() returns an info this file built, so a case is a
 *     set of video parameters and no caps string, GstCaps, or gst_init() is needed;
 *     it can also return NULL, which is what the plugin never used to check.
 *   - gst_video_info_free() is counted, so every exit can assert the info came back.
 *   - st20p_tx_create() is stubbed: the session create path is under test here, not
 *     the library, and a real create would need a NIC.
 *   - st20p_tx_frame_size() is stubbed because the success path calls it on the
 *     handle the stub above invented.
 *
 * No gst_init(): _gst_debug_min stays at its GST_LEVEL_DEFAULT of GST_LEVEL_NONE, so
 * every GST_ERROR() in the compiled-in source is a threshold miss and the
 * uninitialised debug category is never dereferenced. The static assert below is what
 * fails the build if a GStreamer release ever changes that default.
 */

#include "gstreamer/gst_mtl_st20p_tx_harness.h"

#include <gst/video/video.h>
#include <mtl/st_pipeline_api.h>
#include <stdint.h>
#include <string.h>

G_STATIC_ASSERT(GST_LEVEL_DEFAULT == GST_LEVEL_NONE);

static enum ut_gst_st20p_tx_case ut_st20p_case;
static int ut_st20p_info_allocs;
static int ut_st20p_info_frees;

/* Declared before the renames below so the plugin's calls land here, and defined
 * after them so this file still reaches the real functions. */
static GstVideoInfo* ut_gst_video_info_new_from_caps(const GstCaps* caps);
static void ut_gst_video_info_free(GstVideoInfo* info);
static st20p_tx_handle ut_st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops);
static size_t ut_st20p_tx_frame_size(st20p_tx_handle handle);

#define gst_video_info_new_from_caps ut_gst_video_info_new_from_caps
#define gst_video_info_free ut_gst_video_info_free
#define st20p_tx_create ut_st20p_tx_create
#define st20p_tx_frame_size ut_st20p_tx_frame_size
#include "../../../ecosystem/gstreamer_plugin/gst_mtl_st20p_tx.c"
#undef st20p_tx_frame_size
#undef st20p_tx_create
#undef gst_video_info_free
#undef gst_video_info_new_from_caps

static GstVideoInfo* ut_gst_video_info_new_from_caps(const GstCaps* caps) {
  GstVideoFormat format = GST_VIDEO_FORMAT_v210;
  GstVideoInfo* info;

  (void)caps;
  if (ut_st20p_case == UT_GST_ST20P_TX_INFO_NULL) return NULL;

  if (ut_st20p_case == UT_GST_ST20P_TX_FORMAT_UNSUPPORTED)
    format = GST_VIDEO_FORMAT_RGB; /* neither v210 nor I422_10LE */

  info = gst_video_info_new();
  if (!info) return NULL;
  gst_video_info_set_format(info, format, 1920, 1080);

  if (ut_st20p_case == UT_GST_ST20P_TX_INTERLACE_UNSUPPORTED)
    info->interlace_mode = GST_VIDEO_INTERLACE_MODE_FIELDS;
  else
    info->interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  if (ut_st20p_case == UT_GST_ST20P_TX_FPS_OFF_TABLE) {
    info->fps_n = 48; /* outside every st_fps_timings window */
    info->fps_d = 1;
  } else if (ut_st20p_case == UT_GST_ST20P_TX_FPS_DEN_ZERO) {
    info->fps_n = 60;
    info->fps_d = 0;
  } else {
    info->fps_n = 60;
    info->fps_d = 1;
  }

  ut_st20p_info_allocs++;
  return info;
}

static void ut_gst_video_info_free(GstVideoInfo* info) {
  ut_st20p_info_frees++;
  gst_video_info_free(info);
}

static st20p_tx_handle ut_st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops) {
  (void)mt;
  (void)ops;
  if (ut_st20p_case == UT_GST_ST20P_TX_CREATE_FAILS) return NULL;
  return (st20p_tx_handle)(uintptr_t)0x5720; /* never dereferenced */
}

static size_t ut_st20p_tx_frame_size(st20p_tx_handle handle) {
  (void)handle;
  return 1920 * 1080 * 8 / 3;
}

void ut_gst_st20p_tx_session_create(enum ut_gst_st20p_tx_case c,
                                    struct ut_gst_st20p_tx_result* out) {
  Gst_Mtl_St20p_Tx sink;

  ut_st20p_case = c;
  ut_st20p_info_allocs = 0;
  ut_st20p_info_frees = 0;

  memset(&sink, 0, sizeof(sink));
  if (c != UT_GST_ST20P_TX_NO_MTL_HANDLE)
    sink.mtl_lib_handle = (mtl_handle)(uintptr_t)0x2110; /* never dereferenced */

  if (c != UT_GST_ST20P_TX_PORT_UNSET) {
    strncpy(sink.generalArgs.port[MTL_PORT_P], "0000:af:01.0", MTL_PORT_MAX_LEN - 1);
    strncpy(sink.portArgs.session_ip_string[MTL_PORT_P], "239.168.85.20",
            MTL_PORT_MAX_LEN - 1);
    sink.portArgs.udp_port[MTL_PORT_P] = 20000;
    sink.portArgs.payload_type = PAYLOAD_TYPE_VIDEO;
  }

  out->created = gst_mtl_st20p_tx_session_create(&sink, NULL) ? true : false;
  out->info_allocs = ut_st20p_info_allocs;
  out->info_frees = ut_st20p_info_frees;
  out->handle_set = sink.tx_handle != NULL;
}
