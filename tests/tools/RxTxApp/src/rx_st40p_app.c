/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "rx_st40p_app.h"

#include <inttypes.h>

#include "log.h"

#define ST40P_APP_RX_MAX_UDW_SIZE 2048
#define ST40P_APP_RX_RTP_RING_SIZE 2048

/* each frame is one data item, the lib already rejected malformed packets and flags a
 * lost, dropped or reordered item with seq_discont on the next one */
static bool app_rx_st40p_fmd_check(struct st_app_rx_st40p_session* s,
                                   struct st40_frame_info* frame_info) {
  const char* fail = NULL;

  if (frame_info->rtp_marker)
    fail = "marker bit";
  else if (s->st40p_fmd_dit >= 0 && frame_info->fmd_dit != (uint32_t)s->st40p_fmd_dit)
    fail = "data item type";
  else if (s->st40p_fmd_k_bit >= 0 && frame_info->fmd_k_bit != s->st40p_fmd_k_bit)
    fail = "K-bit";
  else if (s->st40p_fmd_ref.data &&
           st_app_fmd_ref_check(&s->st40p_fmd_ref, !frame_info->seq_discont,
                                frame_info->udw_buff_addr,
                                frame_info->udw_buffer_fill) < 0)
    fail = "reference file compare";
  if (fail) {
    if (!s->stat_frame_invalid)
      err("%s(%d), fast metadata %s error\n", __func__, s->idx, fail);
    s->stat_frame_invalid++;
    return false;
  }

  uint64_t now = st_app_get_monotonic_time();
  if (s->st40p_fmd_last_rx_time)
    s->st40p_fmd_max_gap_ns =
        ST_MAX(s->st40p_fmd_max_gap_ns, now - s->st40p_fmd_last_rx_time);
  s->st40p_fmd_last_rx_time = now;
  return true;
}

static void* app_rx_st40p_frame_thread(void* arg) {
  struct st_app_rx_st40p_session* s = arg;
  st40p_rx_handle handle = s->handle;
  struct st40_frame_info* frame_info;
  int idx = s->idx;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40p_app_thread_stop) {
    frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) {
      st_pthread_mutex_lock(&s->st40p_wake_mutex);
      if (!s->st40p_app_thread_stop)
        st_pthread_cond_wait(&s->st40p_wake_cond, &s->st40p_wake_mutex);
      st_pthread_mutex_unlock(&s->st40p_wake_mutex);
      continue;
    }

    if (s->st40p_fmd && !app_rx_st40p_fmd_check(s, frame_info)) {
      st40p_rx_put_frame(handle, frame_info);
      continue;
    }

    s->stat_frame_total_received++;
    if (!s->stat_frame_first_rx_time)
      s->stat_frame_first_rx_time = st_app_get_monotonic_time();
    if (frame_info->seq_discont) s->stat_frame_seq_discont++;
    /* ST 2110-41 has no marker */
    if (!s->st40p_fmd && !frame_info->rtp_marker) s->stat_frame_marker_missing++;
    s->stat_seq_lost_total += frame_info->seq_lost;

    dbg("%s(%d), frame %d, status %d, marker %d, seq_lost %u\n", __func__, idx,
        s->stat_frame_total_received, frame_info->status, frame_info->rtp_marker,
        frame_info->seq_lost);

    st40p_rx_put_frame(handle, frame_info);
  }
  info("%s(%d), stop\n", __func__, idx);
  return NULL;
}

static int app_rx_st40p_frame_available(void* priv) {
  struct st_app_rx_st40p_session* s = priv;

  st_pthread_mutex_lock(&s->st40p_wake_mutex);
  st_pthread_cond_signal(&s->st40p_wake_cond);
  st_pthread_mutex_unlock(&s->st40p_wake_mutex);
  return 0;
}

