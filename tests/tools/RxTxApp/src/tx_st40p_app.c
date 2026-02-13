/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "tx_st40p_app.h"

#include <mtl/st40_pipeline_api.h>

/* Maximum UDW payload per frame */
#define ST40P_APP_MAX_UDW_SIZE (255)

static void app_tx_st40p_build_frame(struct st_app_tx_st40p_session* s,
                                     struct st40_frame_info* frame) {
  uint8_t* udw = frame->udw_buff_addr;
  size_t avail = frame->udw_buffer_size;
  size_t copy_sz = ST40P_APP_MAX_UDW_SIZE;

  if (copy_sz > avail) copy_sz = avail;

  if (s->st40p_source_begin) {
    /* file source */
    size_t remain = s->st40p_source_end - s->st40p_frame_cursor;
    if (copy_sz > remain) copy_sz = remain;
    mtl_memcpy(udw, s->st40p_frame_cursor, copy_sz);
    s->st40p_frame_cursor += copy_sz;
    if (s->st40p_frame_cursor >= s->st40p_source_end) {
      s->st40p_frame_cursor = s->st40p_source_begin;
      s->st40p_frames_copied = true;
    }
  } else {
    /* synthetic: fill with incrementing pattern */
    for (size_t i = 0; i < copy_sz; i++)
      udw[i] = (uint8_t)((s->frame_num + i) & 0xFF);
  }

  /* Fill one ancillary meta entry (closed-caption-like) */
  frame->meta[0].c = 0;
  frame->meta[0].line_number = 10;
  frame->meta[0].hori_offset = 0;
  frame->meta[0].s = 0;
  frame->meta[0].stream_num = 0;
  frame->meta[0].did = 0x43;  /* CEA-708 */
  frame->meta[0].sdid = 0x02; /* closed caption */
  frame->meta[0].udw_size = copy_sz;
  frame->meta[0].udw_offset = 0;
  frame->meta_num = 1;
  frame->udw_buffer_fill = copy_sz;
}

