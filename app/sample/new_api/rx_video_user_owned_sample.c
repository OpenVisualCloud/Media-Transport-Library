/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file rx_video_user_owned_sample.c
 *
 * New unified API sample: RX video with user-owned buffers (zero-copy).
 *
 * Demonstrates true zero-copy to file: the output file is mmap'd and its
 * pages are posted directly to the library as receive buffers. Received
 * data lands in the file-backed memory with no memcpy.
 *
 * Flow:
 *   1. mmap output file (pre-allocated)
 *   2. mem_register()  — register the mmap'd region for DMA
 *   3. buffer_post()   — provide file-backed pages as receive buffers
 *   4. event_poll()    — wait for MTL_EVENT_BUFFER_READY, re-post
 *
 * Usage:
 *   ./NewApiRxVideoUserOwned --p_port 0000:af:01.1 --p_sip 192.168.96.3 \
 *     --p_rx_ip 239.168.85.20 --udp_port 20000 --rx_dump
 */

#include "../sample_util.h"

#include <mtl/mtl_session_api.h>

#define USER_BUF_CNT 4

/* Application's buffer tracking */
typedef struct {
  void* data;
  size_t size;
  int id;
} app_buffer_t;

struct rx_user_sample_ctx {
  mtl_handle st;
  int idx;
  mtl_session_t* session;

  bool stop;
  pthread_t worker_thread;

  int fb_recv;
  size_t frame_size;

  /* File-backed mmap'd region — serves as both receive buffers AND output file.
   * The library writes directly into these pages — zero copy to disk. */
  int dst_fd;
  uint8_t* dst_begin;
  size_t dst_size;
  int dst_frame_cnt;

  /* DMA handle for the mmap'd region */
  mtl_dma_mem_t* dma_handle;

  /* Per-buffer tracking (points into the mmap'd file, no copy) */
  app_buffer_t buffers[USER_BUF_CNT];
};

/**
 * Open (or create) the output file and mmap it.
 * The mmap'd pages ARE the receive buffers — zero copy.
 */
static int rx_open_dest(struct rx_user_sample_ctx* s, const char* file) {
  int fd, ret, idx = s->idx;

  s->dst_frame_cnt = USER_BUF_CNT;
  s->dst_size = s->dst_frame_cnt * s->frame_size;

  fd = st_open_mode(file, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, file);
    return -EIO;
  }

  ret = ftruncate(fd, s->dst_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, s->dst_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, file);
    close(fd);
    return -EIO;
  }

  s->dst_begin = m;
  s->dst_fd = fd;
  info("%s(%d), mmap'd %s: %d frames, %" PRIu64
       " bytes (zero-copy receive target)\n",
       __func__, idx, file, s->dst_frame_cnt, s->dst_size);
  return 0;
}

static void rx_close_dest(struct rx_user_sample_ctx* s) {
  if (s->dst_begin) {
    msync(s->dst_begin, s->dst_size, MS_SYNC);
    munmap(s->dst_begin, s->dst_size);
    s->dst_begin = NULL;
  }
  if (s->dst_fd >= 0) {
    close(s->dst_fd);
    s->dst_fd = -1;
  }
}

