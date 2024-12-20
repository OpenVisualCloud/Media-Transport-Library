/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifndef __GST_MTL_COMMON_H__
#define __GST_MTL_COMMON_H__

#include <arpa/inet.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <mtl/mtl_api.h>
#include <mtl/st_pipeline_api.h>

enum gst_mtl_supported_fps {
  GST_MTL_SUPPORTED_FPS_23_98 = 2398,
  GST_MTL_SUPPORTED_FPS_24 = 24,
  GST_MTL_SUPPORTED_FPS_25 = 25,
  GST_MTL_SUPPORTED_FPS_29_97 = 2997,
  GST_MTL_SUPPORTED_FPS_30 = 30,
  GST_MTL_SUPPORTED_FPS_50 = 50,
  GST_MTL_SUPPORTED_FPS_59_94 = 5994,
  GST_MTL_SUPPORTED_FPS_60 = 60,
  GST_MTL_SUPPORTED_FPS_100 = 100,
  GST_MTL_SUPPORTED_FPS_119_88 = 11988,
  GST_MTL_SUPPORTED_FPS_120 = 120
};

typedef struct StDevArgs {
  gchar port[MTL_PORT_MAX_LEN];
  gchar local_ip_string[MTL_PORT_MAX_LEN];
  gint tx_queues_cnt[MTL_PORT_MAX];
  gint rx_queues_cnt[MTL_PORT_MAX];
  gchar dma_dev[MTL_PORT_MAX_LEN];
} StDevArgs;

typedef struct SessionPortArgs {
  gchar session_ip_string[MTL_PORT_MAX_LEN];
  gchar port[MTL_PORT_MAX_LEN];
  gint udp_port;
  gint payload_type;
} SessionPortArgs;

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo* finfo,
                                          enum st_frame_fmt* fmt);
gboolean gst_mtl_common_parse_fps(GstVideoInfo* info, enum st_fps* fps);
gboolean gst_mtl_common_parse_fps_code(gint fps_code, enum st_fps* fps);
gboolean gst_mtl_common_parse_pixel_format(const char* format, enum st_frame_fmt* fmt);

#endif /* __GST_MTL_COMMON_H__ */