static void* app_tx_st40p_frame_thread(void* arg) {
  struct st_app_tx_st40p_session* s = arg;
  st40p_tx_handle handle = s->handle;
  int idx = s->idx;
  struct st40_frame_info* frame;
  double frame_time;

  frame_time = s->expect_fps ? (NS_PER_S / s->expect_fps) : 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40p_app_thread_stop) {
    frame = st40p_tx_get_frame(handle);
    if (!frame) { /* no ready frame */
      warn("%s(%d), get frame time out\n", __func__, idx);
      continue;
    }
    app_tx_st40p_build_frame(s, frame);

    if (s->user_time) {
      bool restart_base_time = !s->local_tai_base_time;

      frame->timestamp = st_app_user_time(s->ctx, s->user_time, s->frame_num, frame_time,
                                           restart_base_time);
      frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
      s->local_tai_base_time = s->user_time->base_tai_time;
    }

    s->frame_num++;
    st40p_tx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_st40p_open_source(struct st_app_tx_st40p_session* s) {
  int fd;
  struct stat i;

  if (s->st40p_source_url[0] == '\0') {
    info("%s, no source url, use synthetic data\n", __func__);
    return 0;
  }

  fd = st_open(s->st40p_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open fail '%s'\n", __func__, s->st40p_source_url);
    return -EIO;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, s->st40p_source_url);
    close(fd);
    return -EIO;
  }
  if (i.st_size == 0) {
    err("%s, %s file size is zero\n", __func__, s->st40p_source_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap fail '%s'\n", __func__, s->st40p_source_url);
    close(fd);
    return -EIO;
  }

  s->st40p_source_begin = mtl_hp_malloc(s->st, i.st_size, MTL_PORT_P);
  if (!s->st40p_source_begin) {
    warn("%s, source malloc on hugepage fail\n", __func__);
    s->st40p_source_begin = m;
    s->st40p_frame_cursor = m;
    s->st40p_source_end = m + i.st_size;
    s->st40p_source_fd = fd;
  } else {
    s->st40p_frame_cursor = s->st40p_source_begin;
    mtl_memcpy(s->st40p_source_begin, m, i.st_size);
    s->st40p_source_end = s->st40p_source_begin + i.st_size;
    munmap(m, i.st_size);
    close(fd);
  }

  return 0;
}

static int app_tx_st40p_start_source(struct st_app_tx_st40p_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  ret = pthread_create(&s->st40p_app_thread, NULL, app_tx_st40p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail ret %d\n", __func__, idx, ret);
    return ret;
  }
  s->st40p_app_thread_stop = false;

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_st40p_%d", idx);
  mtl_thread_setname(s->st40p_app_thread, thread_name);

  return 0;
}

static void app_tx_st40p_stop_source(struct st_app_tx_st40p_session* s) {
  s->st40p_app_thread_stop = true;
  if (s->st40p_app_thread) {
    info("%s(%d), wait app thread stop\n", __func__, s->idx);
    if (s->handle) st40p_tx_wake_block(s->handle);
    pthread_join(s->st40p_app_thread, NULL);
    s->st40p_app_thread = 0;
  }
}

static int app_tx_st40p_close_source(struct st_app_tx_st40p_session* s) {
  if (s->st40p_source_fd < 0 && s->st40p_source_begin) {
    mtl_hp_free(s->st, s->st40p_source_begin);
    s->st40p_source_begin = NULL;
  }
  if (s->st40p_source_fd >= 0) {
    munmap(s->st40p_source_begin, s->st40p_source_end - s->st40p_source_begin);
    close(s->st40p_source_fd);
    s->st40p_source_fd = -1;
  }

  return 0;
}

static int app_tx_st40p_handle_free(struct st_app_tx_st40p_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st40p_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st40p_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_st40p_uinit(struct st_app_tx_st40p_session* s) {
  app_tx_st40p_stop_source(s);
  app_tx_st40p_handle_free(s);
  app_tx_st40p_close_source(s);
  return 0;
}

static int app_tx_st40p_init(struct st_app_context* ctx,
                             st_json_st40p_session_t* st40p,
                             struct st_app_tx_st40p_session* s) {
  int idx = s->idx, ret;
  struct st40p_tx_ops ops;
  char name[32];
  st40p_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->last_stat_time_ns = st_app_get_monotonic_time();

  snprintf(name, 32, "app_tx_st40p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st40p ? st40p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P],
         st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st40p ? st40p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] =
      st40p ? st40p->base.udp_port : (10100 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST40P_TX_FLAG_USER_P_MAC;
  }
  if (ops.port.num_port > 1) {
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_R],
           st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st40p ? st40p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st40p ? st40p->base.udp_port : (10100 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST40P_TX_FLAG_USER_R_MAC;
    }
  }
  ops.port.payload_type =
      st40p ? st40p->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  ops.fps = st40p ? st40p->info.anc_fps : ST_FPS_P59_94;
  ops.interlaced = st40p ? st40p->info.interlaced : false;
  ops.max_udw_buff_size = ST40P_APP_MAX_UDW_SIZE;
  ops.framebuff_cnt = 3;

  s->expect_fps = st_frame_rate(ops.fps);

  if (st40p && st40p->user_pacing) {
    ops.flags |= ST40P_TX_FLAG_USER_PACING;
    s->user_time = &ctx->user_time;
    s->frame_num = 0;
    s->local_tai_base_time = 0;
  }
  if (st40p && st40p->exact_user_pacing) {
    ops.flags |= ST40P_TX_FLAG_EXACT_USER_PACING;
  }
  if (st40p && st40p->user_timestamp) {
    ops.flags |= ST40P_TX_FLAG_USER_TIMESTAMP;
  }
  if (st40p && st40p->enable_rtcp) {
    ops.flags |= ST40P_TX_FLAG_ENABLE_RTCP;
  }

  /* Wire test-mode mutation (mirrors GStreamer tx-test-mode property) */
  if (st40p && st40p->test_mode != ST40_TX_TEST_NONE) {
    ops.test.pattern = (enum st40_tx_test_pattern)st40p->test_mode;
    ops.test.frame_count = st40p->test_frame_count; /* 0 → lib default (8 for redundant) */
    ops.test.paced_pkt_count = st40p->test_pkt_count;
    /* Enable split-ANC-by-pkt when any test pattern is active (same as GStreamer) */
    ops.flags |= ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
  }
  /* Wire redundant path delay for path-asymmetry / dejitter testing */
  if (st40p && st40p->redundant_delay_ns) {
    ops.test.redundant_delay_ns = st40p->redundant_delay_ns;
  }

  ops.flags |= ST40P_TX_FLAG_BLOCK_GET;
  s->num_port = ops.port.num_port;
  memcpy(s->st40p_source_url, st40p ? st40p->info.anc_url : ctx->tx_st40p_url,
         ST_APP_URL_MAX_LEN);
  s->st = ctx->st;

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st40p_source_fd = -1;

  if (ctx->tx_anc_dedicate_queue) ops.flags |= ST40P_TX_FLAG_DEDICATE_QUEUE;

  handle = st40p_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40p_tx_create fail\n", __func__, idx);
    app_tx_st40p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  ret = app_tx_st40p_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st40p_open_source fail %d\n", __func__, idx, ret);
    app_tx_st40p_uinit(s);
    return ret;
  }
  ret = app_tx_st40p_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st40p_start_source fail %d\n", __func__, idx, ret);
    app_tx_st40p_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_st40p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_st40p_session* s;
  ctx->tx_st40p_sessions = (struct st_app_tx_st40p_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_st40p_session) * ctx->tx_st40p_session_cnt);
  if (!ctx->tx_st40p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_st40p_session_cnt; i++) {
    s = &ctx->tx_st40p_sessions[i];
    s->idx = i;
    ret = app_tx_st40p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_st40p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_st40p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_st40p_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_st40p_session* s;
  if (!ctx->tx_st40p_sessions) return 0;
  for (int i = 0; i < ctx->tx_st40p_session_cnt; i++) {
    s = &ctx->tx_st40p_sessions[i];
    app_tx_st40p_stop_source(s);
  }

  return 0;
}

int st_app_tx_st40p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_st40p_session* s;
  if (!ctx->tx_st40p_sessions) return 0;
  for (i = 0; i < ctx->tx_st40p_session_cnt; i++) {
    s = &ctx->tx_st40p_sessions[i];
    app_tx_st40p_uinit(s);
  }
  st_app_free(ctx->tx_st40p_sessions);

  return 0;
}
