/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifdef APP_HAS_SDL2
#include <SDL2/SDL.h>
#endif
#ifdef APP_HAS_SDL2_TTF
#include <SDL2/SDL_ttf.h>
#endif
#include <errno.h>
#include <mtl/experimental/st20_combined_api.h>
#include <mtl/st20_api.h>
#include <mtl/st30_api.h>
#include <mtl/st40_api.h>
#include <mtl/st_pipeline_api.h>
#include <pcap.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_platform.h"
#include "fmt.h"
#include "parse_json.h"

#ifndef _ST_APP_BASE_HEAD_H_
#define _ST_APP_BASE_HEAD_H_

#define ST_APP_MAX_TX_VIDEO_SESSIONS (180)
#define ST_APP_MAX_RX_VIDEO_SESSIONS (180)

#define ST_APP_MAX_TX_AUDIO_SESSIONS (1024)
#define ST_APP_MAX_RX_AUDIO_SESSIONS (1024)

#define ST_APP_MAX_TX_ANC_SESSIONS (180)
#define ST_APP_MAX_RX_ANC_SESSIONS (180)

#define ST_APP_MAX_LCORES (32)

#define ST_APP_EXPECT_NEAR(val, expect, delta) \
  ((val > (expect - delta)) && (val < (expect + delta)))

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

#ifndef NS_PER_US
#define NS_PER_US (1000)
#endif

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

#define UTC_OFFSET (37) /* 2022/07 */

#define ST_MAX(a, b) ((a) > (b) ? (a) : (b))

struct st_display {
  char name[36];
#ifdef APP_HAS_SDL2
  SDL_Window* window;
  SDL_Renderer* renderer;
  SDL_Texture* texture;
  SDL_PixelFormatEnum fmt;
  SDL_Rect msg_rect;
#endif
#ifdef APP_HAS_SDL2_TTF
  TTF_Font* font;
#endif
  int window_w;
  int window_h;
  int pixel_w;
  int pixel_h;
  void* front_frame;
  int front_frame_size;
  uint32_t last_time;
  uint32_t frame_cnt;
  double fps;

  pthread_t display_thread;
  bool display_thread_stop;
  pthread_cond_t display_wake_cond;
  pthread_mutex_t display_wake_mutex;
  pthread_mutex_t display_frame_mutex;
};

struct st_app_frameinfo {
  bool used;
  bool second_field;
  uint16_t lines_ready;
};

struct st_app_tx_video_session {
  int idx;
  mtl_handle st;
  st20_tx_handle handle;
  int handle_sch_idx;

  struct st_app_context* ctx;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;
  bool sha_check;

  pcap_t* st20_pcap;
  bool st20_pcap_input;

  char st20_source_url[ST_APP_URL_MAX_LEN];
  uint8_t* st20_source_begin;
  uint8_t* st20_source_end;
  uint8_t* st20_frame_cursor;
  int st20_source_fd;
  bool st20_frames_copied;

  int st20_frame_size;
  bool st20_second_field;
  struct st20_pgroup st20_pg;
  uint16_t lines_per_slice;

  int width;
  int height;
  bool interlaced;
  bool second_field;
  bool single_line;
  bool slice;
  bool enable_vsync;
  uint8_t num_port;
  uint64_t last_stat_time_ns;

  /* rtp mode info */
  bool st20_rtp_input;
  int st20_pkts_in_line;  /* GPM only, number of packets per each line, 4 for 1080p */
  int st20_bytes_in_line; /* bytes per line, 4800 for 1080p yuv422 10bit */
  uint32_t
      st20_pkt_data_len; /* data len(byte) for each pkt, 1200 for 1080p yuv422 10bit */
  struct st20_rfc4175_rtp_hdr st20_rtp_base;
  int st20_total_pkts;  /* total pkts in one frame, ex: 4320 for 1080p */
  int st20_pkt_idx;     /* pkt index in current frame */
  uint32_t st20_seq_id; /* seq id in current frame */
  uint32_t st20_rtp_tmstamp;
  uint8_t payload_type;

