/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "sample_util.h"

struct tx_st30p_sample_ctx {
  mtl_handle st;
  int idx;
  st30p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  int fb_send_done;

  size_t frame_size;
  uint8_t* source_begin;
  mtl_iova_t source_begin_iova;
  uint8_t* source_end;
  uint8_t* frame_cursor;
};

static int tx_st30p_close_source(struct tx_st30p_sample_ctx* s) {
  if (s->source_begin) {
    mtl_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }

  return 0;
}

static int tx_st30p_open_source(struct tx_st30p_sample_ctx* s, char* file) {
  int fd = -EIO;
  struct stat i;
  int frame_cnt = 2;
  uint8_t* m = NULL;
  size_t fbs_size = s->frame_size * frame_cnt;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    err("%s, open %s fail\n", __func__, file);
    goto init_fb;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }
  if (i.st_size < s->frame_size) {
    err("%s, %s file size small then a frame %" PRIu64 "\n", __func__, file,
        s->frame_size);
    close(fd);
    return -EIO;
  }
  if (i.st_size % s->frame_size) {
    err("%s, %s file size should be multiple of frame size %" PRIu64 "\n", __func__, file,
        s->frame_size);
    close(fd);
    return -EIO;
  }
  m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }
  frame_cnt = i.st_size / s->frame_size;
  fbs_size = i.st_size;

init_fb:

  s->source_begin = mtl_hp_zmalloc(s->st, fbs_size, MTL_PORT_P);
  if (!s->source_begin) {
    err("%s, source malloc on hugepage fail\n", __func__);
    if (m) munmap(m, i.st_size);
    if (fd >= 0) close(fd);
    return -EIO;
  }
  s->frame_cursor = s->source_begin;
  if (m) mtl_memcpy(s->source_begin, m, fbs_size);
  s->source_end = s->source_begin + fbs_size;

  if (m) munmap(m, i.st_size);
  if (fd >= 0) close(fd);

  return 0;
}

static int tx_st30p_frame_done(void* priv, struct st30_frame* frame) {
  struct tx_st30p_sample_ctx* s = priv;
  MTL_MAY_UNUSED(frame);

  s->fb_send_done++;
  dbg("%s(%d), done %d\n", __func__, s->idx, s->fb_send_done);
  return 0;
}

static void tx_st30p_build_frame(struct tx_st30p_sample_ctx* s,
                                 struct st30_frame* frame) {
  uint8_t* src = s->frame_cursor;

  mtl_memcpy(frame->addr, src, s->frame_size);
}

static void* tx_st30p_frame_thread(void* arg) {
  struct tx_st30p_sample_ctx* s = arg;
  st30p_tx_handle handle = s->handle;
  struct st30_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st30p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }

    if (s->source_begin) tx_st30p_build_frame(s, frame);
    st30p_tx_put_frame(handle, frame);

    /* point to next frame */
    s->frame_cursor += s->frame_size;
    if (s->frame_cursor + s->frame_size > s->source_end) {
      s->frame_cursor = s->source_begin;
    }
    s->fb_send++;
    dbg("%s(%d), fb_send %d\n", __func__, s->idx, s->fb_send);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct tx_st30p_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct tx_st30p_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_st30p_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;

    struct st30p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30p_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.port.num_port = ctx.param.num_ports;
    memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.audio_udp_port + i * 2;
    if (ops_tx.port.num_port > 1) {
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_R], ctx.tx_dip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      ops_tx.port.udp_port[MTL_SESSION_PORT_R] = ctx.audio_udp_port + i * 2;
    }
    ops_tx.port.payload_type = ctx.audio_payload_type;
    ops_tx.framebuff_cnt = ctx.framebuff_cnt;
    ops_tx.flags = ST30P_TX_FLAG_BLOCK_GET;
    ops_tx.notify_frame_done = tx_st30p_frame_done;
    ops_tx.fmt = ctx.audio_fmt;
    ops_tx.channel = ctx.audio_channel;
    ops_tx.sampling = ctx.audio_sampling;
    ops_tx.ptime = ctx.audio_ptime;

    /* count frame size */
    int pkt_per_frame = 1;
    int pkt_len =
        st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
    double pkt_time = st30_get_packet_time(ops_tx.ptime);
    /* when ptime <= 1ms, set frame time to 1ms */
    if (pkt_time < NS_PER_MS) {
      pkt_per_frame = NS_PER_MS / pkt_time;
    }
    uint32_t framebuff_size = pkt_per_frame * pkt_len;
    ops_tx.framebuff_size = framebuff_size;

    st30p_tx_handle tx_handle = st30p_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s(%d), st30p_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle;

    app[i]->frame_size = st30p_tx_frame_size(tx_handle);
    ret = tx_st30p_open_source(app[i], ctx.tx_audio_url);
    if (ret < 0) {
      err("%s(%d), open source fail\n", __func__, i);
      goto error;
    }
    info("%s(%d), frame_size %" PRId64 ", tx url %s\n", __func__, i, app[i]->frame_size,
         ctx.tx_audio_url);

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st30p_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start tx
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    if (app[i]->handle) st30p_tx_wake_block(app[i]->handle);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), sent frames %d(done %d)\n", __func__, i, app[i]->fb_send,
         app[i]->fb_send_done);

    tx_st30p_close_source(app[i]);
  }

  // stop tx
  ret = mtl_stop(ctx.st);

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_send <= 0) {
      err("%s(%d), error, no sent frames %d\n", __func__, i, app[i]->fb_send);
      ret = -EIO;
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->handle) st30p_tx_free(app[i]->handle);
      free(app[i]);
    }
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
