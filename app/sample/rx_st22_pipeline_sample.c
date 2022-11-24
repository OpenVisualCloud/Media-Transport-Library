/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct rx_st22p_sample_ctx {
  int idx;
  st22p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  int dst_fd;
  uint8_t* dst_begin;
  uint8_t* dst_end;
  uint8_t* dst_cursor;
};

static int rx_st22p_frame_available(void* priv) {
  struct rx_st22p_sample_ctx* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st22p_close_source(struct rx_st22p_sample_ctx* s) {
  if (s->dst_fd >= 0) {
    close(s->dst_fd);
    s->dst_fd = 0;
  }

  return 0;
}

static int rx_st22p_open_source(struct rx_st22p_sample_ctx* s, const char* file) {
  int fd, ret, idx = s->idx;
  off_t f_size;
  int fb_cnt = 3;

  fd = st_open_mode(file, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, file);
    return -EIO;
  }

  f_size = fb_cnt * s->frame_size;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  s->dst_begin = m;
  s->dst_cursor = m;
  s->dst_end = m + f_size;
  s->dst_fd = fd;
  info("%s(%d), save %d framebuffers to file %s(%p,%ld)\n", __func__, idx, fb_cnt, file,
       m, f_size);

  return 0;
}

static void rx_st22p_consume_frame(struct rx_st22p_sample_ctx* s,
                                   struct st_frame* frame) {
  if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;

  uint8_t planes = st_frame_fmt_planes(frame->fmt);
  uint8_t* dst = s->dst_cursor;
  for (uint8_t plane = 0; plane < planes; plane++) {
    size_t plane_sz = st_frame_plane_size(frame, plane);
    mtl_memcpy(dst, frame->addr[plane], plane_sz);
    dst += plane_sz;
  }
  s->dst_cursor += s->frame_size;

  s->fb_recv++;
}

static void* rx_st22p_frame_thread(void* arg) {
  struct rx_st22p_sample_ctx* s = arg;
  st22p_rx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    rx_st22p_consume_frame(s, frame);
    st22p_rx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_st22p_sample_ctx* app[session_num];

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_st22p_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_st22p_sample_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->dst_fd = -1;

    struct st22p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22p_sample";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = 1;
    memcpy(ops_rx.port.sip_addr[MTL_PORT_P], ctx.rx_sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_rx.port.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_rx.port.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.output_fmt = ctx.st22p_output_fmt;
    ops_rx.pack_type = ST22_PACK_CODESTREAM;
    ops_rx.codec = ctx.st22p_codec;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.max_codestream_size = 0; /* let lib to decide */
    ops_rx.framebuff_cnt = ctx.framebuff_cnt;
    ops_rx.codec_thread_cnt = 2;
    ops_rx.notify_frame_available = rx_st22p_frame_available;

    st22p_rx_handle rx_handle = st22p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%d), st22p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

    app[i]->frame_size = st22p_rx_frame_size(rx_handle);
    ret = rx_st22p_open_source(app[i], ctx.rx_url);
    if (ret < 0) {
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st22p_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start rx
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_recv);

    rx_st22p_close_source(app[i]);
  }

  // stop rx
  ret = mtl_stop(ctx.st);

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_recv <= 0) {
      err("%s(%d), error, no received frames %d\n", __func__, i, app[i]->fb_recv);
      ret = -EIO;
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->handle) st22p_rx_free(app[i]->handle);
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
