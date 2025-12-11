/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "tx_st40p_app.h"

#include <inttypes.h>
#include <mtl/st40_pipeline_api.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "log.h"

#define ST40P_APP_MAX_UDW_SIZE 255

static void app_tx_st40p_fill_meta(struct st_app_tx_st40p_session* s,
                                   struct st40_frame_info* frame_info,
                                   uint32_t udw_size) {
  struct st40_meta* meta = frame_info->meta;

  meta[0].c = 0;
  meta[0].line_number = 10 + (s->fb_send % 100);
  meta[0].hori_offset = 0;
  meta[0].s = 0;
  meta[0].stream_num = 0;
  meta[0].did = 0x43;
  meta[0].sdid = 0x02;
  meta[0].udw_size = udw_size;
  meta[0].udw_offset = 0;
  frame_info->meta_num = 1;
  frame_info->udw_buffer_fill = udw_size;
}

static void app_tx_st40p_fill_payload(struct st_app_tx_st40p_session* s,
                                      struct st40_frame_info* frame_info) {
  uint32_t chunk = frame_info->udw_buffer_size;
  if (s->udw_payload_limit && chunk > s->udw_payload_limit) chunk = s->udw_payload_limit;
  if (chunk > ST40P_APP_MAX_UDW_SIZE) chunk = ST40P_APP_MAX_UDW_SIZE;

  if (s->st40p_source_begin) {
    size_t remaining = s->st40p_source_end - s->st40p_frame_cursor;
    if (!remaining) {
      s->st40p_frame_cursor = s->st40p_source_begin;
      remaining = s->st40p_source_end - s->st40p_frame_cursor;
    }
    if (remaining < chunk) chunk = remaining;
    mtl_memcpy(frame_info->udw_buff_addr, s->st40p_frame_cursor, chunk);
    s->st40p_frame_cursor += chunk;
    if (s->st40p_frame_cursor >= s->st40p_source_end)
      s->st40p_frame_cursor = s->st40p_source_begin;
  } else {
    for (uint32_t i = 0; i < chunk; i++) {
      frame_info->udw_buff_addr[i] = (uint8_t)((s->fb_send + i) & 0xff);
    }
  }

  app_tx_st40p_fill_meta(s, frame_info, chunk);
}

static int app_tx_st40p_open_source(struct st_app_tx_st40p_session* s) {
  int fd = -1;
  struct stat stat_info;
  uint8_t* mapped = NULL;
  const char* file = s->st40p_source_url;

  if (!file[0]) return 0; /* synthetic */

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    warn("%s(%d), open %s fail, use synthetic ANC data instead\n", __func__, s->idx,
         file);
    return 0;
  }

  if (fstat(fd, &stat_info) < 0) {
    err("%s(%d), fstat %s fail\n", __func__, s->idx, file);
    close(fd);
    return -EIO;
  }

  if (!stat_info.st_size) {
    warn("%s(%d), %s is empty, use synthetic ANC data instead\n", __func__, s->idx, file);
    close(fd);
    return 0;
  }

  mapped = mmap(NULL, stat_info.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == mapped) {
    err("%s(%d), mmap %s fail\n", __func__, s->idx, file);
    close(fd);
    return -EIO;
  }

  s->st40p_source_begin = mtl_hp_zmalloc(s->st, stat_info.st_size, MTL_PORT_P);
  if (!s->st40p_source_begin) {
    err("%s(%d), source malloc on hugepage fail\n", __func__, s->idx);
    munmap(mapped, stat_info.st_size);
    close(fd);
    return -ENOMEM;
  }

  mtl_memcpy(s->st40p_source_begin, mapped, stat_info.st_size);
  s->st40p_source_end = s->st40p_source_begin + stat_info.st_size;
  s->st40p_frame_cursor = s->st40p_source_begin;

  munmap(mapped, stat_info.st_size);
  close(fd);

  info("%s(%d), loaded %s (%" PRIu64 " bytes) into hugepage buffer\n", __func__, s->idx,
       file, (uint64_t)stat_info.st_size);

  return 0;
}

static void app_tx_st40p_close_source(struct st_app_tx_st40p_session* s) {
  if (s->st40p_source_begin) {
    mtl_hp_free(s->st, s->st40p_source_begin);
    s->st40p_source_begin = NULL;
    s->st40p_source_end = NULL;
    s->st40p_frame_cursor = NULL;
  }
}

static void* app_tx_st40p_frame_thread(void* arg) {
  struct st_app_tx_st40p_session* s = arg;
  st40p_tx_handle handle = s->handle;
  struct st40_frame_info* frame_info;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->st40p_app_thread_stop) {
    frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) {
      warn("%s(%d), get frame timeout\n", __func__, s->idx);
      continue;
    }

    app_tx_st40p_fill_payload(s, frame_info);

    if (s->user_time && s->frame_time > 0) {
      bool restart_base_time = !s->local_tai_base_time;
      frame_info->timestamp = st_app_user_time(s->ctx, s->user_time, s->frame_num,
                                               s->frame_time, restart_base_time);
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
      s->frame_num++;
      s->local_tai_base_time = s->user_time->base_tai_time;
    }

    if (st40p_tx_put_frame(handle, frame_info) < 0) {
      err("%s(%d), put frame fail\n", __func__, s->idx);
      break;
    }

    s->fb_send++;
    dbg("%s(%d), fb_send %d\n", __func__, s->idx, s->fb_send);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int app_tx_st40p_start_source(struct st_app_tx_st40p_session* s) {
  int ret;

  s->st40p_app_thread_stop = false;
  ret = pthread_create(&s->st40p_app_thread, NULL, app_tx_st40p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail %d\n", __func__, s->idx, ret);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_st40p_%d", s->idx);
  mtl_thread_setname(s->st40p_app_thread, thread_name);

  return 0;
}

