/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <json-c/json.h>
#include <math.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_platform.h"
#include "fmt.h"

#ifndef _ST_APP_PARSE_JSON_HEAD_H_
#define _ST_APP_PARSE_JSON_HEAD_H_

#define MAX_INTERFACES 2

#define MAX_VIDEO 180
#define MAX_AUDIO 180
#define MAX_ANC 180

#define ST_APP_URL_MAX_LEN (256)

enum return_type {
  ST_JSON_SUCCESS,
  ST_JSON_PARSE_FAIL,
  ST_JSON_NOT_VALID,
  ST_JSON_NULL,
  ST_JSON_MAX,
};

enum pacing {
  PACING_GAP,
  PACING_LINEAR,
  PACING_MAX,
};

enum tr_offset {
  TR_OFFSET_DEFAULT,
  TR_OFFSET_NONE,
  TR_OFFSET_MAX,
};

enum video_format {
  VIDEO_FORMAT_480I_59FPS,
  VIDEO_FORMAT_576I_50FPS,
  VIDEO_FORMAT_720P_59FPS,
  VIDEO_FORMAT_720P_50FPS,
  VIDEO_FORMAT_720P_29FPS,
  VIDEO_FORMAT_720P_25FPS,
  VIDEO_FORMAT_1080P_59FPS,
  VIDEO_FORMAT_1080P_50FPS,
  VIDEO_FORMAT_1080P_29FPS,
  VIDEO_FORMAT_1080P_25FPS,
  VIDEO_FORMAT_1080I_59FPS,
  VIDEO_FORMAT_1080I_50FPS,
  VIDEO_FORMAT_2160P_59FPS,
  VIDEO_FORMAT_2160P_50FPS,
  VIDEO_FORMAT_2160P_29FPS,
  VIDEO_FORMAT_2160P_25FPS,
  VIDEO_FORMAT_4320P_59FPS,
  VIDEO_FORMAT_4320P_50FPS,
  VIDEO_FORMAT_4320P_29FPS,
  VIDEO_FORMAT_4320P_25FPS,
  VIDEO_FORMAT_MAX,
};

enum anc_format {
  ANC_FORMAT_CLOSED_CAPTION,
  ANC_FORMAT_MAX,
};

typedef struct st_json_interface {
  char name[ST_PORT_MAX_LEN];
  uint8_t ip_addr[ST_IP_ADDR_LEN];
} st_json_interface_t;

typedef struct st_json_tx_video_session {
  uint8_t dip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  char video_url[ST_APP_URL_MAX_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
  enum video_format video_format;
  enum pacing pacing;
  enum st20_type type;
  enum st20_packing packing;
  enum tr_offset tr_offset;
  enum st20_fmt pg_format;
} st_json_tx_video_session_t;

typedef struct st_json_tx_audio_session {
  uint8_t dip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  char audio_url[ST_APP_URL_MAX_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
  enum st30_type type;
  enum st30_fmt audio_format;
  int audio_channel;
  enum st30_sampling audio_sampling;
  int audio_frametime_ms;
} st_json_tx_audio_session_t;

typedef struct st_json_tx_ancillary_session {
  uint8_t dip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  char anc_url[ST_APP_URL_MAX_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
  enum st40_type type;
  enum anc_format anc_format;
  enum st_fps anc_fps;
} st_json_tx_ancillary_session_t;

typedef struct st_json_rx_video_session {
  uint8_t ip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
  enum video_format video_format;
  enum pacing pacing;
  enum st20_type type;
  enum tr_offset tr_offset;
  enum st20_fmt pg_format;
  enum user_pg_fmt user_pg_format;
  bool display;
} st_json_rx_video_session_t;

typedef struct st_json_rx_audio_session {
  uint8_t ip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  char audio_url[ST_APP_URL_MAX_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
  enum st30_type type;
  enum st30_fmt audio_format;
  int audio_channel;
  enum st30_sampling audio_sampling;
  int audio_frametime_ms;
} st_json_rx_audio_session_t;

typedef struct st_json_rx_ancillary_session {
  uint8_t ip[ST_PORT_MAX][ST_IP_ADDR_LEN];
  char anc_url[ST_APP_URL_MAX_LEN];
  st_json_interface_t* inf[ST_PORT_MAX];
  int num_inf;

  uint16_t udp_port;
} st_json_rx_ancillary_session_t;

typedef struct st_json_context {
  st_json_interface_t interfaces[MAX_INTERFACES];
  int num_interfaces;
  int sch_quota;

  st_json_tx_video_session_t tx_video[MAX_VIDEO];
  int tx_video_session_cnt;
  st_json_tx_audio_session_t tx_audio[MAX_AUDIO];
  int tx_audio_session_cnt;
  st_json_tx_ancillary_session_t tx_anc[MAX_ANC];
  int tx_anc_session_cnt;

  st_json_rx_video_session_t rx_video[MAX_VIDEO];
  int rx_video_session_cnt;
  st_json_rx_audio_session_t rx_audio[MAX_AUDIO];
  int rx_audio_session_cnt;
  st_json_rx_ancillary_session_t rx_anc[MAX_ANC];
  int rx_anc_session_cnt;
} st_json_context_t;

int st_app_parse_json(st_json_context_t* ctx, const char* filename);

enum st_fps st_app_get_fps(enum video_format fmt);
int st_app_get_width(enum video_format fmt);
int st_app_get_height(enum video_format fmt);
bool st_app_get_interlaced(enum video_format fmt);
#endif
