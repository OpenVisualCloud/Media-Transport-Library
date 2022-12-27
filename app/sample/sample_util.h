/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_APP_SAMPLE_UTIL_H_
#define _ST_APP_SAMPLE_UTIL_H_

#include <errno.h>
#include <fcntl.h>
#include <mtl/st20_redundant_api.h>
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
  uint8_t rx_sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];  /* rx source IP */
  uint8_t fwd_dip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN]; /* fwd destination IP */
  char tx_url[ST_SAMPLE_URL_MAX_LEN];
  char rx_url[ST_SAMPLE_URL_MAX_LEN];

  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st20_fmt fmt;
  enum st_frame_fmt input_fmt;
  enum st_frame_fmt output_fmt;
  uint16_t framebuff_cnt;
  uint16_t udp_port;
  uint8_t payload_type;
  uint32_t sessions; /* number of sessions */
  bool ext_frame;
  bool hdr_split;

  char logo_url[ST_SAMPLE_URL_MAX_LEN];
  uint32_t logo_width;
  uint32_t logo_height;

  enum st_frame_fmt st22p_input_fmt;
  enum st_frame_fmt st22p_output_fmt;
  enum st22_codec st22p_codec; /* st22 codec */

  enum sample_udp_mode udp_mode;
  uint64_t udp_tx_bps;

  bool exit;
  void (*sig_handler)(int signo);
};

int sample_parse_args(struct st_sample_context* ctx, int argc, char** argv, bool tx,
                      bool rx, bool unicast);

int tx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int rx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int fwd_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

int dma_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv);

void fill_rfc4175_422_10_pg2_data(struct st20_rfc4175_422_10_pg2_be* data, int w, int h);

#endif
