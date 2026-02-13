/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <json-c/json.h>
#include <math.h>
#include <mtl/st30_api.h>
#include <mtl/st40_api.h>
#include <mtl/st41_api.h>
#include <mtl/st_pipeline_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_platform.h"
#include "fmt.h"

#ifndef _ST_APP_PARSE_JSON_HEAD_H_
#define _ST_APP_PARSE_JSON_HEAD_H_

#define ST_APP_URL_MAX_LEN (256)

enum return_type {
  ST_JSON_SUCCESS = 0,
  ST_JSON_PARSE_FAIL,
  ST_JSON_NOT_VALID,
  ST_JSON_NULL,
  ST_JSON_MAX,
};

enum tr_offset {
  TR_OFFSET_DEFAULT,
  TR_OFFSET_NONE,
  TR_OFFSET_MAX,
};

enum video_format {
  VIDEO_FORMAT_480I_59FPS,
  VIDEO_FORMAT_576I_50FPS,
  VIDEO_FORMAT_720P_119FPS,
  VIDEO_FORMAT_720P_59FPS,
  VIDEO_FORMAT_720P_50FPS,
  VIDEO_FORMAT_720P_29FPS,
  VIDEO_FORMAT_720P_25FPS,
  VIDEO_FORMAT_720P_60FPS,
  VIDEO_FORMAT_720P_30FPS,
  VIDEO_FORMAT_720P_24FPS,
  VIDEO_FORMAT_720P_23FPS,
  VIDEO_FORMAT_1080P_120FPS,
  VIDEO_FORMAT_1080P_119FPS,
  VIDEO_FORMAT_1080P_100FPS,
  VIDEO_FORMAT_1080P_59FPS,
  VIDEO_FORMAT_1080P_50FPS,
  VIDEO_FORMAT_1080P_29FPS,
  VIDEO_FORMAT_1080P_25FPS,
  VIDEO_FORMAT_1080I_59FPS,
  VIDEO_FORMAT_1080I_50FPS,
  VIDEO_FORMAT_1080P_60FPS,
  VIDEO_FORMAT_1080P_30FPS,
  VIDEO_FORMAT_1080P_24FPS,
  VIDEO_FORMAT_1080P_23FPS,
  VIDEO_FORMAT_2160P_119FPS,
  VIDEO_FORMAT_2160P_59FPS,
  VIDEO_FORMAT_2160P_50FPS,
  VIDEO_FORMAT_2160P_29FPS,
  VIDEO_FORMAT_2160P_25FPS,
  VIDEO_FORMAT_2160P_60FPS,
  VIDEO_FORMAT_2160P_30FPS,
  VIDEO_FORMAT_2160P_24FPS,
  VIDEO_FORMAT_2160P_23FPS,
  VIDEO_FORMAT_4320P_119FPS,
  VIDEO_FORMAT_4320P_59FPS,
  VIDEO_FORMAT_4320P_50FPS,
  VIDEO_FORMAT_4320P_29FPS,
  VIDEO_FORMAT_4320P_25FPS,
  VIDEO_FORMAT_4320P_60FPS,
  VIDEO_FORMAT_4320P_30FPS,
  VIDEO_FORMAT_4320P_24FPS,
  VIDEO_FORMAT_4320P_23FPS,
  VIDEO_FORMAT_DCI1080P_119FPS,
  VIDEO_FORMAT_DCI1080P_60FPS,
  VIDEO_FORMAT_DCI1080P_59FPS,
  VIDEO_FORMAT_DCI1080P_50FPS,
  VIDEO_FORMAT_DCI1080P_30FPS,
  VIDEO_FORMAT_DCI1080P_29FPS,
  VIDEO_FORMAT_DCI1080P_25FPS,
  VIDEO_FORMAT_DCI1080P_24FPS,
  VIDEO_FORMAT_DCI1080P_23FPS,
  VIDEO_FORMAT_DCI2160P_119FPS,
  VIDEO_FORMAT_DCI2160P_60FPS,
  VIDEO_FORMAT_DCI2160P_59FPS,
  VIDEO_FORMAT_DCI2160P_50FPS,
  VIDEO_FORMAT_DCI2160P_30FPS,
  VIDEO_FORMAT_DCI2160P_29FPS,
  VIDEO_FORMAT_DCI2160P_25FPS,
  VIDEO_FORMAT_DCI2160P_24FPS,
  VIDEO_FORMAT_DCI2160P_23FPS,
  VIDEO_FORMAT_AUTO,
  VIDEO_FORMAT_MAX,
};

