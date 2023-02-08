/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct tx_st20p_sample_ctx {
  mtl_handle st;
  int idx;
  st20p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  uint8_t* source_begin;
  mtl_iova_t source_begin_iova;
  uint8_t* source_end;
  uint8_t* frame_cursor;

  bool ext;
  mtl_dma_mem_handle dma_mem;
};

static int tx_st20p_close_source(struct tx_st20p_sample_ctx* s) {
  if (s->ext) {
    if (s->dma_mem) mtl_dma_mem_free(s->st, s->dma_mem);
  } else if (s->source_begin) {
    mtl_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }

  return 0;
}

static int tx_st20p_open_source(struct tx_st20p_sample_ctx* s, char* file) {
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

  fstat(fd, &i);
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
  if (s->ext) { /* ext frame enabled */
    if (frame_cnt < 2) {
      /* notice that user should prepare more buffer than fb_cnt *frame_size */
      warn("%s, only 1 frame, will duplicate to 2\n", __func__);
      fbs_size *= 2;
    }

    /* alloc enough memory to hold framebuffers and map to iova */
    mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(s->st, fbs_size);
    if (!dma_mem) {
      err("%s(%d), dma mem alloc/map fail\n", __func__, s->idx);
      close(fd);
      return -EIO;
    }
    s->dma_mem = dma_mem;

    s->source_begin = mtl_dma_mem_addr(dma_mem);
    s->source_begin_iova = mtl_dma_mem_iova(dma_mem);
    s->frame_cursor = s->source_begin;
    if (m) {
      if (frame_cnt < 2) {
        mtl_memcpy(s->source_begin, m, s->frame_size);
        mtl_memcpy(s->source_begin + s->frame_size, m, s->frame_size);
      } else {
        mtl_memcpy(s->source_begin, m, i.st_size);
      }
    }
    s->source_end = s->source_begin + fbs_size;
    info("%s, source begin at %p, end at %p\n", __func__, s->source_begin, s->source_end);
  } else {
    s->source_begin = mtl_hp_zmalloc(s->st, fbs_size, MTL_PORT_P);
    if (!s->source_begin) {
      err("%s, source malloc on hugepage fail\n", __func__);
      close(fd);
      return -EIO;
    }
    s->frame_cursor = s->source_begin;
    if (m) mtl_memcpy(s->source_begin, m, fbs_size);
    s->source_end = s->source_begin + fbs_size;
  }

  if (fd >= 0) close(fd);

  return 0;
}

static int tx_st20p_frame_available(void* priv) {
  struct tx_st20p_sample_ctx* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int tx_st20p_frame_done(void* priv, struct st_frame* frame) {
  struct tx_st20p_sample_ctx* s = priv;

  if (s->ext) {
    /* free or return the ext memory here if necessary */
    /* then clear the frame buffer */
  }

  return 0;
}

static void tx_st20p_build_frame(struct tx_st20p_sample_ctx* s, struct st_frame* frame) {
  uint8_t* src = s->frame_cursor;

  mtl_memcpy(frame->addr[0], src, s->frame_size);
}

static void* tx_st20p_frame_thread(void* arg) {
  struct tx_st20p_sample_ctx* s = arg;
  st20p_tx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    if (s->ext) {
      struct st_ext_frame ext_frame;
      ext_frame.addr[0] = s->frame_cursor;
      ext_frame.iova[0] = s->source_begin_iova + (s->frame_cursor - s->source_begin);
      ext_frame.linesize[0] = st_frame_least_linesize(frame->fmt, frame->width, 0);
      uint8_t planes = st_frame_fmt_planes(frame->fmt);
      for (uint8_t plane = 1; plane < planes; plane++) { /* assume planes continous */
        ext_frame.linesize[plane] =
            st_frame_least_linesize(frame->fmt, frame->width, plane);
        ext_frame.addr[plane] = (uint8_t*)ext_frame.addr[plane - 1] +
                                ext_frame.linesize[plane - 1] * frame->height;
        ext_frame.iova[plane] =
            ext_frame.iova[plane - 1] + ext_frame.linesize[plane - 1] * frame->height;
      }
      ext_frame.size = s->frame_size;
      ext_frame.opaque = NULL;
      st20p_tx_put_ext_frame(handle, frame, &ext_frame);
    } else {
      if (s->source_begin) tx_st20p_build_frame(s, frame);
      st20p_tx_put_frame(handle, frame);
    }
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
  struct tx_st20p_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct tx_st20p_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_st20p_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->ext = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    struct st20p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20p_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.port.num_port = ctx.param.num_ports;
    memcpy(ops_tx.port.dip_addr[MTL_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_tx.port.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    if (ops_tx.port.num_port > 1) {
      memcpy(ops_tx.port.dip_addr[MTL_PORT_R], ctx.tx_dip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      strncpy(ops_tx.port.port[MTL_PORT_R], ctx.param.port[MTL_PORT_R], MTL_PORT_MAX_LEN);
      ops_tx.port.udp_port[MTL_PORT_R] = ctx.udp_port + i;
    }
    ops_tx.port.payload_type = ctx.payload_type;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.input_fmt = ctx.input_fmt;
    ops_tx.transport_fmt = ctx.fmt;
    ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_tx.framebuff_cnt = ctx.framebuff_cnt;
    ops_tx.notify_frame_available = tx_st20p_frame_available;
    ops_tx.notify_frame_done = tx_st20p_frame_done;
    if (ctx.ext_frame) {
      ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME;
      app[i]->ext = true;
    }

    st20p_tx_handle tx_handle = st20p_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s(%d), st20p_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle;

    app[i]->frame_size = st20p_tx_frame_size(tx_handle);
    ret = tx_st20p_open_source(app[i], ctx.tx_url);
    if (ret < 0) {
      err("%s(%d), open source fail\n", __func__, i);
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st20p_frame_thread, app[i]);
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
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), sent frames %d\n", __func__, i, app[i]->fb_send);

    tx_st20p_close_source(app[i]);
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
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
      if (app[i]->handle) st20p_tx_free(app[i]->handle);
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
