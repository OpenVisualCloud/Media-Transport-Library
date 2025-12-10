/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct rx_st20p_sample_ctx {
  int idx;
  st20p_rx_handle handle;

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

  mtl_dma_mem_handle dma_mem;
  struct st20_ext_frame* ext_frames;
  int ext_idx;
  int fb_cnt;
};

static int rx_st20p_frame_available(void* priv) {
  struct rx_st20p_sample_ctx* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st20p_query_ext_frame(void* priv, struct st_ext_frame* ext_frame,
                                    struct st20_rx_frame_meta* meta) {
  struct rx_st20p_sample_ctx* s = priv;
  int i = s->ext_idx;
  MTL_MAY_UNUSED(meta);
  /* you can check the timestamp from lib by meta->timestamp */

  ext_frame->addr[0] = s->ext_frames[i].buf_addr;
  ext_frame->iova[0] = s->ext_frames[i].buf_iova;
  ext_frame->size = s->ext_frames[i].buf_len;

  uint8_t* addr = ext_frame->addr[0];
  enum st_frame_fmt frame_fmt = st_frame_fmt_from_transport(meta->fmt);
  uint8_t planes = st_frame_fmt_planes(frame_fmt);
  for (int plane = 0; plane < planes; plane++) {
    if (plane > 0)
      ext_frame->iova[plane] =
          ext_frame->iova[plane - 1] + ext_frame->linesize[plane - 1] * meta->height;
    ext_frame->linesize[plane] = st_frame_least_linesize(frame_fmt, meta->width, plane);
    ext_frame->addr[plane] = addr;
    addr += ext_frame->linesize[plane] * meta->height;
  }

  /* save your private data here get it from st_frame.opaque */
  /* ext_frame->opaque = ?; */

  if (++s->ext_idx >= s->fb_cnt) s->ext_idx = 0;

  return 0;
}

static int rx_st20p_close_source(struct rx_st20p_sample_ctx* s) {
  if (s->dst_begin) {
    munmap(s->dst_begin, s->dst_end - s->dst_begin);
    s->dst_begin = NULL;
  }
  if (s->dst_fd >= 0) {
    close(s->dst_fd);
    s->dst_fd = 0;
  }

  return 0;
}

static int rx_st20p_open_source(struct rx_st20p_sample_ctx* s, const char* file) {
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
  info("%s(%d), save %d framebuffers to file %s(%p,%" PRIu64 ")\n", __func__, idx, fb_cnt,
       file, m, f_size);

  return 0;
}

static void rx_st20p_consume_frame(struct rx_st20p_sample_ctx* s,
                                   struct st_frame* frame) {
  if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;
  mtl_memcpy(s->dst_cursor, frame->addr[0], s->frame_size);
  s->dst_cursor += s->frame_size;
  /* parse private data for dynamic ext frame
    if (frame->opaque) {
      do_something(frame->opaque);
    }
  */
  s->fb_recv++;
}

static void* rx_st20p_frame_thread(void* arg) {
  struct rx_st20p_sample_ctx* s = arg;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    rx_st20p_consume_frame(s, frame);
    st20p_rx_put_frame(handle, frame);
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

  bool is_output_yuv420 = ctx.output_fmt == ST_FRAME_FMT_YUV420CUSTOM8 ||
                          ctx.output_fmt == ST_FRAME_FMT_YUV420PLANAR8;
  if (ctx.ext_frame && is_output_yuv420) {
    warn(
        "%s: external frame mode does not support yuv420 output format, use other format "
        "e.g. yuv422\n",
        __func__);
  }

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_st20p_sample_ctx* app[session_num];
  bool equal = st_frame_fmt_equal_transport(ctx.output_fmt, ctx.fmt);

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_st20p_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_st20p_sample_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->dst_fd = -1;
    app[i]->fb_cnt = ctx.framebuff_cnt;

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = 1;
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.transport_fmt = ctx.fmt;
    ops_rx.output_fmt = ctx.output_fmt;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.framebuff_cnt = app[i]->fb_cnt;
    ops_rx.notify_frame_available = rx_st20p_frame_available;

    if (equal || ctx.ext_frame) {
      /* pre-allocate ext frames */
      app[i]->ext_frames =
          (struct st20_ext_frame*)malloc(sizeof(*app[i]->ext_frames) * app[i]->fb_cnt);
      size_t framebuff_size = st_frame_size(ops_rx.output_fmt, ops_rx.width,
                                            ops_rx.height, ops_rx.interlaced);

      size_t fb_size = framebuff_size * app[i]->fb_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(ctx.st, fb_size);
      if (!dma_mem) {
        err("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      app[i]->dma_mem = dma_mem;

      for (int j = 0; j < app[i]->fb_cnt; ++j) {
        app[i]->ext_frames[j].buf_addr = mtl_dma_mem_addr(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_iova = mtl_dma_mem_iova(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_len = framebuff_size;
      }
      app[i]->ext_idx = 0;
      /* ops_rx.ext_frames = (convert to st_ext_frame)app[i]->p_ext_frames; */
      /* use dynamic external frames */
      ops_rx.query_ext_frame = rx_st20p_query_ext_frame;
      ops_rx.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      ops_rx.flags |= ST20P_RX_FLAG_EXT_FRAME;
    }

    st20p_rx_handle rx_handle = st20p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%d), st20p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

    app[i]->frame_size = st20p_rx_frame_size(rx_handle);
    ret = rx_st20p_open_source(app[i], ctx.rx_url);
    if (ret < 0) {
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st20p_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

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

    rx_st20p_close_source(app[i]);
  }

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
      if (app[i]->handle) st20p_rx_free(app[i]->handle);
      if (ctx.st && app[i]->dma_mem) mtl_dma_mem_free(ctx.st, app[i]->dma_mem);
      if (app[i]->ext_frames) free(app[i]->ext_frames);
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
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
