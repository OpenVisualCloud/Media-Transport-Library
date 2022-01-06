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

#include <SDL2/SDL.h>
#include <errno.h>
#include <pcap.h>
#include <pthread.h>
#include <signal.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_platform.h"
#include "parse_json.h"

#ifndef _ST_APP_BASE_HEAD_H_
#define _ST_APP_BASE_HEAD_H_

#define ST_APP_MAX_TX_VIDEO_SESSIONS (60)
#define ST_APP_MAX_TX_AUDIO_SESSIONS (60)
#define ST_APP_MAX_TX_ANC_SESSIONS (60)

#define ST_APP_MAX_RX_VIDEO_SESSIONS (60)
#define ST_APP_MAX_RX_AUDIO_SESSIONS (60)
#define ST_APP_MAX_RX_ANC_SESSIONS (60)

#define ST_APP_MAX_LCORES (32)

#define ST_APP_EXPECT_NEAR(val, expect, delta) \
  ((val > (expect - delta)) && (val < (expect + delta)))

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

struct st_display {
  int idx;
  SDL_Window* window;
  SDL_Renderer* renderer;
  SDL_Texture* texture;
  int window_w;
  int window_h;
  int pixel_w;
  int pixel_h;
  uint8_t* display_frame;
  uint8_t* source_frame;

  pthread_t st_dispaly_thread;
  bool st_dispaly_thread_stop;
  pthread_cond_t st_dispaly_wake_cond;
  pthread_mutex_t st_dispaly_wake_mutex;
};

struct st_app_tx_video_session {
  int idx;
  st_handle st;
  st20_tx_handle handle;
  int handle_sch_idx;
  uint16_t framebuff_cnt;
  uint16_t st21_framebuff_idx; /* index for current fb */
  int* st21_free_framebuff;
  int* st21_ready_framebuff;
  char st21_source_url[ST_APP_URL_MAX_LEN];

  pcap_t* st21_pcap;
  bool st21_pcap_input;
  int st21_source_fd;
  uint8_t* st21_source_begin;
  uint8_t* st21_source_end;
  uint8_t* st21_frame_cursor;
  int st21_frame_size;
  struct st20_pgroup st21_pg;
  int width;
  int height;
  bool single_line;

  /* rtp info */
  bool st21_rtp_input;
  int st21_pkts_in_line;  /* GPM only, number of packets per each line, 4 for 1080p */
  int st21_bytes_in_line; /* bytes per line, 4800 for 1080p yuv422 10bit */
  uint32_t
      st21_pkt_data_len; /* data len(byte) for each pkt, 1200 for 1080p yuv422 10bit */
  struct st20_rfc4175_rtp_hdr st20_rtp_base;
  int st21_total_pkts;  /* total pkts in one frame, ex: 4320 for 1080p */
  int st21_pkt_idx;     /* pkt index in current frame */
  uint32_t st21_seq_id; /* seq id in current frame */
  uint32_t st21_rtp_tmstamp;

  double expect_fps;
  uint64_t stat_frame_frist_tx_time;
  uint32_t st21_frame_done_cnt;
  uint32_t st21_packet_done_cnt;

  pthread_t st21_app_thread;
  bool st21_app_thread_stop;
  pthread_cond_t st21_wake_cond;
  pthread_mutex_t st21_wake_mutex;

  struct st_display* display;
  int lcore;
};

struct st_app_tx_audio_session {
  int idx;
  st30_tx_handle handle;

  uint16_t framebuff_cnt;
  uint16_t st30_framebuff_idx; /* index for current fb */
  int* st30_free_framebuff;
  int* st30_ready_framebuff;
  int st30_frame_done_cnt;
  int st30_packet_done_cnt;

  char st30_source_url[ST_APP_URL_MAX_LEN];
  int st30_source_fd;
  pcap_t* st30_pcap;
  bool st30_pcap_input;
  bool st30_rtp_input;
  uint8_t* st30_source_begin;
  uint8_t* st30_source_end;
  uint8_t* st30_frame_cursor; /* cursor to current frame */
  int st30_frame_size;
  int pkt_len;
  pthread_t st30_app_thread;
  bool st30_app_thread_stop;
  pthread_cond_t st30_wake_cond;
  pthread_mutex_t st30_wake_mutex;
  uint32_t st30_rtp_tmstamp;
  uint16_t st30_seq_id;
};

