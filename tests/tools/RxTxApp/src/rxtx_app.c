/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "args.h"
#include "experimental/rx_st20r_app.h"
#include "legacy/rx_audio_app.h"
#include "legacy/rx_st22_app.h"
#include "legacy/rx_video_app.h"
#include "legacy/tx_audio_app.h"
#include "legacy/tx_st22_app.h"
#include "legacy/tx_video_app.h"
#include "log.h"
#include "player.h"
#include "rx_ancillary_app.h"
#include "rx_fastmetadata_app.h"
#include "rx_st20p_app.h"
#include "rx_st22p_app.h"
#include "rx_st30p_app.h"
#include "rx_st40p_app.h"
#include "tx_ancillary_app.h"
#include "tx_fastmetadata_app.h"
#include "tx_st20p_app.h"
#include "tx_st22p_app.h"
#include "tx_st30p_app.h"
#include "tx_st40p_app.h"

static struct st_app_context* g_app_ctx; /* only for st_app_sig_handler */
static enum mtl_log_level app_log_level;

static int app_dump_io_stat(struct st_app_context* ctx) {
  struct mtl_fix_info fix;
  struct mtl_port_status stats;
  int ret;
  double time_sec =
      (double)(st_app_get_monotonic_time() - ctx->last_stat_time_ns) / NS_PER_S;
  double tx_rate_m, rx_rate_m;

  ret = mtl_get_fix_info(ctx->st, &fix);
  if (ret < 0) return ret;

  for (uint8_t port = 0; port < fix.num_ports; port++) {
    ret = mtl_get_port_stats(ctx->st, port, &stats);
    if (ret < 0) return ret;
    tx_rate_m = (double)stats.tx_bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    rx_rate_m = (double)stats.rx_bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    info("%s(%u), tx %f Mb/s rx %f Mb/s\n", __func__, port, tx_rate_m, rx_rate_m);
    if (stats.rx_hw_dropped_packets || stats.rx_err_packets || stats.rx_nombuf_packets ||
        stats.tx_err_packets) {
      warn("%s(%u), hw drop %" PRIu64 " rx err %" PRIu64 " no mbuf %" PRIu64
           " tx err %" PRIu64 "\n",
           __func__, port, stats.rx_hw_dropped_packets, stats.rx_err_packets,
           stats.rx_nombuf_packets, stats.tx_err_packets);
    }
    mtl_reset_port_stats(ctx->st, port);
  }

  return 0;
}

static int app_dump_ptp_sync_stat(struct st_app_context* ctx) {
  info("%s, cnt %d max %" PRId64 " min %" PRId64 " average %fus\n", __func__,
       ctx->ptp_sync_cnt, ctx->ptp_sync_delta_max, ctx->ptp_sync_delta_min,
       (float)ctx->ptp_sync_delta_sum / ctx->ptp_sync_cnt / NS_PER_US);
  ctx->ptp_sync_delta_sum = 0;
  ctx->ptp_sync_cnt = 0;
  ctx->ptp_sync_delta_max = INT64_MIN;
  ctx->ptp_sync_delta_min = INT64_MAX;
  return 0;
}

static void app_stat(void* priv) {
  struct st_app_context* ctx = priv;

  if (ctx->stop) return;

  if (ctx->mtl_log_stream) {
    app_dump_io_stat(ctx);
    st_app_tx_videos_io_stat(ctx);
    st_app_rx_videos_io_stat(ctx);
    st_app_tx_st20p_io_stat(ctx);
    st_app_rx_st20p_io_stat(ctx);
  }

  st_app_rx_video_sessions_stat(ctx);
  st_app_rx_st22p_sessions_stat(ctx);
  st_app_rx_st20p_sessions_stat(ctx);
  st_app_rx_st20r_sessions_stat(ctx);
  st_app_rx_audio_sessions_stat(ctx);
  st_app_rx_st30p_sessions_stat(ctx);
  st_app_rx_st40p_sessions_stat(ctx);

  if (ctx->ptp_systime_sync) {
    app_dump_ptp_sync_stat(ctx);
  }

  ctx->last_stat_time_ns = st_app_get_monotonic_time();
}