enum anc_format {
  ANC_FORMAT_CLOSED_CAPTION,
  ANC_FORMAT_MAX,
};

struct st_video_fmt_desc {
  enum video_format fmt;
  char* name;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
};

typedef struct st_json_interface {
  char name[MTL_PORT_MAX_LEN];
  enum mtl_net_proto net_proto;
  uint8_t ip_addr[MTL_IP_ADDR_LEN];
  uint8_t netmask[MTL_IP_ADDR_LEN];
  uint8_t gateway[MTL_IP_ADDR_LEN];
  uint16_t tx_queues_cnt;
  uint16_t rx_queues_cnt;
  int tx_video_sessions_cnt; /* st20/st22/st20p/st22p on interface level */
  int rx_video_sessions_cnt; /* st20/st22/st20p/st22p on interface level */
  int tx_audio_sessions_cnt; /* st30 on interface level */
  int rx_audio_sessions_cnt; /* st30 on interface level */
  int tx_anc_sessions_cnt;   /* st40 on interface level */
  int rx_anc_sessions_cnt;   /* st40 on interface level */
  int tx_fmd_sessions_cnt;   /* st41 on interface level */
  int rx_fmd_sessions_cnt;   /* st41 on interface level */
} st_json_interface_t;

enum st_json_ip_type {
  ST_JSON_IP_ADDR = 0,
  ST_JSON_IP_LOCAL_IF,
  ST_JSON_IP_MAX,
};