/* Worker thread: polls events and re-posts buffers */
static void* rx_worker_thread(void* arg) {
  struct rx_user_sample_ctx* s = arg;
  mtl_event_t event;
  int ret;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    ret = mtl_session_event_poll(s->session, &event, 1000);
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

    if (event.type == MTL_EVENT_BUFFER_READY) {
      app_buffer_t* buf = (app_buffer_t*)event.ctx;
      if (buf) {
        s->fb_recv++;
        dbg("%s(%d), frame received in buffer %d (already in file, zero-copy)\n",
            __func__, s->idx, buf->id);

        /* Data is already in the file-backed mmap — nothing to copy.
         * Just re-post the buffer for the next frame. */
        ret = mtl_session_buffer_post(s->session, buf->data, buf->size, buf);
        if (ret < 0) {
          err("%s(%d), failed to repost buffer %d: %d\n", __func__, s->idx, buf->id,
              ret);
        }

        if (s->fb_recv % 100 == 0)
          info("%s(%d), received %d frames (zero-copy to file)\n", __func__, s->idx,
               s->fb_recv);
      }
    } else if (event.type == MTL_EVENT_ERROR) {
      err("%s(%d), error event: %d\n", __func__, s->idx, event.status);
    }
  }
  info("%s(%d), stop, received %d frames\n", __func__, s->idx, s->fb_recv);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  /* Default: always dump to file (that's the point of this zero-copy sample) */
  if (!ctx.rx_dump) {
    ctx.rx_dump = true;
    info("rx_dump enabled by default for zero-copy sample\n");
  }

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_user_sample_ctx* app[session_num];

  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_user_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_user_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->dst_fd = -1;

    /* Configure unified session */
    mtl_video_config_t config;
    memset(&config, 0, sizeof(config));
    config.base.direction = MTL_SESSION_RX;
    config.base.ownership = MTL_BUFFER_USER_OWNED;
    config.base.num_buffers = USER_BUF_CNT;
    config.base.name = "new_api_rx_user";

    /* Port config */
    config.rx_port.num_port = ctx.param.num_ports;
    memcpy(config.rx_port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(config.rx_port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    config.rx_port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    if (config.rx_port.num_port > 1) {
      memcpy(config.rx_port.ip_addr[MTL_SESSION_PORT_R], ctx.rx_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(config.rx_port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      config.rx_port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }
    if (ctx.multi_inc_addr) {
      config.rx_port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
      config.rx_port.ip_addr[MTL_SESSION_PORT_P][3] += i;
    }
    config.rx_port.payload_type = ctx.payload_type;

    /* Video format */
    config.width = ctx.width;
    config.height = ctx.height;
    config.fps = ctx.fps;
    config.interlaced = ctx.interlaced;
    config.frame_fmt = ctx.output_fmt;
    config.transport_fmt = ctx.fmt;

    ret = mtl_video_session_create(ctx.st, &config, &app[i]->session);
    if (ret < 0) {
      err("%s(%d), session create fail: %d\n", __func__, i, ret);
      goto error;
    }

    app[i]->frame_size = mtl_session_get_frame_size(app[i]->session);
    info("%s(%d), frame_size %" PRId64 "\n", __func__, i, app[i]->frame_size);

    /* mmap output file — these pages ARE the receive buffers (zero-copy) */
    ret = rx_open_dest(app[i], ctx.rx_url);
    if (ret < 0) {
      err("%s(%d), open dest fail\n", __func__, i);
      goto error;
    }

    /* Register the file-backed mmap region for DMA */
    ret = mtl_session_mem_register(app[i]->session, app[i]->dst_begin, app[i]->dst_size,
                                   &app[i]->dma_handle);
    if (ret < 0) {
      err("%s(%d), mem_register fail: %d\n", __func__, i, ret);
      goto error;
    }

    /* Set up buffer tracking — each buffer points into the mmap'd file.
     * Received data lands directly in the file, zero copy. */
    for (int j = 0; j < USER_BUF_CNT; j++) {
      app[i]->buffers[j].data = app[i]->dst_begin + j * app[i]->frame_size;
      app[i]->buffers[j].size = app[i]->frame_size;
      app[i]->buffers[j].id = j;

      /* Pre-post buffer so library can start receiving into it */
      ret = mtl_session_buffer_post(app[i]->session, app[i]->buffers[j].data,
                                    app[i]->buffers[j].size, &app[i]->buffers[j]);
      if (ret < 0) {
        err("%s(%d), failed to pre-post buffer %d: %d\n", __func__, i, j, ret);
        goto error;
      }
    }

    ret = mtl_session_start(app[i]->session);
    if (ret < 0) {
      err("%s(%d), session start fail: %d\n", __func__, i, ret);
      goto error;
    }

    ret = pthread_create(&app[i]->worker_thread, NULL, rx_worker_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail: %d\n", __func__, i, ret);
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
    pthread_join(app[i]->worker_thread, NULL);
    info("%s(%d), received %d frames (zero-copy to file)\n", __func__, i,
         app[i]->fb_recv);
  }

  /* Check result */
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_recv <= 0) {
      err("%s(%d), error, no received frames %d\n", __func__, i, app[i]->fb_recv);
      ret = -EIO;
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->dma_handle)
        mtl_session_mem_unregister(app[i]->session, app[i]->dma_handle);
      rx_close_dest(app[i]);
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
