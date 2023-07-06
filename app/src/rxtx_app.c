/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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
#include "rx_st20p_app.h"
#include "rx_st20r_app.h"
#include "rx_st22_app.h"
#include "rx_st22p_app.h"
#include "rx_video_app.h"
#include "tx_ancillary_app.h"
#include "tx_audio_app.h"
#include "tx_st20p_app.h"
#include "tx_st22_app.h"
#include "tx_st22p_app.h"
#include "tx_video_app.h"

static struct st_app_context* g_app_ctx; /* only for st_app_sig_handler */
static enum mtl_log_level app_log_level;

static void app_stat(void* priv) {
  struct st_app_context* ctx = priv;

  st_app_rx_video_sessions_stat(ctx);
  st_app_rx_st22p_sessions_stat(ctx);
  st_app_rx_st20p_sessions_stat(ctx);
  st_app_rx_st20r_sessions_stat(ctx);
}

void app_set_log_level(enum mtl_log_level level) { app_log_level = level; }

enum mtl_log_level app_get_log_level(void) { return app_log_level; }

static uint64_t app_ptp_from_tai_time(void* priv) {
  struct st_app_context* ctx = priv;
  struct timespec spec;
  st_getrealtime(&spec);
  spec.tv_sec -= ctx->utc_offset;
  return ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
}

static void user_param_init(struct st_app_context* ctx, struct mtl_init_params* p) {
  memset(p, 0x0, sizeof(*p));

  p->pmd[MTL_PORT_P] = MTL_PMD_DPDK_USER;
  p->pmd[MTL_PORT_R] = MTL_PMD_DPDK_USER;
  /* default start queue set to 1 */
  p->xdp_info[MTL_PORT_P].start_queue = 1;
  p->xdp_info[MTL_PORT_R].start_queue = 1;
  p->flags |= MTL_FLAG_BIND_NUMA; /* default bind to numa */
  p->flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
  p->flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
  p->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  p->priv = ctx;
  p->ptp_get_time_fn = app_ptp_from_tai_time;
  p->stat_dump_cb_fn = app_stat;
  p->log_level = MTL_LOG_LEVEL_INFO;
  app_set_log_level(p->log_level);
}

static void var_param_init(struct st_app_context* ctx) {
  if (ctx->var_para.sch_force_sleep_us)
    mtl_sch_set_sleep_us(ctx->st, ctx->var_para.sch_force_sleep_us);
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
  strncpy(ctx->tx_st22p_url, "test_rfc4175.yuv", sizeof(ctx->tx_st22p_url));
  ctx->tx_st22p_session_cnt = 0;
  strncpy(ctx->tx_st20p_url, "test_rfc4175.yuv", sizeof(ctx->tx_st20p_url));
  ctx->tx_st20p_session_cnt = 0;

  /* rx */
  ctx->rx_video_session_cnt = 0;
  ctx->rx_audio_session_cnt = 0;
  ctx->rx_anc_session_cnt = 0;
  ctx->rx_st22_session_cnt = 0;
  ctx->rx_st22p_session_cnt = 0;
  ctx->rx_st20p_session_cnt = 0;
  ctx->rx_st20r_session_cnt = 0;

  /* st22 */
  ctx->st22_bpp = 3; /* 3bit per pixel */

  ctx->utc_offset = UTC_OFFSET;

  /* init lcores and sch */
  for (int i = 0; i < ST_APP_MAX_LCORES; i++) {
    ctx->lcore[i] = -1;
    ctx->rtp_lcore[i] = -1;
  }
}

