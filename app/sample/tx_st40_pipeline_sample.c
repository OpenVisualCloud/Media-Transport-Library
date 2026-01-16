/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "sample_util.h"

#define ST40P_SAMPLE_MAX_UDW_SIZE 255

struct tx_st40p_sample_ctx {
  mtl_handle st;
  int idx;
  st40p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  int fb_send_done;

  size_t udw_payload_limit;
  uint8_t* source_begin;
  uint8_t* source_end;
  uint8_t* frame_cursor;
};

static void tx_st40p_close_source(struct tx_st40p_sample_ctx* s) {
  if (!s) return;

  if (s->source_begin) {
    mtl_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
    s->source_end = NULL;
    s->frame_cursor = NULL;
  }
}

static int tx_st40p_open_source(struct tx_st40p_sample_ctx* s, const char* file) {
  int fd = -1;
  struct stat stat_info;
  uint8_t* mapped = NULL;

  if (!file || !strlen(file)) {
    info("%s(%d), no tx_url provided, will generate synthetic ANC data\n", __func__,
         s->idx);
    return 0;
  }

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

  s->source_begin = mtl_hp_zmalloc(s->st, stat_info.st_size, MTL_PORT_P);
  if (!s->source_begin) {
    err("%s(%d), source malloc on hugepage fail\n", __func__, s->idx);
    munmap(mapped, stat_info.st_size);
    close(fd);
    return -ENOMEM;
  }

  mtl_memcpy(s->source_begin, mapped, stat_info.st_size);
  s->source_end = s->source_begin + stat_info.st_size;
  s->frame_cursor = s->source_begin;

  munmap(mapped, stat_info.st_size);
  close(fd);

  info("%s(%d), loaded %s (%" PRIu64 " bytes) into hugepage buffer\n", __func__, s->idx,
       file, (uint64_t)stat_info.st_size);

  return 0;
}

static int tx_st40p_frame_done(void* priv, struct st40_frame_info* frame_info) {
  struct tx_st40p_sample_ctx* s = priv;
  MTL_MAY_UNUSED(frame_info);

  s->fb_send_done++;
  dbg("%s(%d), done %d\n", __func__, s->idx, s->fb_send_done);
  return 0;
}

static void tx_st40p_fill_meta(struct tx_st40p_sample_ctx* s,
                               struct st40_frame_info* frame_info, uint32_t udw_size) {
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

static void tx_st40p_fill_payload(struct tx_st40p_sample_ctx* s,
                                  struct st40_frame_info* frame_info) {
  uint32_t chunk = frame_info->udw_buffer_size;
  if (s->udw_payload_limit && chunk > s->udw_payload_limit) chunk = s->udw_payload_limit;

  if (s->source_begin) {
    size_t remaining = s->source_end - s->frame_cursor;
    if (!remaining) {
      s->frame_cursor = s->source_begin;
      remaining = s->source_end - s->frame_cursor;
    }
    if (remaining < chunk) chunk = remaining;
    mtl_memcpy(frame_info->udw_buff_addr, s->frame_cursor, chunk);
    s->frame_cursor += chunk;
    if (s->frame_cursor >= s->source_end) s->frame_cursor = s->source_begin;
  } else {
    for (uint32_t i = 0; i < chunk; i++) {
      frame_info->udw_buff_addr[i] = (uint8_t)((s->fb_send + i) & 0xff);
    }
  }

  tx_st40p_fill_meta(s, frame_info, chunk);
}

static void* tx_st40p_frame_thread(void* arg) {
  struct tx_st40p_sample_ctx* s = arg;
  st40p_tx_handle handle = s->handle;
  struct st40_frame_info* frame_info;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) {
      warn("%s(%d), get frame timeout\n", __func__, s->idx);
      continue;
    }

    tx_st40p_fill_payload(s, frame_info);

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

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret = 0;

  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct tx_st40p_sample_ctx* app[session_num];
  memset(app, 0, sizeof(app));

  for (uint32_t i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(*app[i]));
    if (!app[i]) {
      err("%s(%u), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(*app[i]));
    app[i]->st = ctx.st;
    app[i]->idx = i;

    struct st40p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40p_tx_sample";
    ops_tx.priv = app[i];
    ops_tx.port.num_port = ctx.param.num_ports;

    memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;

    if (ops_tx.port.num_port > 1) {
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_R], ctx.tx_dip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      ops_tx.port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }

    if (ctx.multi_inc_addr) {
      ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
      ops_tx.port.dip_addr[MTL_SESSION_PORT_P][3] += i;
      if (ops_tx.port.num_port > 1) ops_tx.port.dip_addr[MTL_SESSION_PORT_R][3] += i;
    }

    ops_tx.port.payload_type = ctx.payload_type;
    ops_tx.fps = ctx.fps;
    ops_tx.interlaced = ctx.interlaced;
    ops_tx.framebuff_cnt = ctx.framebuff_cnt;
    ops_tx.max_udw_buff_size = ST40P_SAMPLE_MAX_UDW_SIZE;
    ops_tx.flags = ST40P_TX_FLAG_BLOCK_GET;
    if (ctx.split_anc_by_pkt) ops_tx.flags |= ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
    ops_tx.notify_frame_done = tx_st40p_frame_done;

    if (ctx.has_tx_dst_mac[MTL_PORT_P]) {
      memcpy(ops_tx.tx_dst_mac[MTL_SESSION_PORT_P], ctx.tx_dst_mac[MTL_PORT_P],
             MTL_MAC_ADDR_LEN);
      ops_tx.flags |= ST40P_TX_FLAG_USER_P_MAC;
    }
    if (ctx.has_tx_dst_mac[MTL_PORT_R] && ops_tx.port.num_port > 1) {
      memcpy(ops_tx.tx_dst_mac[MTL_SESSION_PORT_R], ctx.tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops_tx.flags |= ST40P_TX_FLAG_USER_R_MAC;
    }

    st40p_tx_handle tx_handle = st40p_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s(%u), st40p_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle;
    app[i]->udw_payload_limit = st40p_tx_max_udw_buff_size(tx_handle);

    ret = tx_st40p_open_source(app[i], ctx.tx_url);
    if (ret < 0) {
      err("%s(%u), open source fail\n", __func__, i);
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st40p_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%u), thread create fail %d\n", __func__, i, ret);
      ret = -EIO;
      goto error;
    }
  }

  while (!ctx.exit) {
    sleep(1);
  }

  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    app[i]->stop = true;
    if (app[i]->handle) st40p_tx_wake_block(app[i]->handle);
    pthread_join(app[i]->frame_thread, NULL);
    app[i]->frame_thread = 0;
    info("%s(%u), sent frames %d(done %d)\n", __func__, i, app[i]->fb_send,
         app[i]->fb_send_done);
    tx_st40p_close_source(app[i]);
  }

  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->fb_send <= 0) {
      err("%s(%u), error, no sent frames\n", __func__, i);
      ret = -EIO;
    }
  }

error:
  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->frame_thread) {
      app[i]->stop = true;
      if (app[i]->handle) st40p_tx_wake_block(app[i]->handle);
      pthread_join(app[i]->frame_thread, NULL);
      app[i]->frame_thread = 0;
    }
    if (app[i]->handle) {
      st40p_tx_free(app[i]->handle);
      app[i]->handle = NULL;
    }
    tx_st40p_close_source(app[i]);
    free(app[i]);
    app[i] = NULL;
  }

  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }

  return ret;
}