  double expect_fps;
  uint64_t stat_frame_first_tx_time;
  uint32_t st20_frame_done_cnt;
  uint32_t st20_packet_done_cnt;

  pthread_t st20_app_thread;
  bool st20_app_thread_stop;
  pthread_cond_t st20_wake_cond;
  pthread_mutex_t st20_wake_mutex;

  struct st_display* display;
  int lcore;
};

struct st_app_tx_audio_session {
  int idx;
  st30_tx_handle handle;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  int st30_frame_done_cnt;
  int st30_packet_done_cnt;

  char st30_source_url[ST_APP_URL_MAX_LEN + 1];
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
  enum st30_sampling sampling;
};

struct st_app_tx_anc_session {
  int idx;
  st40_tx_handle handle;

  uint16_t framebuff_cnt;

  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  uint32_t st40_frame_done_cnt;
  uint32_t st40_packet_done_cnt;

  char st40_source_url[ST_APP_URL_MAX_LEN + 1];
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
  mtl_handle st;
  st20_rx_handle handle;
  st20rc_rx_handle st20r_handle; /* for st20r */
  int framebuff_cnt;
  int st20_frame_size;
  bool slice;
  uint8_t num_port;
  uint64_t last_stat_time_ns;
  bool sha_check;

  char st20_dst_url[ST_APP_URL_MAX_LEN];
  int st20_dst_fb_cnt; /* the count of received fbs will be saved to file */
  int st20_dst_fd;
  uint8_t* st20_dst_begin;
  uint8_t* st20_dst_end;
  uint8_t* st20_dst_cursor;

  /* frame info */
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  /* rtp info */
  uint32_t st20_last_tmstamp;
  struct st20_pgroup st20_pg;
  struct user_pgroup user_pg;
  int width;
  int height;
  bool interlaced;

  /* stat */
  int stat_frame_received;
  uint64_t stat_last_time;
  int stat_frame_total_received;
  uint64_t stat_frame_first_rx_time;
  double expect_fps;

  pthread_t st20_app_thread;
  pthread_cond_t st20_wake_cond;
  pthread_mutex_t st20_wake_mutex;
  bool st20_app_thread_stop;

  struct st_display* display;
  uint32_t pcapng_max_pkts;

  bool measure_latency;
  uint64_t stat_latency_us_sum;
};

struct st_app_rx_audio_session {
  int idx;
  st30_rx_handle handle;
  int framebuff_cnt;
  int st30_frame_size;
  int pkt_len;

  char st30_ref_url[ST_APP_URL_MAX_LEN + 1];
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
  uint64_t stat_frame_first_rx_time;
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
  uint64_t stat_frame_first_rx_time;
};

struct st22_app_tx_session {
  int idx;
  st22_tx_handle handle;

  int width;
  int height;
  enum st22_type type;
  int bpp;
  size_t bytes_per_frame;

  struct st_app_context* ctx;
  mtl_handle st;
  int lcore;
  int handle_sch_idx;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  bool st22_app_thread_stop;
  pthread_t st22_app_thread;
  char st22_source_url[ST_APP_URL_MAX_LEN];
  int st22_source_fd;
  uint8_t* st22_source_begin;
  uint8_t* st22_source_end;
  uint8_t* st22_frame_cursor;

  int fb_send;
};

struct st22_app_rx_session {
  int idx;
  st22_rx_handle handle;
  int width;
  int height;
  int bpp;
  size_t bytes_per_frame;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  bool st22_app_thread_stop;
  pthread_t st22_app_thread;
  int fb_decoded;

  char st22_dst_url[ST_APP_URL_MAX_LEN];
  int st22_dst_fb_cnt; /* the count of received fbs will be saved to file */
  int st22_dst_fd;
  uint8_t* st22_dst_begin;
  uint8_t* st22_dst_end;
  uint8_t* st22_dst_cursor;
};

struct st_app_tx_st22p_session {
  int idx;
  st22p_tx_handle handle;
  mtl_handle st;
  int framebuff_cnt;
  int st22p_frame_size;
  int width;
  int height;