int st_app_video_get_lcore(struct st_app_context* ctx, int sch_idx, bool rtp,
                           unsigned int* lcore) {
  int ret;
  unsigned int video_lcore;

  if (sch_idx < 0 || sch_idx >= ST_APP_MAX_LCORES) {
    err("%s, invalid sch idx %d\n", __func__, sch_idx);
    return -EINVAL;
  }

  if (rtp) {
    if (ctx->rtp_lcore[sch_idx] < 0) {
      ret = mtl_get_lcore(ctx->st, &video_lcore);
      if (ret < 0) return ret;
      ctx->rtp_lcore[sch_idx] = video_lcore;
      info("%s, new rtp lcore %d for sch idx %d\n", __func__, video_lcore, sch_idx);
    }
  } else {
    if (ctx->lcore[sch_idx] < 0) {
      ret = mtl_get_lcore(ctx->st, &video_lcore);
      if (ret < 0) return ret;
      ctx->lcore[sch_idx] = video_lcore;
      info("%s, new lcore %d for sch idx %d\n", __func__, video_lcore, sch_idx);
    }
  }

  if (rtp)
    *lcore = ctx->rtp_lcore[sch_idx];
  else
    *lcore = ctx->lcore[sch_idx];
  return 0;
}

static void st_app_ctx_free(struct st_app_context* ctx) {
  st_app_tx_video_sessions_uinit(ctx);
  st_app_tx_audio_sessions_uinit(ctx);
  st_app_tx_anc_sessions_uinit(ctx);
  st_app_tx_st22p_sessions_uinit(ctx);
  st_app_tx_st20p_sessions_uinit(ctx);
  st22_app_tx_sessions_uinit(ctx);

  st_app_rx_video_sessions_uinit(ctx);
  st_app_rx_audio_sessions_uinit(ctx);
  st_app_rx_anc_sessions_uinit(ctx);
  st_app_rx_st22p_sessions_uinit(ctx);
  st_app_rx_st20p_sessions_uinit(ctx);
  st_app_rx_st20r_sessions_uinit(ctx);
  st22_app_rx_sessions_uinit(ctx);

  if (ctx->runtime_session) {
    if (ctx->st) mtl_stop(ctx->st);
  }

  if (ctx->json_ctx) {
    st_app_free_json(ctx->json_ctx);
    st_app_free(ctx->json_ctx);
  }

  if (ctx->st) {
    for (int i = 0; i < ST_APP_MAX_LCORES; i++) {
      if (ctx->lcore[i] >= 0) {
        mtl_put_lcore(ctx->st, ctx->lcore[i]);
        ctx->lcore[i] = -1;
      }
      if (ctx->rtp_lcore[i] >= 0) {
        mtl_put_lcore(ctx->st, ctx->rtp_lcore[i]);
        ctx->rtp_lcore[i] = -1;
      }
    }
    mtl_uninit(ctx->st);
    ctx->st = NULL;
  }

  st_app_player_uinit(ctx);
  st_app_free(ctx);
}

static int st_app_result(struct st_app_context* ctx) {
  int result = 0;

  result += st_app_tx_video_sessions_result(ctx);
  result += st_app_rx_video_sessions_result(ctx);
  result += st_app_rx_audio_sessions_result(ctx);
  result += st_app_rx_anc_sessions_result(ctx);
  result += st_app_rx_st22p_sessions_result(ctx);
  result += st_app_rx_st20p_sessions_result(ctx);
  result += st_app_rx_st20r_sessions_result(ctx);
  return result;
}

static int st_app_pcap(struct st_app_context* ctx) {
  st_app_rx_video_sessions_pcap(ctx);
  st_app_rx_st22p_sessions_pcap(ctx);
  st_app_rx_st20p_sessions_pcap(ctx);
  st_app_rx_st20r_sessions_pcap(ctx);
  return 0;
}

