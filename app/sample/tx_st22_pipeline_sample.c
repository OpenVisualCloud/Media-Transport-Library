/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct tx_st22p_sample_ctx {
  mtl_handle st;
  int idx;
  st22p_tx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  uint8_t* source_begin;
  uint8_t* source_end;
  uint8_t* frame_cursor;

  /* logo */
  void* logo_buf;
  struct st_frame logo_meta;
};

static int tx_st22p_close_source(struct tx_st22p_sample_ctx* s) {
  if (s->source_begin) {
    mtl_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }

  if (s->logo_buf) {
    mtl_hp_free(s->st, s->logo_buf);
    s->logo_buf = NULL;
  }

  return 0;
}

static int tx_st22p_open_logo(struct st_sample_context* ctx,
                              struct tx_st22p_sample_ctx* s, char* file) {
  FILE* fp_logo = st_fopen(file, "rb");
  if (!fp_logo) {
    err("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  size_t logo_size = st_frame_size(ctx->input_fmt, ctx->logo_width, ctx->logo_height);
  s->logo_buf = mtl_hp_malloc(s->st, logo_size, MTL_PORT_P);
  if (!s->logo_buf) {
    err("%s, logo buf malloc fail\n", __func__);
    fclose(fp_logo);
    return -EIO;
  }

  size_t read = fread(s->logo_buf, 1, logo_size, fp_logo);
  if (read != logo_size) {
    err("%s, logo buf read fail\n", __func__);
    mtl_hp_free(s->st, s->logo_buf);
    s->logo_buf = NULL;
    fclose(fp_logo);
    return -EIO;
  }

  s->logo_meta.addr[0] = s->logo_buf;
  s->logo_meta.fmt = ctx->input_fmt;
  s->logo_meta.width = ctx->logo_width;
  s->logo_meta.height = ctx->logo_height;

  fclose(fp_logo);
  return 0;
}

static int tx_st22p_open_source(struct st_sample_context* ctx,
                                struct tx_st22p_sample_ctx* s, char* file) {
  int fd;
  struct stat i;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    err("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->frame_size) {
    err("%s, %s file size small then a frame %ld\n", __func__, file, s->frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }

  s->source_begin = mtl_hp_malloc(s->st, i.st_size, MTL_PORT_P);
  if (!s->source_begin) {
    err("%s, source malloc on hugepage fail\n", __func__);
    close(fd);
    return -EIO;
  }

  s->frame_cursor = s->source_begin;
  mtl_memcpy(s->source_begin, m, i.st_size);
  s->source_end = s->source_begin + i.st_size;
  close(fd);

  tx_st22p_open_logo(ctx, s, ctx->logo_url);

  return 0;
}

static int tx_st22p_frame_available(void* priv) {
  struct tx_st22p_sample_ctx* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void tx_st22p_build_frame(struct tx_st22p_sample_ctx* s, struct st_frame* frame) {
  if (s->frame_cursor + s->frame_size > s->source_end) {
    s->frame_cursor = s->source_begin;
  }
  uint8_t* src = s->frame_cursor;

  mtl_memcpy(frame->addr[0], src, s->frame_size);
  if (s->logo_buf) {
    st_draw_logo(frame, &s->logo_meta, 16, 16);
  }

  /* point to next frame */
  s->frame_cursor += s->frame_size;
}

static void* tx_st22p_frame_thread(void* arg) {
  struct tx_st22p_sample_ctx* s = arg;
  st22p_tx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    if (s->source_begin) tx_st22p_build_frame(s, frame);
    st22p_tx_put_frame(handle, frame);
    s->fb_send++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  int bpp = 3;
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  ret = st_sample_tx_init(&ctx, argc, argv);
  if (ret < 0) return ret;

  uint32_t session_num = ctx.sessions;
  struct tx_st22p_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct tx_st22p_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_st22p_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    struct st22p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22p_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.port.num_port = 1;
    memcpy(ops_tx.port.dip_addr[MTL_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_tx.port.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    ops_tx.port.payload_type = ctx.payload_type;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.input_fmt = ctx.st22p_input_fmt;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.codec = ST22_CODEC_JPEGXS;
    ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
    ops_tx.codec_thread_cnt = 2;
    ops_tx.codestream_size = ops_tx.width * ops_tx.height * bpp / 8;
    ops_tx.framebuff_cnt = ctx.framebuff_cnt;
    ops_tx.notify_frame_available = tx_st22p_frame_available;

    st22p_tx_handle tx_handle = st22p_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s(%d), st22p_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle;

    app[i]->frame_size = st22p_tx_frame_size(tx_handle);
    ret = tx_st22p_open_source(&ctx, app[i], ctx.tx_url);

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_st22p_frame_thread, app[i]);
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

    tx_st22p_close_source(app[i]);
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
      if (app[i]->handle) st22p_tx_free(app[i]->handle);
      free(app[i]);
    }
  }

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}
