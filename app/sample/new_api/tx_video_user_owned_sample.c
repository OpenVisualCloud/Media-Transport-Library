/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file tx_video_user_owned_sample.c
 *
 * New unified API sample: TX video with user-owned buffers (zero-copy).
 *
 * Demonstrates true zero-copy: the source file is mmap'd and its pages
 * are posted directly to the library for DMA transmission — no memcpy.
 *
 * Flow:
 *   1. mmap source file
 *   2. mem_register()  — register the mmap'd region for DMA
 *   3. buffer_post()   — submit file-backed pages for transmission
 *   4. event_poll()    — wait for MTL_EVENT_BUFFER_DONE to re-post
 *
 * Usage:
 *   ./NewApiTxVideoUserOwned --p_port 0000:4b:01.0 --p_sip 192.168.96.2 \
 *     --p_tx_ip 239.168.85.20 --udp_port 20000 --tx_url source.yuv
 */

#include "../sample_util.h"

#include <mtl/mtl_session_api.h>

#define USER_BUF_CNT 4

/* Application's buffer tracking */
typedef struct {
  void* data;
  size_t size;
  int id;
  volatile int in_use; /* 1 = submitted to library */
} app_buffer_t;

struct tx_user_sample_ctx {
  mtl_handle st;
  int idx;
  mtl_session_t* session;

  bool stop;
  pthread_t producer_thread;
  pthread_t event_thread;

  int fb_send;
  int fb_done;
  size_t frame_size;

  /* Source file mmap'd directly — serves as both source data AND transmit buffers */
  int src_fd;
  uint8_t* src_begin;
  size_t src_size;
  int src_frame_cnt;

  /* DMA handle for the mmap'd region */
  mtl_dma_mem_t* dma_handle;

  /* Per-buffer tracking (points into the mmap'd region, no copy) */
  app_buffer_t buffers[USER_BUF_CNT];
};

/**
 * Open source file and mmap it. If no file, allocate hugepage with test pattern.
 * The mmap'd region is used directly as transmit buffers — zero copy.
 */
