/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file tx_video_lib_owned_sample.c
 *
 * New unified API sample: TX video with library-owned buffers.
 * Library manages buffer allocation. App uses buffer_get/put loop.
 *
 * Usage:
 *   ./NewApiTxVideoLibOwned --p_port 0000:4b:01.0 --p_sip 192.168.96.2 \
 *     --p_tx_ip 239.168.85.20 --udp_port 20000
 */

#include "../sample_util.h"

#include <mtl/mtl_session_api.h>

struct tx_sample_ctx {
  mtl_handle st;
  int idx;
  mtl_session_t* session;

  bool stop;
  pthread_t frame_thread;

  int fb_send;
  size_t frame_size;

  /* Source data (from file or generated) */
  uint8_t* source_begin;
  uint8_t* source_end;
  uint8_t* frame_cursor;
};

static int tx_open_source(struct tx_sample_ctx* s, char* file) {
  int fd = -EIO;
  struct stat i;
  int frame_cnt = 2;
  uint8_t* m = NULL;
  size_t fbs_size = s->frame_size * frame_cnt;

  fd = st_open(file, O_RDONLY);
  if (fd < 0) {
    info("%s, open %s fail, will use generated pattern\n", __func__, file);
    goto init_fb;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, file);
    close(fd);
    return -EIO;
  }
  if (i.st_size < s->frame_size) {
    err("%s, %s file size smaller than a frame %" PRIu64 "\n", __func__, file,
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
  fbs_size = frame_cnt * s->frame_size;
  info("%s, tx_url %s frame_cnt %d\n", __func__, file, frame_cnt);

init_fb:
  s->source_begin = mtl_hp_zmalloc(s->st, fbs_size, MTL_PORT_P);
  if (!s->source_begin) {
    err("%s, source malloc on hugepage fail\n", __func__);
    if (m) munmap(m, i.st_size);
    if (fd >= 0) close(fd);
    return -EIO;
  }
  s->frame_cursor = s->source_begin;
  if (m) {
    mtl_memcpy(s->source_begin, m, fbs_size);
    munmap(m, i.st_size);
  } else {
    /* Fill with test pattern */
    memset(s->source_begin, 0x80, fbs_size);
  }
  s->source_end = s->source_begin + fbs_size;

  if (fd >= 0) close(fd);
  return 0;
}

static void tx_close_source(struct tx_sample_ctx* s) {
  if (s->source_begin) {
    mtl_hp_free(s->st, s->source_begin);
    s->source_begin = NULL;
  }
}

static void* tx_frame_thread(void* arg) {
  struct tx_sample_ctx* s = arg;
  mtl_session_t* session = s->session;
  mtl_buffer_t* buf = NULL;
  int ret;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    ret = mtl_session_buffer_get(session, &buf, 1000);
    if (ret == -EAGAIN) {
      info("%s(%d), session stopped\n", __func__, s->idx);
      break;
    }
    if (ret == -ETIMEDOUT) {
      continue;
    }
    if (ret < 0) {
      err("%s(%d), buffer_get error: %d\n", __func__, s->idx, ret);
      break;
    }

    /* Fill buffer with source data */
    if (s->source_begin) {
      mtl_memcpy(buf->data, s->frame_cursor, s->frame_size);
      s->frame_cursor += s->frame_size;
      if (s->frame_cursor + s->frame_size > s->source_end)
        s->frame_cursor = s->source_begin;
    }

    ret = mtl_session_buffer_put(session, buf);
    if (ret < 0) {
      err("%s(%d), buffer_put error: %d\n", __func__, s->idx, ret);
      break;
    }

    s->fb_send++;
    if (s->fb_send % 100 == 0)
      info("%s(%d), sent %d frames\n", __func__, s->idx, s->fb_send);
  }
  info("%s(%d), stop, sent %d frames\n", __func__, s->idx, s->fb_send);

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
  struct tx_sample_ctx* app[session_num];

  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct tx_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_sample_ctx));
    app[i]->st = ctx.st;
    app[i]->idx = i;
    app[i]->stop = false;

    /* Configure unified session */
    mtl_video_config_t config;
    memset(&config, 0, sizeof(config));
    config.base.direction = MTL_SESSION_TX;
    config.base.ownership = MTL_BUFFER_LIBRARY_OWNED;
    config.base.num_buffers = ctx.framebuff_cnt;
    config.base.name = "new_api_tx_lib";
    config.base.flags = MTL_SESSION_FLAG_BLOCK_GET;

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

    ret = tx_open_source(app[i], ctx.tx_url);
    if (ret < 0) {
      err("%s(%d), open source fail\n", __func__, i);
      goto error;
    }

    ret = mtl_session_start(app[i]->session);
    if (ret < 0) {
      err("%s(%d), session start fail: %d\n", __func__, i, ret);
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, tx_frame_thread, app[i]);
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
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), sent frames %d\n", __func__, i, app[i]->fb_send);
    tx_close_source(app[i]);
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