static void app_tx_st40p_stop_source(struct st_app_tx_st40p_session* s) {
  s->st40p_app_thread_stop = true;
  if (s->st40p_app_thread) {
    if (s->handle) st40p_tx_wake_block(s->handle);
    pthread_join(s->st40p_app_thread, NULL);
    s->st40p_app_thread = 0;
  }
}

static int app_tx_st40p_handle_free(struct st_app_tx_st40p_session* s) {
  int ret = 0;
  if (s->handle) {
    ret = st40p_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st40p_tx_free fail %d\n", __func__, s->idx, ret);
    s->handle = NULL;
  }
  return ret;
}

static int app_tx_st40p_uinit(struct st_app_tx_st40p_session* s) {
  app_tx_st40p_stop_source(s);
  app_tx_st40p_handle_free(s);
  app_tx_st40p_close_source(s);
  return 0;
}

static int app_tx_st40p_init(struct st_app_context* ctx, st_json_st40p_session_t* st40p,
                             struct st_app_tx_st40p_session* s) {
  int idx = s->idx, ret;
  struct st40p_tx_ops ops;
  char name[32];
  st40p_tx_handle handle;

  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->st = ctx->st;
  s->st40p = st40p;
  s->framebuff_cnt = 3;
  s->fb_send = 0;
  s->fb_send_done = 0;
  s->frame_num = 0;
  s->local_tai_base_time = 0;
  s->user_time = NULL;
  s->st40p_source_begin = NULL;
  s->st40p_source_end = NULL;
  s->st40p_frame_cursor = NULL;
  s->udw_payload_limit = 0;

  snprintf(name, sizeof(name), "app_tx_st40p_%d", idx);
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
      st40p ? st40p->base.udp_port : (12000 + idx * 2);

  if (ops.port.num_port > 1) {
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_R],
           st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st40p ? st40p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st40p ? st40p->base.udp_port : (12000 + idx * 2);
  }

  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST40P_TX_FLAG_USER_P_MAC;
  }
  if (ctx->has_tx_dst_mac[MTL_PORT_R] && ops.port.num_port > 1) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST40P_TX_FLAG_USER_R_MAC;
  }

  ops.port.payload_type =
      st40p ? st40p->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  ops.fps = st40p ? st40p->info.fps : ST_FPS_P59_94;
  ops.interlaced = st40p ? st40p->info.interlaced : false;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.max_udw_buff_size = ST40P_APP_MAX_UDW_SIZE;
  ops.flags |= ST40P_TX_FLAG_BLOCK_GET;

  s->expect_fps = st_frame_rate(ops.fps);
  s->frame_time = s->expect_fps ? (NS_PER_S / s->expect_fps) : 0;

  /* enable user pacing when requested */
  if ((st40p && st40p->user_pacing) || ctx->tx_ts_epoch || ctx->tx_exact_user_pacing) {
    ops.flags |= ST40P_TX_FLAG_USER_PACING;
    s->user_time = &ctx->user_time;
  }
  if ((st40p && st40p->exact_user_pacing) || ctx->tx_exact_user_pacing)
    ops.flags |= ST40P_TX_FLAG_EXACT_USER_PACING;
  if (st40p && st40p->enable_rtcp) ops.flags |= ST40P_TX_FLAG_ENABLE_RTCP;
  if (ctx->tx_anc_dedicate_queue) ops.flags |= ST40P_TX_FLAG_DEDICATE_QUEUE;

  handle = st40p_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40p_tx_create fail\n", __func__, idx);
    app_tx_st40p_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->udw_payload_limit = st40p_tx_max_udw_buff_size(handle);

  ret = app_tx_st40p_open_source(s);
  if (ret < 0) {
    err("%s(%d), open source fail %d\n", __func__, idx, ret);
    app_tx_st40p_uinit(s);
    return ret;
  }

  ret = app_tx_st40p_start_source(s);
  if (ret < 0) {
    err("%s(%d), start source fail %d\n", __func__, idx, ret);
    app_tx_st40p_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_st40p_sessions_init(struct st_app_context* ctx) {
  int ret = 0;
  struct st_app_tx_st40p_session* s;

  if (!ctx->tx_st40p_session_cnt) return 0;

  ctx->tx_st40p_sessions = (struct st_app_tx_st40p_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_st40p_session) * ctx->tx_st40p_session_cnt);
  if (!ctx->tx_st40p_sessions) return -ENOMEM;

  for (int i = 0; i < ctx->tx_st40p_session_cnt; i++) {
    s = &ctx->tx_st40p_sessions[i];
    s->idx = i;
    const char* url = ctx->tx_st40p_url;
    if (ctx->json_ctx && ctx->json_ctx->tx_st40p_sessions)
      url = ctx->json_ctx->tx_st40p_sessions[i].info.st40p_url;
    snprintf(s->st40p_source_url, sizeof(s->st40p_source_url), "%s", url);

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
  struct st_app_tx_st40p_session* s;
  if (!ctx->tx_st40p_sessions) return 0;

  for (int i = 0; i < ctx->tx_st40p_session_cnt; i++) {
    s = &ctx->tx_st40p_sessions[i];
    app_tx_st40p_uinit(s);
  }
  st_app_free(ctx->tx_st40p_sessions);
  ctx->tx_st40p_sessions = NULL;

  return 0;
}