static void st_app_sig_handler(int signo) {
  struct st_app_context* ctx = g_app_ctx;

  info("%s, signal %d\n", __func__, signo);
  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      if (ctx->st) mtl_abort(ctx->st);
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
  if (ctx->tx_video_session_cnt > ST_APP_MAX_TX_VIDEO_SESSIONS ||
      ctx->tx_st22_session_cnt > ST_APP_MAX_TX_VIDEO_SESSIONS ||
      ctx->tx_st22p_session_cnt > ST_APP_MAX_TX_VIDEO_SESSIONS ||
      ctx->tx_st20p_session_cnt > ST_APP_MAX_TX_VIDEO_SESSIONS ||
      ctx->tx_audio_session_cnt > ST_APP_MAX_TX_AUDIO_SESSIONS ||
      ctx->tx_anc_session_cnt > ST_APP_MAX_TX_ANC_SESSIONS ||
      ctx->rx_video_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st22_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st22p_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st20p_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_audio_session_cnt > ST_APP_MAX_RX_AUDIO_SESSIONS ||
      ctx->rx_anc_session_cnt > ST_APP_MAX_RX_ANC_SESSIONS) {
    err("%s, session cnt invalid, pass the restriction\n", __func__);
    return -EINVAL;
  }

  int tx_st20_sessions = ctx->tx_video_session_cnt + ctx->tx_st22_session_cnt +
                         ctx->tx_st20p_session_cnt + ctx->tx_st22p_session_cnt;
  int rx_st20_sessions = ctx->rx_video_session_cnt + ctx->rx_st22_session_cnt +
                         ctx->rx_st22p_session_cnt + ctx->rx_st20p_session_cnt;
  for (int i = 0; i < ctx->para.num_ports; i++) {
    /* parse queue cnt, todo: split with ports */
    if (!ctx->para.tx_queues_cnt[i]) {
      ctx->para.tx_queues_cnt[i] = st_tx_sessions_queue_cnt(
          tx_st20_sessions, ctx->tx_audio_session_cnt, ctx->tx_anc_session_cnt);
    }
    if (!ctx->para.rx_queues_cnt[i]) {
      ctx->para.rx_queues_cnt[i] = st_rx_sessions_queue_cnt(
          rx_st20_sessions, ctx->rx_audio_session_cnt, ctx->rx_anc_session_cnt);
    }
    /* parse af xdp pmd info */
    ctx->para.pmd[i] = mtl_pmd_by_port_name(ctx->para.port[i]);
    ctx->para.xdp_info[i].queue_count =
        ST_MAX(ctx->para.tx_queues_cnt[i], ctx->para.rx_queues_cnt[i]);
  }

  /* hdr split special */
  if (ctx->enable_hdr_split) {
    ctx->para.nb_rx_hdr_split_queues = ctx->rx_video_session_cnt;
  }

  ctx->st = mtl_init(&ctx->para);
  if (!ctx->st) {
    err("%s, mtl_init fail\n", __func__);
    st_app_ctx_free(ctx);
    return -ENOMEM;
  }

  g_app_ctx = ctx;

  var_param_init(ctx);

  if (signal(SIGINT, st_app_sig_handler) == SIG_ERR) {
    err("%s, cat SIGINT fail\n", __func__);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  if (ctx->json_ctx && ctx->json_ctx->has_display) {
    ret = st_app_player_init(ctx);
    if (ret < 0) {
      ctx->has_sdl = false;
    } else {
      ctx->has_sdl = true;
    }
  }

  if (ctx->runtime_session) {
    ret = mtl_start(ctx->st);
    if (ret < 0) {
      err("%s, start dev fail %d\n", __func__, ret);
      st_app_ctx_free(ctx);
      return -EIO;
    }
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

  ret = st_app_tx_st22p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_st22p_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_tx_st20p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_st20p_sessions_init fail %d\n", __func__, ret);
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

  ret = st_app_rx_st22p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_st22p_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_st20p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_st20p_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_st20r_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_st20r_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  if (!ctx->runtime_session) {
    ret = mtl_start(ctx->st);
    if (ret < 0) {
      err("%s, start dev fail %d\n", __func__, ret);
      st_app_ctx_free(ctx);
      return -EIO;
    }
  }

  test_time_s = ctx->test_time_s;
  info("%s, app lunch succ, test time %ds\n", __func__, test_time_s);
  while (!ctx->stop) {
    sleep(1);
    run_time_s++;
    if (test_time_s && (run_time_s > test_time_s)) break;
    if (ctx->pcapng_max_pkts && (run_time_s == 10)) { /* trigger pcap dump if */
      st_app_pcap(ctx);
    }
  }
  info("%s, start to ending\n", __func__);

  if (!ctx->runtime_session) {
    /* stop st first */
    if (ctx->st) mtl_stop(ctx->st);
  }

  ret = st_app_result(ctx);

  /* free */
  st_app_ctx_free(ctx);

  return ret;
}