static void app_ptp_sync_notify(void* priv, struct mtl_ptp_sync_notify_meta* meta) {
  struct st_app_context* ctx = priv;
  if (!ctx->ptp_systime_sync) return;

  /* sync raw ptp to sys time */
  uint64_t to_ns = mtl_ptp_read_time_raw(ctx->st);
  int ret;
  struct timespec from_ts, to_ts;
  st_get_tai_time(&from_ts);
  from_ts.tv_sec += meta->master_utc_offset; /* utc offset */
  uint64_t from_ns = st_timespec_to_ns(&from_ts);

  /* record the sync delta */
  int64_t delta = to_ns - from_ns;
  ctx->ptp_sync_cnt++;
  ctx->ptp_sync_delta_sum += delta;
  if (delta > ctx->ptp_sync_delta_max) ctx->ptp_sync_delta_max = delta;
  if (delta < ctx->ptp_sync_delta_min) ctx->ptp_sync_delta_min = delta;

  /* sample just offset the system time delta, better to calibrate as phc2sys way which
   * adjust the time frequency also  */
  st_ns_to_timespec(to_ns, &to_ts);
  to_ts.tv_sec -= meta->master_utc_offset; /* utc offset */
  ret = st_set_tai_time(&to_ts);
  if (ret < 0) {
    err("%s, set real time to %" PRIu64 " fail, delta %" PRId64 "\n", __func__, to_ns,
        delta);
    if (ret == -EPERM)
      err("%s, please add capability to the app: sudo setcap 'cap_sys_time+ep' <app>\n",
          __func__);
  }

  dbg("%s, from_ns %" PRIu64 " to_ns %" PRIu64 " delta %" PRId64 " done\n", __func__,
      from_ns, to_ns, delta);
  return;
}

void app_set_log_level(enum mtl_log_level level) {
  app_log_level = level;
}

enum mtl_log_level app_get_log_level(void) {
  return app_log_level;
}

static uint64_t app_ptp_from_tai_time(void* priv) {
  struct st_app_context* ctx = priv;
  struct timespec spec;
  st_get_tai_time(&spec);
  spec.tv_sec -= ctx->utc_offset;
  return ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
}

