/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_APP_SAMPLE_UTIL_H_
#define _ST_APP_SAMPLE_UTIL_H_

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mtl/experimental/st20_combined_api.h>
#include <mtl/st30_pipeline_api.h>
#include <mtl/st40_pipeline_api.h>
#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef MTL_GPU_DIRECT_ENABLED
#include <mtl_gpu_direct/gpu.h>
#endif /* MTL_GPU_DIRECT_ENABLED */

#include "../src/app_platform.h"

/* log define */
#ifdef DEBUG
#define dbg(...)         \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)        \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)
#define warn(...)        \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)
#define err(...)         \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)

#define ST_SAMPLE_URL_MAX_LEN (256)

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

#define ST_MAX(a, b) ((a) > (b) ? (a) : (b))
#define ST_MIN(a, b) ((a) < (b) ? (a) : (b))

enum sample_udp_mode {
  /* client/server mode */
  SAMPLE_UDP_DEFAULT = 0,
  /* transport only */
  SAMPLE_UDP_TRANSPORT,
  /* transport with poll */
  SAMPLE_UDP_TRANSPORT_POLL,
  /* transport with unify poll */
  SAMPLE_UDP_TRANSPORT_UNIFY_POLL,
  SAMPLE_UDP_MODE_MAX,
};

struct st_sample_context {
  mtl_handle st;
  struct mtl_init_params param;
  uint8_t tx_dip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];  /* tx destination IP */
  uint8_t rx_ip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];   /* rx source IP */
  uint8_t fwd_dip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN]; /* fwd destination IP */
  char tx_url[ST_SAMPLE_URL_MAX_LEN];
  char rx_url[ST_SAMPLE_URL_MAX_LEN];
  bool has_tx_dst_mac[MTL_PORT_MAX];
  uint8_t tx_dst_mac[MTL_PORT_MAX][MTL_MAC_ADDR_LEN];

  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  bool interlaced;
  bool split_anc_by_pkt;
  enum st20_fmt fmt;
  enum st_frame_fmt input_fmt;
  enum st_frame_fmt output_fmt;
  enum st20_packing packing;
  uint16_t framebuff_cnt;
  uint16_t udp_port;
  uint8_t payload_type;
  uint32_t sessions; /* number of sessions */
  bool ext_frame;
  bool hdr_split;
  bool rx_dump;
  uint16_t rx_burst_size;
  /* use a new ip addr instead of a new udp port for multi sessions */
  bool multi_inc_addr;

  char tx_audio_url[ST_SAMPLE_URL_MAX_LEN];
  char rx_audio_url[ST_SAMPLE_URL_MAX_LEN];
  uint16_t audio_udp_port;
  uint8_t audio_payload_type;
  enum st30_fmt audio_fmt;
  uint16_t audio_channel;
  enum st30_sampling audio_sampling;
  enum st30_ptime audio_ptime;

  char logo_url[ST_SAMPLE_URL_MAX_LEN];
  uint32_t logo_width;
  uint32_t logo_height;

  enum st22_codec st22p_codec; /* st22 codec */

  enum sample_udp_mode udp_mode;
  uint64_t udp_tx_bps;
  int udp_len;

  bool exit;
  void (*sig_handler)(int signo);

  /* the PA of gpu PCIE bar which connected with GDDR */
  off_t gddr_pa;
  off_t gddr_offset;
  bool use_cpu_copy;
  bool profiling_gddr;

  bool has_user_meta; /* if provide user meta data with the st2110-20 frame */

  /* perf */
  int perf_frames;
  int perf_fb_cnt;

#ifdef MTL_GPU_DIRECT_ENABLED
  /* gpu direct */
  GpuContext* gpu_ctx;
#endif /* MTL_GPU_DIRECT_ENABLED */
};

struct st_frame_user_meta {
  int idx; /* frame index */
  char dummy[512];
};

int sample_parse_args(struct st_sample_context* ctx, int argc, char** argv, bool tx,
                      bool rx, bool unicast);

int tx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int rx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int fwd_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int dma_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

void fill_rfc4175_422_10_pg2_data(struct st20_rfc4175_422_10_pg2_be* data, int w, int h);

void fill_rfc4175_422_12_pg2_data(struct st20_rfc4175_422_12_pg2_be* data, int w, int h);

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t sample_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

int ufd_override_check(struct st_sample_context* ctx);

int sample_tx_queue_cnt_set(struct st_sample_context* ctx, uint16_t cnt);

int sample_rx_queue_cnt_set(struct st_sample_context* ctx, uint16_t cnt);

#endif