struct st_app_tx_anc_session {
  int idx;
  st40_tx_handle handle;

  uint16_t framebuff_cnt;
  uint16_t st40_framebuff_idx; /* index for current fb */
  int* st40_free_framebuff;
  int* st40_ready_framebuff;
  uint32_t st40_frame_done_cnt;
  uint32_t st40_packet_done_cnt;

  char st40_source_url[ST_APP_URL_MAX_LEN];
  int st40_source_fd;
  pcap_t* st40_pcap;
  bool st40_pcap_input;
  bool st40_rtp_input;
  uint8_t* st40_source_begin;
  uint8_t* st40_source_end;
  uint8_t* st40_frame_cursor; /* cursor to current frame */
  pthread_t st40_app_thread;
  bool st40_app_thread_stop;
  pthread_cond_t st40_wake_cond;
  pthread_mutex_t st40_wake_mutex;
  uint32_t st40_rtp_tmstamp;
  uint32_t st40_seq_id;
};

struct st_app_rx_video_session {
  int idx;
  st20_rx_handle handle;
  int framebuff_cnt;
  int st21_frame_size;

  char st21_dst_url[ST_APP_URL_MAX_LEN];
  int st21_dst_fb_cnt; /* the count of recevied fbs will be saved to file */
  int st21_dst_fd;
  uint8_t* st21_dst_begin;
  uint8_t* st21_dst_end;
  uint8_t* st21_dst_cursor;

  /* frame info */
  void** st21_frames_dst_queue; /* list to received frame buffers */
  int st21_dst_q_size;
  int st21_dst_qp_idx; /* producer index */
  int st21_dst_qc_idx; /* consumer index */

  /* rtp info */
  uint32_t st21_last_tmstamp;
  struct st20_pgroup st21_pg;
  int width;
  int height;

  /* stat */
  int stat_frame_received;
  uint64_t stat_last_time;
  int stat_frame_total_received;
  uint64_t stat_frame_frist_rx_time;
  double expect_fps;

  pthread_t st21_app_thread;
  pthread_cond_t st21_wake_cond;
  pthread_mutex_t st21_wake_mutex;
  bool st21_app_thread_stop;

  struct st_display* display;
};

struct st_app_rx_audio_session {
  int idx;
  st30_rx_handle handle;
  int framebuff_cnt;
  int st30_frame_size;
  int pkt_len;

  char st30_ref_url[ST_APP_URL_MAX_LEN];
  int st30_ref_fd;
  uint8_t* st30_ref_begin;
  uint8_t* st30_ref_end;
  uint8_t* st30_ref_cursor;

  pthread_t st30_app_thread;
  pthread_cond_t st30_wake_cond;
  pthread_mutex_t st30_wake_mutex;
  bool st30_app_thread_stop;

  /* stat */
  int stat_frame_total_received;
  uint64_t stat_frame_frist_rx_time;
  double expect_fps;
};

struct st_app_rx_anc_session {
  int idx;
  st40_rx_handle handle;
  pthread_t st40_app_thread;
  pthread_cond_t st40_wake_cond;
  pthread_mutex_t st40_wake_mutex;
  bool st40_app_thread_stop;

  /* stat */
  int stat_frame_total_received;
  uint64_t stat_frame_frist_rx_time;
};

struct st22_app_tx_session {
  int idx;
  st22_tx_handle handle;
  int rtp_frame_total_pkts;
  int rtp_pkt_size; /* pkt size include both rtp and payload */
  int rtp_pd_size;  /* payload size for each pkt */
  int width;
  int height;
  int st22_frame_size;

  pthread_cond_t st22_wake_cond;
  pthread_mutex_t st22_wake_mutex;
  bool st22_app_thread_stop;
  pthread_t st22_app_thread;
  char st22_source_url[ST_APP_URL_MAX_LEN];
  int st22_source_fd;
  uint8_t* st22_source_begin;
  uint8_t* st22_source_end;
  uint8_t* st22_frame_cursor;