static void user_param_init(struct st_app_context* ctx, struct mtl_init_params* p) {
  memset(p, 0x0, sizeof(*p));

  p->pmd[MTL_PORT_P] = MTL_PMD_DPDK_USER;
  p->pmd[MTL_PORT_R] = MTL_PMD_DPDK_USER;
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
  snprintf(ctx->tx_video_url, sizeof(ctx->tx_video_url), "%s", "test.yuv");
  ctx->tx_video_session_cnt = 0;
  snprintf(ctx->tx_audio_url, sizeof(ctx->tx_audio_url), "%s", "test.pcm");
  ctx->tx_audio_session_cnt = 0;
  snprintf(ctx->tx_anc_url, sizeof(ctx->tx_anc_url), "%s", "test.txt");
  ctx->tx_anc_session_cnt = 0;
  snprintf(ctx->tx_fmd_url, sizeof(ctx->tx_fmd_url), "%s", "test.txt");
  ctx->tx_fmd_session_cnt = 0;
  snprintf(ctx->tx_st22_url, sizeof(ctx->tx_st22_url), "%s", "test.raw");
  ctx->tx_st22_session_cnt = 0;
  snprintf(ctx->tx_st22p_url, sizeof(ctx->tx_st22p_url), "%s", "test_rfc4175.yuv");
  ctx->tx_st22p_session_cnt = 0;
  snprintf(ctx->tx_st20p_url, sizeof(ctx->tx_st20p_url), "%s", "test_rfc4175.yuv");
  ctx->tx_st20p_session_cnt = 0;
  snprintf(ctx->tx_st40p_url, sizeof(ctx->tx_st40p_url), "%s", "test.txt");
  ctx->tx_st40p_session_cnt = 0;

  /* rx */
  ctx->rx_video_session_cnt = 0;
  ctx->rx_audio_session_cnt = 0;
  ctx->rx_anc_session_cnt = 0;
  ctx->rx_fmd_session_cnt = 0;
  ctx->rx_st22_session_cnt = 0;
  ctx->rx_st22p_session_cnt = 0;
  ctx->rx_st20p_session_cnt = 0;
  ctx->rx_st20r_session_cnt = 0;
  ctx->rx_st40p_session_cnt = 0;

  /* st22 */
  ctx->st22_bpp = 3; /* 3bit per pixel */

  ctx->utc_offset = 0;

  ctx->ptp_sync_delta_min = INT64_MAX;
  ctx->ptp_sync_delta_max = INT64_MIN;

  /* init lcores and sch */
  for (int i = 0; i < ST_APP_MAX_LCORES; i++) {
    ctx->lcore[i] = -1;
    ctx->rtp_lcore[i] = -1;
  }

  ctx->force_tx_video_numa = -1;
  ctx->force_rx_video_numa = -1;
  ctx->force_tx_audio_numa = -1;
  ctx->force_rx_audio_numa = -1;

  ctx->last_stat_time_ns = st_app_get_monotonic_time();
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

static int st_mtl_log_file_free(struct st_app_context* ctx) {
  if (ctx->mtl_log_stream) {
    fclose(ctx->mtl_log_stream);
    ctx->mtl_log_stream = NULL;
  }

  return 0;
}

static void st_app_ctx_free(struct st_app_context* ctx) {
  st_app_tx_video_sessions_uinit(ctx);
  st_app_tx_audio_sessions_uinit(ctx);
  st_app_tx_anc_sessions_uinit(ctx);
  st_app_tx_fmd_sessions_uinit(ctx);
  st_app_tx_st22p_sessions_uinit(ctx);
  st_app_tx_st20p_sessions_uinit(ctx);
  st_app_tx_st30p_sessions_uinit(ctx);
  st_app_tx_st40p_sessions_uinit(ctx);
  st22_app_tx_sessions_uinit(ctx);

  st_app_rx_video_sessions_uinit(ctx);
  st_app_rx_audio_sessions_uinit(ctx);
  st_app_rx_anc_sessions_uinit(ctx);
  st_app_rx_fmd_sessions_uinit(ctx);
  st_app_rx_st22p_sessions_uinit(ctx);
  st_app_rx_st20p_sessions_uinit(ctx);
  st_app_rx_st30p_sessions_uinit(ctx);
  st_app_rx_st40p_sessions_uinit(ctx);
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
  st_mtl_log_file_free(ctx);
  st_app_free(ctx);
}

static int st_app_result(struct st_app_context* ctx) {
  int result = 0;

  result += st_app_tx_video_sessions_result(ctx);
  result += st_app_rx_video_sessions_result(ctx);
  result += st_app_rx_audio_sessions_result(ctx);
  result += st_app_rx_anc_sessions_result(ctx);
  result += st_app_rx_fmd_sessions_result(ctx);
  result += st_app_rx_st22p_sessions_result(ctx);
  result += st_app_rx_st20p_sessions_result(ctx);
  result += st_app_rx_st30p_sessions_result(ctx);
  result += st_app_rx_st40p_sessions_result(ctx);
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
      ctx->tx_st40p_session_cnt > ST_APP_MAX_TX_ANC_SESSIONS ||
      ctx->tx_fmd_session_cnt > ST_APP_MAX_TX_FMD_SESSIONS ||
      ctx->rx_video_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st22_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st22p_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_st20p_session_cnt > ST_APP_MAX_RX_VIDEO_SESSIONS ||
      ctx->rx_audio_session_cnt > ST_APP_MAX_RX_AUDIO_SESSIONS ||
      ctx->rx_anc_session_cnt > ST_APP_MAX_RX_ANC_SESSIONS ||
      ctx->rx_st40p_session_cnt > ST_APP_MAX_RX_ANC_SESSIONS ||
      ctx->rx_fmd_session_cnt > ST_APP_MAX_RX_FMD_SESSIONS) {
    err("%s, session cnt invalid, pass the restriction\n", __func__);
    return -EINVAL;
  }

  int tx_st20_sessions = ctx->tx_video_session_cnt + ctx->tx_st22_session_cnt +
                         ctx->tx_st20p_session_cnt + ctx->tx_st22p_session_cnt;
  int rx_st20_sessions = ctx->rx_video_session_cnt + ctx->rx_st22_session_cnt +
                         ctx->rx_st22p_session_cnt + ctx->rx_st20p_session_cnt;
  for (int i = 0; i < ctx->para.num_ports; i++) {
    ctx->para.pmd[i] = mtl_pmd_by_port_name(ctx->para.port[i]);

    if (!ctx->para.tx_queues_cnt[i]) {
      if (ctx->json_ctx) {
        /* get from the assigned sessions on each interface */
        ctx->para.tx_queues_cnt[i] =
            st_tx_sessions_queue_cnt(ctx->json_ctx->interfaces[i].tx_video_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].tx_audio_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].tx_anc_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].tx_fmd_sessions_cnt);
      } else {
        ctx->para.tx_queues_cnt[i] =
            st_tx_sessions_queue_cnt(tx_st20_sessions, ctx->tx_audio_session_cnt,
                                     ctx->tx_anc_session_cnt, ctx->tx_fmd_session_cnt);
      }
      if (ctx->para.tx_queues_cnt[i] && (ctx->para.pmd[i] == MTL_PMD_DPDK_USER)) {
        ctx->para.tx_queues_cnt[i] += 4; /* add extra 4 queues for recovery */
      }
    }
    if (!ctx->para.rx_queues_cnt[i]) {
      if (ctx->json_ctx) {
        /* get from the assigned sessions on each interface */
        ctx->para.rx_queues_cnt[i] =
            st_rx_sessions_queue_cnt(ctx->json_ctx->interfaces[i].rx_video_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].rx_audio_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].rx_anc_sessions_cnt,
                                     ctx->json_ctx->interfaces[i].rx_fmd_sessions_cnt);
      } else {
        ctx->para.rx_queues_cnt[i] =
            st_rx_sessions_queue_cnt(rx_st20_sessions, ctx->rx_audio_session_cnt,
                                     ctx->rx_anc_session_cnt, ctx->rx_fmd_session_cnt);
      }
    }
  }

  /* hdr split special */
  if (ctx->enable_hdr_split) {
    ctx->para.nb_rx_hdr_split_queues = ctx->rx_video_session_cnt;
  }

  if (ctx->ptp_systime_sync) ctx->para.ptp_sync_notify = app_ptp_sync_notify;

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

  if ((ctx->json_ctx && ctx->json_ctx->has_display) || ctx->tx_display ||
      ctx->rx_display) {
    ret = st_app_player_init(ctx);
    if (ret < 0) {
      err("%s, player init fail %d\n", __func__, ret);
      st_app_ctx_free(ctx);
      return ret;
    }
    ctx->has_sdl = true;
  }

  if (ctx->runtime_session) {
    ret = mtl_start(ctx->st);
    if (ret < 0) {
      err("%s, start dev fail %d\n", __func__, ret);
      st_app_ctx_free(ctx);
      return -EIO;
    }
  }

  if (ctx->json_ctx->user_time_offset) {
    ctx->user_time.user_time_offset = ctx->json_ctx->user_time_offset;
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

  ret = st_app_tx_fmd_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_fmd_sessions_init fail %d\n", __func__, ret);
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

  ret = st_app_tx_st30p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_st30p_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_tx_st40p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_tx_st40p_sessions_init fail %d\n", __func__, ret);
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

  ret = st_app_rx_fmd_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_fmd_sessions_init fail %d\n", __func__, ret);
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

  ret = st_app_rx_st30p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_st30p_sessions_init fail %d\n", __func__, ret);
    st_app_ctx_free(ctx);
    return -EIO;
  }

  ret = st_app_rx_st40p_sessions_init(ctx);
  if (ret < 0) {
    err("%s, st_app_rx_st40p_sessions_init fail %d\n", __func__, ret);
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
  mtl_thread_setname(pthread_self(), "RxTxApp_main");
  info("%s, app lunch succ, test time %ds\n", __func__, test_time_s);
  while (!ctx->stop) {
    sleep(1);
    run_time_s++;
    if (test_time_s && (run_time_s > test_time_s)) break;
    if (ctx->pcapng_max_pkts && (run_time_s == 10)) { /* trigger pcap dump if */
      st_app_pcap(ctx);
    }
    /* check for auto_stop condition */
    if (ctx->auto_stop) {
      bool tx_complete = st_app_tx_st20p_sessions_all_complete(ctx);
      bool rx_timeout = st_app_rx_st20p_sessions_all_timeout(ctx);
      if (tx_complete && rx_timeout) {
        info("%s, auto_stop triggered: tx complete and rx timeout\n", __func__);
        break;
      }
    }
  }

  if (!ctx->runtime_session) {
    /* stop st first */
    if (ctx->st) mtl_stop(ctx->st);
  }

  ret = st_app_result(ctx);

  /* free */
  st_app_ctx_free(ctx);

  return ret;
}