static int tx_open_source(struct tx_user_sample_ctx* s, char* file) {
  struct stat st;
  int fd;

  s->src_fd = -1;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    info("%s, open %s fail, will use hugepage with test pattern\n", __func__, file);
    goto fallback_hugepage;
  }

  if (fstat(fd, &st) < 0) {
    err("%s, fstat %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }
  if ((size_t)st.st_size < s->frame_size) {
    err("%s, %s file size %" PRId64 " < frame_size %" PRIu64 "\n", __func__, file,
        (int64_t)st.st_size, s->frame_size);
    close(fd);
    return -EIO;
  }

  s->src_begin = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (s->src_begin == MAP_FAILED) {
    err("%s, mmap %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }

  s->src_fd = fd;
  s->src_frame_cnt = st.st_size / s->frame_size;
  /* Trim to frame-aligned size */
  s->src_size = s->src_frame_cnt * s->frame_size;
  info("%s, mmap'd %s: %d frames, %" PRIu64 " bytes (zero-copy source)\n", __func__,
       file, s->src_frame_cnt, s->src_size);
  return 0;

fallback_hugepage:
  /* No file — allocate hugepage memory with a test pattern */
  s->src_frame_cnt = USER_BUF_CNT;
  s->src_size = s->src_frame_cnt * s->frame_size;
  s->src_begin = mtl_hp_zmalloc(s->st, s->src_size, MTL_PORT_P);
  if (!s->src_begin) {
    err("%s, hugepage malloc fail\n", __func__);
    return -ENOMEM;
  }
  memset(s->src_begin, 0x80, s->src_size);
  info("%s, using hugepage test pattern: %d frames, %" PRIu64 " bytes\n", __func__,
       s->src_frame_cnt, s->src_size);
  if (fd >= 0) close(fd);
  return 0;
}

static void tx_close_source(struct tx_user_sample_ctx* s) {
  if (s->src_begin) {
    if (s->src_fd >= 0) {
      munmap(s->src_begin, s->src_size);
    } else {
      mtl_hp_free(s->st, s->src_begin);
    }
    s->src_begin = NULL;
  }
  if (s->src_fd >= 0) {
    close(s->src_fd);
    s->src_fd = -1;
  }
}

/**
 * Producer thread: posts buffers pointing directly into the mmap'd source file.
 * No memcpy — the DMA engine reads from the file-backed pages.
 */
static void* tx_producer_thread(void* arg) {
  struct tx_user_sample_ctx* s = arg;
  int next_buf = 0;
  int ret;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    app_buffer_t* buf = &s->buffers[next_buf];

    /* Wait for buffer to be free */
    while (buf->in_use && !s->stop) {
      usleep(1000);
    }
    if (s->stop) break;

    /* Post buffer for transmission (data already points into mmap'd file) */
    buf->in_use = 1;
    ret = mtl_session_buffer_post(s->session, buf->data, buf->size, buf);
    if (ret < 0) {
      err("%s(%d), buffer_post failed: %d\n", __func__, s->idx, ret);
      buf->in_use = 0;
      if (ret == -EAGAIN) break; /* Session stopped */
      break;
    }

    s->fb_send++;
    next_buf = (next_buf + 1) % USER_BUF_CNT;

    if (s->fb_send % 100 == 0)
      info("%s(%d), posted %d frames (zero-copy)\n", __func__, s->idx, s->fb_send);
  }
  info("%s(%d), stop, posted %d frames\n", __func__, s->idx, s->fb_send);

  return NULL;
}

/* Event thread: handles completion events */
static void* tx_event_thread(void* arg) {
  struct tx_user_sample_ctx* s = arg;
  mtl_event_t event;
  int ret;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    ret = mtl_session_event_poll(s->session, &event, 100);
    if (ret == -EAGAIN) {
      info("%s(%d), session stopped\n", __func__, s->idx);
      break;
    }
    if (ret == -ETIMEDOUT) {
      continue;
    }
    if (ret < 0) {
      err("%s(%d), event_poll error: %d\n", __func__, s->idx, ret);
      break;
    }

    if (event.type == MTL_EVENT_BUFFER_DONE) {
      app_buffer_t* buf = (app_buffer_t*)event.ctx;
      if (buf) {
        buf->in_use = 0;
        s->fb_done++;
        dbg("%s(%d), buffer %d done, total %d\n", __func__, s->idx, buf->id,
            s->fb_done);
      }
    } else if (event.type == MTL_EVENT_ERROR) {
      err("%s(%d), error event: %d\n", __func__, s->idx, event.status);
    }
  }
  info("%s(%d), stop, completed %d frames\n", __func__, s->idx, s->fb_done);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct tx_user_sample_ctx* app[session_num];

  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct tx_user_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_user_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->src_fd = -1;

    /* Configure unified session */
    mtl_video_config_t config;
    memset(&config, 0, sizeof(config));
    config.base.direction = MTL_SESSION_TX;
    config.base.ownership = MTL_BUFFER_USER_OWNED;
    config.base.num_buffers = USER_BUF_CNT;
    config.base.name = "new_api_tx_user";

    /* Port config */
    config.tx_port.num_port = ctx.param.num_ports;
    memcpy(config.tx_port.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(config.tx_port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    config.tx_port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    if (config.tx_port.num_port > 1) {
      memcpy(config.tx_port.dip_addr[MTL_SESSION_PORT_R], ctx.tx_dip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(config.tx_port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      config.tx_port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }
    if (ctx.multi_inc_addr) {
      config.tx_port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
      config.tx_port.dip_addr[MTL_SESSION_PORT_P][3] += i;
    }
    config.tx_port.payload_type = ctx.payload_type;

    /* Video format */
    config.width = ctx.width;
    config.height = ctx.height;
    config.fps = ctx.fps;
    config.interlaced = ctx.interlaced;
    config.frame_fmt = ctx.input_fmt;
    config.transport_fmt = ctx.fmt;
    config.packing = ctx.packing;
    config.pacing = ST21_PACING_NARROW;

    ret = mtl_video_session_create(ctx.st, &config, &app[i]->session);
    if (ret < 0) {
      err("%s(%d), session create fail: %d\n", __func__, i, ret);
      goto error;
    }

    app[i]->frame_size = mtl_session_get_frame_size(app[i]->session);
    info("%s(%d), frame_size %" PRId64 "\n", __func__, i, app[i]->frame_size);

    /* Open source: mmap file directly (zero-copy) or hugepage fallback */
    ret = tx_open_source(app[i], ctx.tx_url);
    if (ret < 0) {
      err("%s(%d), open source fail\n", __func__, i);
      goto error;
    }

    /* Register the mmap'd/hugepage source region for DMA */
    ret = mtl_session_mem_register(app[i]->session, app[i]->src_begin, app[i]->src_size,
                                   &app[i]->dma_handle);
    if (ret < 0) {
      err("%s(%d), mem_register fail: %d\n", __func__, i, ret);
      goto error;
    }

    /* Set up buffer tracking — each buffer points directly into the source region.
     * Buffers cycle through the source frames with no copy. */
    for (int j = 0; j < USER_BUF_CNT; j++) {
      int frame_idx = j % app[i]->src_frame_cnt;
      app[i]->buffers[j].data = app[i]->src_begin + frame_idx * app[i]->frame_size;
      app[i]->buffers[j].size = app[i]->frame_size;
      app[i]->buffers[j].id = j;
      app[i]->buffers[j].in_use = 0;
    }

    ret = mtl_session_start(app[i]->session);
    if (ret < 0) {
      err("%s(%d), session start fail: %d\n", __func__, i, ret);
      goto error;
    }

    /* Start worker threads */
    ret = pthread_create(&app[i]->event_thread, NULL, tx_event_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), event thread create fail: %d\n", __func__, i, ret);
      ret = -EIO;
      goto error;
    }
    ret = pthread_create(&app[i]->producer_thread, NULL, tx_producer_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), producer thread create fail: %d\n", __func__, i, ret);
      ret = -EIO;
      goto error;
    }
  }

  while (!ctx.exit) {
    sleep(1);
  }

  /* Stop */
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    if (app[i]->session) mtl_session_stop(app[i]->session);
    pthread_join(app[i]->producer_thread, NULL);
    pthread_join(app[i]->event_thread, NULL);
    info("%s(%d), sent %d frames, completed %d (zero-copy)\n", __func__, i,
         app[i]->fb_send, app[i]->fb_done);
  }

  /* Check result */
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_send <= 0) {
      err("%s(%d), error, no sent frames %d\n", __func__, i, app[i]->fb_send);
      ret = -EIO;
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->dma_handle)
        mtl_session_mem_unregister(app[i]->session, app[i]->dma_handle);
      tx_close_source(app[i]);
      if (app[i]->session) mtl_session_destroy(app[i]->session);
      free(app[i]);
    }
  }

  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