  int st22_pkt_idx;          /* pkt index in current frame */
  uint16_t st22_seq_id;      /* seq id in current session */
  uint32_t st22_rtp_tmstamp; /* tmstamp for current frame */
  int st22_frame_idx;
  struct st_rfc3550_rtp_hdr st22_rtp_base;
};

struct st22_app_rx_session {
  int idx;
  st22_rx_handle handle;
  int rtp_frame_total_pkts;
  int rtp_pkt_size; /* pkt size include both rtp and payload */
  int rtp_pd_size;  /* payload size for each pkt */
  int width;
  int height;
  int st22_frame_size;

  int st22_pkt_idx; /* pkt index in current frame */
  uint32_t st22_last_tmstamp;

  pthread_cond_t st22_wake_cond;
  pthread_mutex_t st22_wake_mutex;
  bool st22_app_thread_stop;
  pthread_t st22_app_thread;

  char st22_dst_url[ST_APP_URL_MAX_LEN];
  int st22_dst_fb_cnt; /* the count of recevied fbs will be saved to file */
  int st22_dst_fd;
  uint8_t* st22_dst_begin;
  uint8_t* st22_dst_end;
  uint8_t* st22_dst_cursor;
};

struct st_app_context {
  st_json_context_t* json_ctx;
  struct st_init_params para;
  st_handle st;
  int test_time_s;
  bool stop;
  uint8_t tx_dip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN]; /* tx destination IP */

  int last_session_sch_index;
  int lcore[ST_APP_MAX_LCORES];

  char tx_video_url[ST_APP_URL_MAX_LEN]; /* send video content url*/
  struct st_app_tx_video_session tx_video_sessions[ST_APP_MAX_TX_VIDEO_SESSIONS];
  int tx_video_session_cnt;
  int tx_video_rtp_ring_size; /* the ring size for tx video rtp type */

  struct st_app_tx_audio_session tx_audio_sessions[ST_APP_MAX_TX_AUDIO_SESSIONS];
  char tx_audio_url[ST_APP_URL_MAX_LEN];
  int tx_audio_session_cnt;
  int tx_audio_rtp_ring_size; /* the ring size for tx audio rtp type */

  struct st_app_tx_anc_session tx_anc_sessions[ST_APP_MAX_TX_ANC_SESSIONS];
  char tx_anc_url[ST_APP_URL_MAX_LEN];
  int tx_anc_session_cnt;
  int tx_anc_rtp_ring_size; /* the ring size for tx anc rtp type */

  uint8_t rx_sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN]; /* rx source IP */

  struct st_app_rx_video_session rx_video_sessions[ST_APP_MAX_RX_VIDEO_SESSIONS];
  int rx_video_session_cnt;
  int rx_video_file_frames;   /* the frames recevied saved to file */
  int rx_video_rtp_ring_size; /* the ring size for rx video rtp type */
  bool display;               /* flag to display all rx video with SDL */
  bool has_sdl;               /* has SDL device or not*/

  struct st_app_rx_audio_session rx_audio_sessions[ST_APP_MAX_RX_AUDIO_SESSIONS];
  int rx_audio_session_cnt;
  int rx_audio_rtp_ring_size; /* the ring size for rx audio rtp type */

  struct st_app_rx_anc_session rx_anc_sessions[ST_APP_MAX_RX_ANC_SESSIONS];
  int rx_anc_session_cnt;

  char tx_st22_url[ST_APP_URL_MAX_LEN]; /* send st22 content url*/
  struct st22_app_tx_session tx_st22_sessions[ST_APP_MAX_TX_VIDEO_SESSIONS];
  int tx_st22_session_cnt;
  struct st22_app_rx_session rx_st22_sessions[ST_APP_MAX_RX_VIDEO_SESSIONS];
  int rx_st22_session_cnt;
  int st22_rtp_frame_total_pkts;
  int st22_rtp_pkt_size; /* pkt size include both rtp and payload */
};

static inline void* st_app_malloc(size_t sz) { return malloc(sz); }

static inline void* st_app_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void st_app_free(void* p) { free(p); }

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_app_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

int st_app_video_get_lcore(struct st_app_context* ctx, int sch_idx, unsigned int* lcore);

#endif