/**
 * Returns the pacing time based on the user_pacing structure.
 * If user_pacing is NULL, disabled, or an error occurs, returns 0.
 * Otherwise, returns the calculated time.
 */
uint64_t st_app_user_time(void* ctx, struct st_user_time* user_time, uint64_t frame_num,
                          double frame_time, bool restart_base_time) {
  uint64_t tai_time, offset;

  if (!user_time) return 0;

  if (restart_base_time) {
    pthread_mutex_lock(&user_time->base_tai_time_mutex);

    user_time->base_tai_time = app_ptp_from_tai_time(ctx);
    /* align to N*frame_time, "epochs" */
    user_time->base_tai_time +=
        (uint64_t)(frame_time - fmod((double)user_time->base_tai_time, frame_time));

    info("%s, restart base tai time %lu\n", __func__, user_time->base_tai_time);
    if (user_time->base_tai_time == 0) {
      err("%s, get tai time fail\n", __func__);
      pthread_mutex_unlock(&user_time->base_tai_time_mutex);
      return 0;
    }
    pthread_mutex_unlock(&user_time->base_tai_time_mutex);
  }
  offset = user_time->user_time_offset;
  tai_time = user_time->base_tai_time + offset + (uint64_t)(frame_time * frame_num);

  return tai_time;
}

int st_set_mtl_log_file(struct st_app_context* ctx, const char* file) {
  FILE* f = fopen(file, "w");
  if (!f) {
    err("%s, fail(%s) to open %s\n", __func__, strerror(errno), file);
    return -EIO;
  }

  /* close any log file */
  st_mtl_log_file_free(ctx);

  int ret = mtl_openlog_stream(f);
  if (ret < 0) {
    err("%s, set mtl log stream fail %d\n", __func__, ret);
    return -EIO;
  }

  ctx->mtl_log_stream = f;
  info("%s, succ to %s\n", __func__, file);
  return 0;
}

void st_sha_dump(const char* tag, const unsigned char* sha) {
  if (tag) info("%s, ", tag);
  for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    info("0x%02x ", sha[i]);
  }
  info("\n");
}