  char st22p_source_url[ST_APP_URL_MAX_LEN];
  uint8_t* st22p_source_begin;
  uint8_t* st22p_source_end;
  uint8_t* st22p_frame_cursor;
  int st22p_source_fd;

  struct st_display* display;
  double expect_fps;

  pthread_t st22p_app_thread;
  pthread_cond_t st22p_wake_cond;
  pthread_mutex_t st22p_wake_mutex;
  bool st22p_app_thread_stop;
};

struct st_app_rx_st22p_session {
  int idx;
  mtl_handle st;
  st22p_rx_handle handle;
  int framebuff_cnt;
  int st22p_frame_size;
  bool slice;
  int width;
  int height;

  /* stat */
  int stat_frame_received;
  uint64_t stat_last_time;
  int stat_frame_total_received;
  uint64_t stat_frame_first_rx_time;
  double expect_fps;

  pthread_t st22p_app_thread;
  pthread_cond_t st22p_wake_cond;
  pthread_mutex_t st22p_wake_mutex;
  bool st22p_app_thread_stop;

  struct st_display* display;
  uint32_t pcapng_max_pkts;

  bool measure_latency;
  uint64_t stat_latency_us_sum;
};

struct st_app_tx_st20p_session {
  int idx;
  st20p_tx_handle handle;
  mtl_handle st;
  int framebuff_cnt;
  int st20p_frame_size;
  int width;
  int height;
  uint8_t num_port;
  uint64_t last_stat_time_ns;
  bool sha_check;

  char st20p_source_url[ST_APP_URL_MAX_LEN];
  uint8_t* st20p_source_begin;
  uint8_t* st20p_source_end;
  uint8_t* st20p_frame_cursor;
  int st20p_source_fd;

  struct st_display* display;
  double expect_fps;

  pthread_t st20p_app_thread;
  pthread_cond_t st20p_wake_cond;
  pthread_mutex_t st20p_wake_mutex;
  bool st20p_app_thread_stop;
};

struct st_app_rx_st20p_session {
  int idx;
  st20p_rx_handle handle;
  mtl_handle st;
  int framebuff_cnt;
  int st20p_frame_size;
  int width;
  int height;
  uint8_t num_port;
  uint64_t last_stat_time_ns;
  bool sha_check;

  /* stat */
  int stat_frame_received;
  uint64_t stat_last_time;
  int stat_frame_total_received;
  uint64_t stat_frame_first_rx_time;
  double expect_fps;

  pthread_t st20p_app_thread;
  pthread_cond_t st20p_wake_cond;
  pthread_mutex_t st20p_wake_mutex;
  bool st20p_app_thread_stop;

  struct st_display* display;
  uint32_t pcapng_max_pkts;

  bool measure_latency;
  uint64_t stat_latency_us_sum;
};

struct st_app_var_params {
  /* force sleep time(us) for sch tasklet sleep */
  uint64_t sch_force_sleep_us;
};