static int app_rx_st40p_uinit(struct st_app_rx_st40p_session* s) {
  int ret, idx = s->idx;

  s->st40p_app_thread_stop = true;
  if (s->st40p_app_thread) {
    if (s->handle) st40p_rx_wake_block(s->handle);
    st_pthread_mutex_lock(&s->st40p_wake_mutex);
    st_pthread_cond_signal(&s->st40p_wake_cond);
    st_pthread_mutex_unlock(&s->st40p_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st40p_app_thread, NULL);
    s->st40p_app_thread = 0;
  }
  if (s->handle) {
    ret = st40p_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st40p_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  st_pthread_mutex_destroy(&s->st40p_wake_mutex);
  st_pthread_cond_destroy(&s->st40p_wake_cond);
  st_app_fmd_ref_close(&s->st40p_fmd_ref);

  return 0;
}

static int app_rx_st40p_init(struct st_app_context* ctx, st_json_st40p_session_t* st40p,
                             struct st_app_rx_st40p_session* s) {
  int idx = s->idx, ret;
  struct st40p_rx_ops ops;
  char name[32];
  st40p_rx_handle handle;

  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->st = ctx->st;
  s->framebuff_cnt = 3;

  snprintf(name, sizeof(name), "app_rx_st40p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st40p ? st40p->base.num_inf : ctx->para.num_ports;

  memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P],
         st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_P)
               : ctx->rx_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      ops.port.mcast_sip_addr[MTL_SESSION_PORT_P],
      st40p ? st40p->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st40p ? st40p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] =
      st40p ? st40p->base.udp_port : (10200 + idx * 2);

  if (ops.port.num_port > 1) {
    memcpy(ops.port.ip_addr[MTL_SESSION_PORT_R],
           st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_R)
                 : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        ops.port.mcast_sip_addr[MTL_SESSION_PORT_R],
        st40p ? st40p->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st40p ? st40p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st40p ? st40p->base.udp_port : (10200 + idx * 2);
  }

  ops.port.payload_type =
      st40p ? st40p->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  ops.interlaced = st40p ? st40p->info.interlaced : false;
  s->expect_fps = st_frame_rate(st40p ? st40p->info.fps : ST_FPS_P59_94);
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.max_udw_buff_size = ST40P_APP_RX_MAX_UDW_SIZE;
  ops.rtp_ring_size = ST40P_APP_RX_RTP_RING_SIZE;
  ops.notify_frame_available = app_rx_st40p_frame_available;
  ops.flags = ST40P_RX_FLAG_BLOCK_GET;
  if (st40p && st40p->enable_rtcp) ops.flags |= ST40P_RX_FLAG_ENABLE_RTCP;

  st_pthread_mutex_init(&s->st40p_wake_mutex, NULL);
  st_pthread_cond_init(&s->st40p_wake_cond, NULL);
  s->st40p_fmd = st40p && st40p->info.fast_metadata;
  if (s->st40p_fmd) {
    ops.flags |= ST40P_RX_FLAG_FAST_METADATA;
    s->st40p_fmd_dit = st40p->info.fmd_dit;
    s->st40p_fmd_k_bit = st40p->info.fmd_k_bit;
    if (!st40p->info.st40p_url[0])
      info(
          "%s(%d), no st40p_url, the payload is not verified, only DIT, K-bit and "
          "rate\n",
          __func__, idx);
    else if (st_app_fmd_ref_open(&s->st40p_fmd_ref, st40p->info.st40p_url) < 0) {
      app_rx_st40p_uinit(s);
      return -EIO;
    }
  }

  handle = st40p_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40p_rx_create fail\n", __func__, idx);
    app_rx_st40p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st40p_app_thread_stop = false;
  ret = pthread_create(&s->st40p_app_thread, NULL, app_rx_st40p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail %d\n", __func__, idx, ret);
    app_rx_st40p_uinit(s);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_st40p_%d", idx);
  mtl_thread_setname(s->st40p_app_thread, thread_name);

  return 0;
}

