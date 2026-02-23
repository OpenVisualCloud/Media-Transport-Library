/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifndef __GST_MTL_COMMON_H__
#define __GST_MTL_COMMON_H__

#include <arpa/inet.h>
#include <gst/audio/audio-info.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <mtl/mtl_api.h>
#include <mtl/st30_pipeline_api.h>
#include <mtl/st40_api.h>
#include <mtl/st40_pipeline_api.h>
#include <mtl/st_pipeline_api.h>

#define PAYLOAD_TYPE_AUDIO (111)
#define PAYLOAD_TYPE_VIDEO (112)
#define PAYLOAD_TYPE_ANCILLARY (113)

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

#ifndef NS_PER_S
#define NS_PER_S (1000 * NS_PER_MS)
#endif

#define DEFAULT_FRAMERATE 25
#define GST_MTL_DEFAULT_FRAMEBUFF_CNT 3

enum {
  PROP_GENERAL_0,
  PROP_GENERAL_LOG_LEVEL,
  PROP_GENERAL_DEV_ARGS_PORT,
  PROP_GENERAL_DEV_ARGS_PORT_R,
  PROP_GENERAL_DEV_ARGS_SIP,
  PROP_GENERAL_DEV_ARGS_SIP_R,
  PROP_GENERAL_DEV_ARGS_DMA_DEV,
  PROP_GENERAL_SESSION_PORT,
  PROP_GENERAL_SESSION_PORT_R,
  PROP_GENERAL_PORT_IP,
  PROP_GENERAL_PORT_IP_R,
  PROP_GENERAL_PORT_UDP_PORT,
  PROP_GENERAL_PORT_UDP_PORT_R,
  PROP_GENERAL_PORT_PAYLOAD_TYPE,
  PROP_GENERAL_PORT_RX_QUEUES,
  PROP_GENERAL_PORT_TX_QUEUES,
  PROP_GENERAL_ENABLE_ONBOARD_PTP,
  PROP_GENERAL_ENABLE_DMA_OFFLOAD,
  PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_P,
  PROP_GENERAL_DEV_ARGS_ALLOW_DOWN_R,
  PROP_GENERAL_MAX
};

enum gst_mtl_supported_audio_sampling {
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_44_1K = 44100,
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_48K = 48000,
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_96K = 96000
};

typedef struct GeneralArgs {
  gchar port[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
  gchar local_ip_string[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
  gint tx_queues_cnt[MTL_PORT_MAX];
  gint rx_queues_cnt[MTL_PORT_MAX];
  gchar dma_dev[MTL_PORT_MAX_LEN];
  gint log_level;
  gboolean enable_onboard_ptp;
  gboolean enable_dma_offload;
  gboolean allow_port_down[MTL_PORT_MAX];
} GeneralArgs;

typedef struct SessionPortArgs {
  gchar session_ip_string[2][MTL_PORT_MAX_LEN];
  gchar port[2][MTL_PORT_MAX_LEN];
  gint udp_port[2];
  gint payload_type;
} SessionPortArgs;

gboolean gst_mtl_common_parse_input_finfo(const GstVideoFormatInfo* finfo,
                                          enum st_frame_fmt* fmt);
gboolean gst_mtl_common_parse_pixel_format(const char* format, enum st_frame_fmt* fmt);

gboolean gst_mtl_common_parse_audio_format(const char* format, enum st30_fmt* audio);
gboolean gst_mtl_common_parse_ptime(const char* ptime_str, enum st30_ptime* ptime);
gboolean gst_mtl_common_gst_to_st_sampling(gint sampling,
                                           enum st30_sampling* st_sampling);
gboolean gst_mtl_common_st_to_gst_sampling(enum st30_sampling st_sampling,
                                           gint* gst_sampling);

guint gst_mtl_common_parse_tx_port_arguments(struct st_tx_port* port,
                                             SessionPortArgs* port_args);
guint gst_mtl_common_parse_rx_port_arguments(struct st_rx_port* port,
                                             SessionPortArgs* port_args);
gboolean gst_mtl_common_parse_general_arguments(struct mtl_init_params* mtl_init_params,
                                                GeneralArgs* generalArgs);
void gst_mtl_common_copy_general_to_session_args(GeneralArgs* general_args,
                                                 SessionPortArgs* port_args);

void gst_mtl_common_init_general_arguments(GObjectClass* gobject_class);

void gst_mtl_common_set_general_arguments(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec,
                                          GeneralArgs* generalArgs,
                                          SessionPortArgs* portArgs);

void gst_mtl_common_get_general_arguments(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec,
                                          GeneralArgs* generalArgs,
                                          SessionPortArgs* portArgs);

mtl_handle gst_mtl_common_init_handle(GeneralArgs* generalArgs,
                                      gboolean force_to_initialize_new_instance);

gint gst_mtl_common_deinit_handle(mtl_handle* handle);
#endif /* __GST_MTL_COMMON_H__ */