struct st_app_context {
  st_json_context_t* json_ctx;
  struct mtl_init_params para;
  struct st_app_var_params var_para;
  mtl_handle st;
  int test_time_s;
  bool stop;
  uint8_t tx_dip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN]; /* tx destination IP */
  bool has_tx_dst_mac[MTL_PORT_MAX];
  uint8_t tx_dst_mac[MTL_PORT_MAX][MTL_MAC_ADDR_LEN];

  int lcore[ST_APP_MAX_LCORES];
  int rtp_lcore[ST_APP_MAX_LCORES];
  FILE* mtl_log_stream;
  uint64_t last_stat_time_ns;

  bool runtime_session;
  bool enable_hdr_split;
  bool tx_copy_once;
  bool app_thread;
  bool enable_timing_parser;
  bool tx_display;
  bool rx_display;

  bool ptp_systime_sync;
  int ptp_sync_cnt;
  int64_t ptp_sync_delta_sum;
  int64_t ptp_sync_delta_max;
  int64_t ptp_sync_delta_min;

  char tx_video_url[ST_APP_URL_MAX_LEN]; /* send video content url*/
  struct st_app_tx_video_session* tx_video_sessions;
  int tx_video_session_cnt;
  int tx_video_rtp_ring_size; /* the ring size for tx video rtp type */
  uint16_t tx_start_vrx;
  uint16_t tx_pad_interval;
  bool tx_no_static_pad;
  bool tx_ts_first_pkt;
  bool tx_ts_epoch;
  int32_t tx_ts_delta_us;
  enum st21_pacing tx_pacing_type;
  bool tx_no_bulk;
  bool video_sha_check;

  struct st_app_tx_audio_session* tx_audio_sessions;
  char tx_audio_url[ST_APP_URL_MAX_LEN];
  int tx_audio_session_cnt;
  int tx_audio_rtp_ring_size; /* the ring size for tx audio rtp type */
  bool tx_audio_build_pacing;
  int tx_audio_fifo_size;

  struct st_app_tx_anc_session* tx_anc_sessions;
  char tx_anc_url[ST_APP_URL_MAX_LEN];
  int tx_anc_session_cnt;
  int tx_anc_rtp_ring_size; /* the ring size for tx anc rtp type */

  char tx_st22p_url[ST_APP_URL_MAX_LEN]; /* send st22p content url*/
  struct st_app_tx_st22p_session* tx_st22p_sessions;
  int tx_st22p_session_cnt;

  char tx_st20p_url[ST_APP_URL_MAX_LEN]; /* send st20p content url*/
  struct st_app_tx_st20p_session* tx_st20p_sessions;
  int tx_st20p_session_cnt;

  uint8_t rx_sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];       /* rx source IP */
  uint8_t rx_mcast_sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN]; /* rx multicast source IP */

  struct st_app_rx_video_session* rx_video_sessions;
  int rx_video_session_cnt;
  int rx_video_file_frames; /* the frames received saved to file */
  int rx_video_fb_cnt;
  int rx_video_rtp_ring_size; /* the ring size for rx video rtp type */
  bool has_sdl;               /* has SDL device or not*/

  struct st_app_rx_audio_session* rx_audio_sessions;
  int rx_audio_session_cnt;
  int rx_audio_rtp_ring_size; /* the ring size for rx audio rtp type */

  struct st_app_rx_anc_session* rx_anc_sessions;
  int rx_anc_session_cnt;

  struct st_app_rx_st22p_session* rx_st22p_sessions;
  int rx_st22p_session_cnt;

  struct st_app_rx_st20p_session* rx_st20p_sessions;
  int rx_st20p_session_cnt;

  struct st_app_rx_video_session* rx_st20r_sessions;
  int rx_st20r_session_cnt;

  char tx_st22_url[ST_APP_URL_MAX_LEN]; /* send st22 content url*/
  struct st22_app_tx_session* tx_st22_sessions;
  int tx_st22_session_cnt;
  struct st22_app_rx_session* rx_st22_sessions;
  int rx_st22_session_cnt;
  int st22_bpp;

  uint32_t pcapng_max_pkts;
  char ttf_file[ST_APP_URL_MAX_LEN];
  int utc_offset;
};

static inline void* st_app_malloc(size_t sz) { return malloc(sz); }

static inline void* st_app_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void st_app_free(void* p) { free(p); }

static inline uint64_t st_timespec_to_ns(const struct timespec* ts) {
  return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
}

static inline void st_ns_to_timespec(uint64_t ns, struct timespec* ts) {
  ts->tv_sec = ns / NS_PER_S;
  ts->tv_nsec = ns % NS_PER_S;
}

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_app_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return st_timespec_to_ns(&ts);
}

int st_app_video_get_lcore(struct st_app_context* ctx, int sch_idx, bool rtp,
                           unsigned int* lcore);

uint8_t* st_json_ip(struct st_app_context* ctx, st_json_session_base_t* base,
                    enum mtl_session_port port);

int st_set_mtl_log_file(struct st_app_context* ctx, const char* file);

void st_sha_dump(const char* tag, const unsigned char* sha);

#endif