static int app_rx_st40p_result(struct st_app_rx_st40p_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
  double framerate = time_sec > 0 ? s->stat_frame_total_received / time_sec : 0;

  if (!s->stat_frame_total_received) {
    if (s->stat_frame_invalid)
      err("%s(%d), no valid frame, %d rejected as invalid\n", __func__, idx,
          s->stat_frame_invalid);
    return -EINVAL;
  }

  bool ok = ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05);
  char fmd[64] = "";
  if (s->st40p_fmd) {
    s->st40p_fmd_max_gap_ns =
        ST_MAX(s->st40p_fmd_max_gap_ns, cur_time_ns - s->st40p_fmd_last_rx_time);
    ok = ok && !s->stat_frame_invalid && s->st40p_fmd_max_gap_ns <= ST_APP_FMD_MAX_GAP_NS;
    snprintf(fmd, sizeof(fmd), ", %d invalid, max gap %.3f ms", s->stat_frame_invalid,
             (double)s->st40p_fmd_max_gap_ns / NS_PER_MS);
  }
  notce(
      "%s(%d), %s, fps %f, %d frames received, seq_discont %d, marker_missing %d, "
      "seq_lost %u%s\n",
      __func__, idx, ok ? "OK" : "FAILED", framerate, s->stat_frame_total_received,
      s->stat_frame_seq_discont, s->stat_frame_marker_missing, s->stat_seq_lost_total,
      fmd);

  /* When the pipeline observed seq gaps, the user has no easy way to tell whether
   * the wire layer also lost the packets (true loss) or healed them via redundancy /
   * the late-arrival bitmap (cross-port reorder, harmless).  Fetch the wire-layer
   * counters and classify the cause from already-published stats. */
  if (s->stat_seq_lost_total > 0 && s->handle) {
    struct st40_rx_user_stats wire = {0};
    if (st40p_rx_get_session_stats(s->handle, &wire) == 0) {
      const char* cause;
      if (wire.common.stat_pkts_unrecovered > 0) {
        cause = "true bilateral data loss";
      } else if (wire.common.stat_pkts_wrong_pt_dropped ||
                 wire.stat_pkts_wrong_interlace_dropped ||
                 wire.common.stat_pkts_wrong_ssrc_dropped) {
        cause = "producer protocol violation (check wrong_*_dropped)";
      } else if (wire.stat_pkts_enqueue_fail) {
        cause = "RTP ring full \u2014 receiver too slow";
      } else if (wire.common.stat_lost_packets > 0) {
        cause =
            "cross-port reorder / late arrival beyond pipeline merge window (not data "
            "loss)";
      } else {
        cause =
            "unknown \u2014 pipeline saw a gap the wire layer did not, please file a bug";
      }
      notce("%s(%d), seq_lost=%u reconciliation: wire lost=%" PRIu64
            " unrecovered=%" PRIu64 " redundant=%" PRIu64 " wrong_pt=%" PRIu64
            " wrong_ssrc=%" PRIu64 " wrong_interlace=%" PRIu64 " enqueue_fail=%" PRIu64
            " \u21d2 %s\n",
            __func__, idx, s->stat_seq_lost_total, wire.common.stat_lost_packets,
            wire.common.stat_pkts_unrecovered, wire.common.stat_pkts_redundant,
            wire.common.stat_pkts_wrong_pt_dropped,
            wire.common.stat_pkts_wrong_ssrc_dropped,
            wire.stat_pkts_wrong_interlace_dropped, wire.stat_pkts_enqueue_fail, cause);
    }
  }
  return (s->st40p_fmd && !ok) ? -EIO : 0;
}

int st_app_rx_st40p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_st40p_session* s;

  if (!ctx->rx_st40p_session_cnt) return 0;

  ctx->rx_st40p_sessions = (struct st_app_rx_st40p_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_st40p_session) * ctx->rx_st40p_session_cnt);
  if (!ctx->rx_st40p_sessions) return -ENOMEM;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    s->idx = i;

    ret = app_rx_st40p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_st40p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st40p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_st40p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st40p_session* s;
  if (!ctx->rx_st40p_sessions) return 0;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    app_rx_st40p_uinit(s);
  }
  st_app_free(ctx->rx_st40p_sessions);
  ctx->rx_st40p_sessions = NULL;
  return 0;
}

int st_app_rx_st40p_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_st40p_session* s;
  if (!ctx->rx_st40p_sessions) return 0;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    ret += app_rx_st40p_result(s);
  }
  return ret;
}
