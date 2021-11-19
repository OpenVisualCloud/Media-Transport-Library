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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "args.h"
#include "log.h"
#include "player.h"
#include "rx_ancillary_app.h"
#include "rx_audio_app.h"
#include "rx_st22_app.h"
#include "rx_video_app.h"
#include "tx_ancillary_app.h"
#include "tx_audio_app.h"
#include "tx_st22_app.h"
#include "tx_video_app.h"

static struct st_app_context* g_app_ctx; /* only for st_app_sig_handler */
static enum st_log_level app_log_level;

static void app_stat(void* priv) {
  struct st_app_context* ctx = priv;

  st_app_rx_video_sessions_stat(ctx);
}

void app_set_log_level(enum st_log_level level) { app_log_level = level; }

enum st_log_level app_get_log_level(void) { return app_log_level; }

static uint64_t app_ptp_from_real_time(void* priv) {
  struct timespec spec;

  clock_gettime(CLOCK_REALTIME, &spec);
  return ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
}

static void user_param_init(struct st_app_context* ctx, struct st_init_params* p) {
  memset(p, 0x0, sizeof(*p));

  p->flags = ST_FLAG_BIND_NUMA; /* default bind to numa */
  p->priv = ctx;
  p->ptp_get_time_fn = app_ptp_from_real_time;
  p->stat_dump_cb_fn = app_stat;
  p->log_level = ST_LOG_LEVEL_INFO;
  app_set_log_level(p->log_level);
}

static void st_app_ctx_init(struct st_app_context* ctx) {
  user_param_init(ctx, &ctx->para);

  /* tx */
  strncpy(ctx->tx_video_url, "test.yuv", sizeof(ctx->tx_video_url));
  ctx->tx_video_session_cnt = 0;
  strncpy(ctx->tx_audio_url, "test.wav", sizeof(ctx->tx_audio_url));
  ctx->tx_audio_session_cnt = 0;
  strncpy(ctx->tx_anc_url, "test.txt", sizeof(ctx->tx_anc_url));
  ctx->tx_anc_session_cnt = 0;
  strncpy(ctx->tx_st22_url, "test.raw", sizeof(ctx->tx_st22_url));
  ctx->tx_st22_session_cnt = 0;

  /* rx */
  ctx->rx_video_session_cnt = 0;
  ctx->rx_audio_session_cnt = 0;
  ctx->rx_anc_session_cnt = 0;
  ctx->rx_st22_session_cnt = 0;

  /* st22 */
  ctx->st22_rtp_frame_total_pkts = 540; /* compress ratio 1/8, 4320/8 */
  ctx->st22_rtp_pkt_size = 1280 + sizeof(struct st22_rfc9143_rtp_hdr);

  ctx->lcore = -1;
}

static void st_app_ctx_free(struct st_app_context* ctx) {
  st_app_tx_video_sessions_stop(ctx);
  st_app_tx_audio_sessions_stop(ctx);
  st_app_tx_anc_sessions_stop(ctx);
  st22_app_tx_sessions_stop(ctx);

  /* stop st first */
  if (ctx->st) st_stop(ctx->st);

  if (ctx->json_ctx) st_app_free(ctx->json_ctx);

  st_app_tx_audio_sessions_uinit(ctx);
  st_app_tx_anc_sessions_uinit(ctx);
  st22_app_tx_sessions_uinit(ctx);

  st_app_tx_video_sessions_handle_uinit(ctx);

  st_app_rx_video_sessions_uinit(ctx);
  st_app_rx_audio_sessions_uinit(ctx);
  st_app_rx_anc_sessions_uinit(ctx);
  st22_app_rx_sessions_uinit(ctx);

  if (ctx->st) {
    if (ctx->lcore >= 0) st_put_lcore(ctx->st, ctx->lcore);
    st_uninit(ctx->st);
    ctx->st = NULL;
  }

  st_app_tx_video_sessions_uinit(ctx);
  st_app_player_uinit(ctx);
  st_app_free(ctx);
}

static void st_app_sig_handler(int signo) {
  struct st_app_context* ctx = g_app_ctx;

  info("%s, signal %d\n", __func__, signo);
  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      if (ctx->st) st_request_exit(ctx->st);
      ctx->stop = true;
      break;
  }

  return;
}

int main(int argc, char** argv) {
  int ret;
  struct st_app_context* ctx;
  int run_time_s = 0;
  int test_time_s;
  unsigned int lcore;

  ctx = st_app_zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }

  st_app_ctx_init(ctx);

  ret = st_app_parse_args(ctx, &ctx->para, argc, argv);
  if (ret < 0) {
    err("%s, st_app_parse_args fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return ret;
  }
  ctx->para.tx_sessions_cnt_max = ctx->tx_video_session_cnt + ctx->tx_audio_session_cnt +
                                  ctx->tx_anc_session_cnt + ctx->tx_st22_session_cnt;
  ctx->para.rx_sessions_cnt_max = ctx->rx_video_session_cnt + ctx->rx_audio_session_cnt +
                                  ctx->rx_anc_session_cnt + ctx->rx_st22_session_cnt;

  ctx->st = st_init(&ctx->para);
  if (!ctx->st) {
    err("%s, st_init fail\n", __func__);
    st_app_ctx_free(ctx);
    return -ENOMEM;
  }

  /* get one lcore for app usage */
  ret = st_get_lcore(ctx->st, &lcore);
  if (ret < 0) {
    err("%s, st_get_lcore fail %d\n", __func__, ret);
  } else {
    ctx->lcore = lcore;
  }

  g_app_ctx = ctx;

  if (signal(SIGINT, st_app_sig_handler) == SIG_ERR) {
    err("%s, cat SIGINT fail\n", __func__);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_player_init(ctx);
  if (ret < 0) {
    ctx->has_sdl = false;
  } else {
    ctx->has_sdl = true;
  }

  ret = st_app_tx_video_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_video_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_tx_audio_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_audio_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_tx_anc_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_anc_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st22_app_tx_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st22_app_tx_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_video_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_video_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_audio_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_audio_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_anc_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_anc_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st22_app_rx_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st22_app_rx_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_start(ctx->st);
  if (ret < 0) {
    err("%s, start dev fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  test_time_s = ctx->test_time_s;
  info("%s, app lunch succ, test time %ds\n", __func__, test_time_s);
  while (!ctx->stop) {
    sleep(1);
    run_time_s++;
    if (test_time_s && (run_time_s > test_time_s)) break;
  }
  info("%s, start to ending\n", __func__);

  /* free */
  st_app_ctx_free(ctx);

  return 0;
}