typedef struct st_json_session_base {
  uint8_t ip[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  uint8_t mcast_src_ip[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  st_json_interface_t* inf[MTL_SESSION_PORT_MAX];
  int num_inf;
  uint16_t udp_port;
  uint8_t payload_type;
  enum st_json_ip_type type[MTL_SESSION_PORT_MAX];
  enum mtl_port local[MTL_SESSION_PORT_MAX]; /* if use ST_JSON_IP_LOCAL_IF */
  uint8_t local_ip[MTL_SESSION_PORT_MAX]
                  [MTL_IP_ADDR_LEN]; /* if use ST_JSON_IP_LOCAL_IF */
} st_json_session_base_t;

typedef struct st_json_video_info {
  enum video_format video_format;
  enum st21_pacing pacing;
  enum st20_type type;
  enum st20_packing packing;
  enum tr_offset tr_offset;
  enum st20_fmt pg_format;

  char video_url[ST_APP_URL_MAX_LEN];
} st_json_video_info_t;

typedef struct st_json_audio_info {
  enum st30_type type;
  enum st30_fmt audio_format;
  int audio_channel;
  enum st30_sampling audio_sampling;
  enum st30_ptime audio_ptime;

  char audio_url[ST_APP_URL_MAX_LEN];
} st_json_audio_info_t;

typedef struct st_json_ancillary_info {
  enum st40_type type;
  enum anc_format anc_format;
  enum st_fps anc_fps;
  bool interlaced;

  char anc_url[ST_APP_URL_MAX_LEN];
} st_json_ancillary_info_t;

typedef struct st_json_fastmetadata_info {
  enum st41_type type;
  uint32_t fmd_dit;
  uint8_t fmd_k_bit;
  enum st_fps fmd_fps;
  bool interlaced;

  char fmd_url[ST_APP_URL_MAX_LEN];
} st_json_fastmetadata_info_t;

typedef struct st_json_st22p_info {
  enum st_frame_fmt format;
  enum st21_pacing pacing;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  bool interlaced;
  enum st_plugin_device device;
  enum st22_codec codec;
  enum st22_pack_type pack_type;
  enum st22_quality_mode quality;
  uint32_t codec_thread_count;

  char st22p_url[ST_APP_URL_MAX_LEN];
} st_json_st22p_info_t;

typedef struct st_json_st20p_info {
  enum st_frame_fmt format;
  enum st20_fmt transport_format;
  enum st21_pacing transport_pacing;
  enum st20_packing transport_packing;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st_plugin_device device;
  bool interlaced;
  char st20p_url[ST_APP_URL_MAX_LEN];
} st_json_st20p_info_t;

typedef struct st_json_video_session {
  st_json_session_base_t base;
  st_json_video_info_t info;

  bool display;
  bool enable_rtcp;

  /* rx only items */
  enum user_pg_fmt user_pg_format;
  bool measure_latency;
} st_json_video_session_t;

typedef struct st_json_audio_session {
  st_json_session_base_t base;
  st_json_audio_info_t info;

  bool enable_rtcp;
} st_json_audio_session_t;

typedef struct st_json_st30p_session {
  st_json_session_base_t base;
  st_json_audio_info_t info;
  bool enable_rtcp;
  bool user_pacing;
} st_json_st30p_session_t;

typedef struct st_json_ancillary_session {
  st_json_session_base_t base;
  st_json_ancillary_info_t info;

  bool user_pacing;
  bool exact_user_pacing;
  bool user_timestamp;
  bool enable_rtcp;
} st_json_ancillary_session_t;

typedef struct st_json_st40p_session {
  st_json_session_base_t base;
  st_json_ancillary_info_t info;
  bool user_pacing;
  bool exact_user_pacing;
  bool user_timestamp;
  bool enable_rtcp;
  int test_mode; /* maps to enum st40_tx_test_pattern (0=none,1=no-marker,2=seq-gap,...)
                  */
  int test_pkt_count;   /* ANC packets per test frame (0 = mode default) */
  int test_frame_count; /* number of frames to mutate (0 = library default, e.g. 8 for
                           redundant) */
  uint32_t redundant_delay_ns; /* extra delay (ns) before sending on port R */
  uint64_t reorder_window_ns;  /* RX reorder window (ns), 0 = lib default 10 ms */
} st_json_st40p_session_t;

typedef struct st_json_fastmetadata_session {
  st_json_session_base_t base;
  st_json_fastmetadata_info_t info;

  bool enable_rtcp;
} st_json_fastmetadata_session_t;

typedef struct st_json_st22p_session {
  st_json_session_base_t base;
  st_json_st22p_info_t info;

  bool display;
  bool measure_latency;
  bool enable_rtcp;
} st_json_st22p_session_t;

typedef struct st_json_st20p_session {
  st_json_session_base_t base;
  st_json_st20p_info_t info;

  bool display;
  bool measure_latency;
  bool enable_rtcp;
  bool user_pacing;
  bool exact_user_pacing;
  bool user_timestamp;
  uint64_t user_time_offset;
} st_json_st20p_session_t;

typedef struct st_json_context {
  st_json_interface_t* interfaces;
  int num_interfaces;
  enum mtl_rss_mode rss_mode;
  int sch_quota;
  int tx_audio_sessions_max_per_sch;
  int rx_audio_sessions_max_per_sch;
  bool has_display;
  bool shared_tx_queues;
  bool shared_rx_queues;
  bool tx_no_chain;
  char* log_file;
  uint64_t user_time_offset;

  st_json_video_session_t* tx_video_sessions;
  int tx_video_session_cnt;
  st_json_audio_session_t* tx_audio_sessions;
  int tx_audio_session_cnt;
  st_json_ancillary_session_t* tx_anc_sessions;
  int tx_anc_session_cnt;
  st_json_fastmetadata_session_t* tx_fmd_sessions;
  int tx_fmd_session_cnt;
  st_json_st22p_session_t* tx_st22p_sessions;
  int tx_st22p_session_cnt;
  st_json_st20p_session_t* tx_st20p_sessions;
  int tx_st20p_session_cnt;
  st_json_st30p_session_t* tx_st30p_sessions;
  int tx_st30p_session_cnt;
  st_json_st40p_session_t* tx_st40p_sessions;
  int tx_st40p_session_cnt;

  st_json_video_session_t* rx_video_sessions;
  int rx_video_session_cnt;
  st_json_audio_session_t* rx_audio_sessions;
  int rx_audio_session_cnt;
  st_json_ancillary_session_t* rx_anc_sessions;
  int rx_anc_session_cnt;
  st_json_fastmetadata_session_t* rx_fmd_sessions;
  int rx_fmd_session_cnt;
  st_json_st22p_session_t* rx_st22p_sessions;
  int rx_st22p_session_cnt;
  st_json_st20p_session_t* rx_st20p_sessions;
  int rx_st20p_session_cnt;
  st_json_video_session_t* rx_st20r_sessions;
  int rx_st20r_session_cnt;
  st_json_st30p_session_t* rx_st30p_sessions;
  int rx_st30p_session_cnt;
  st_json_st40p_session_t* rx_st40p_sessions;
  int rx_st40p_session_cnt;
} st_json_context_t;

int st_app_parse_json(st_json_context_t* ctx, const char* filename);
void st_app_free_json(st_json_context_t* ctx);

enum st_fps st_app_get_fps(enum video_format fmt);
uint32_t st_app_get_width(enum video_format fmt);
uint32_t st_app_get_height(enum video_format fmt);
bool st_app_get_interlaced(enum video_format fmt);

#endif
