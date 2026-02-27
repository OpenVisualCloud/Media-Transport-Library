/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file rx_video_lib_owned_sample.c
 *
 * New unified API sample: RX video with library-owned buffers.
 * Library manages buffer allocation. App uses buffer_get/put loop.
 *
 * Usage:
 *   ./NewApiRxVideoLibOwned --p_port 0000:4b:01.1 --p_sip 192.168.96.3 \
 *     --p_rx_ip 239.168.85.20 --udp_port 20000
 */

#include "../sample_util.h"

#include <mtl/mtl_session_api.h>

struct rx_sample_ctx {
  int idx;
  mtl_session_t* session;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;
  size_t frame_size;

  /* Optional: dump received frames to file */
  int dst_fd;
  uint8_t* dst_begin;
  uint8_t* dst_end;
  uint8_t* dst_cursor;
  int fb_cnt;
};

static int rx_open_dest(struct rx_sample_ctx* s, const char* file) {
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
  info("%s(%d), save %d framebuffers to file %s(%p,%" PRIu64 ")\n", __func__, idx,
       fb_cnt, file, m, f_size);
  return 0;
}

static void rx_close_dest(struct rx_sample_ctx* s) {
  if (s->dst_begin) {
    munmap(s->dst_begin, s->dst_end - s->dst_begin);
    s->dst_begin = NULL;
  }
  if (s->dst_fd >= 0) {
    close(s->dst_fd);
    s->dst_fd = -1;
  }
}

static void rx_consume_frame(struct rx_sample_ctx* s, mtl_buffer_t* buf) {
  s->fb_recv++;
  if (s->dst_fd < 0) return; /* no dump */

  if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;
  mtl_memcpy(s->dst_cursor, buf->data, s->frame_size);
  s->dst_cursor += s->frame_size;
}

static void* rx_frame_thread(void* arg) {
  struct rx_sample_ctx* s = arg;
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

    if (buf->flags & MTL_BUF_FLAG_INCOMPLETE) {
      dbg("%s(%d), incomplete frame\n", __func__, s->idx);
    }

    rx_consume_frame(s, buf);

    ret = mtl_session_buffer_put(session, buf);
    if (ret < 0) {
      err("%s(%d), buffer_put error: %d\n", __func__, s->idx, ret);
      break;
    }

    if (s->fb_recv % 100 == 0)
      info("%s(%d), received %d frames\n", __func__, s->idx, s->fb_recv);
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

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_sample_ctx* app[session_num];

  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_sample_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->dst_fd = -1;
    app[i]->fb_cnt = ctx.framebuff_cnt;

    /* Configure unified session */
    mtl_video_config_t config;
    memset(&config, 0, sizeof(config));
    config.base.direction = MTL_SESSION_RX;
    config.base.ownership = MTL_BUFFER_LIBRARY_OWNED;
    config.base.num_buffers = ctx.framebuff_cnt;
    config.base.name = "new_api_rx_lib";
    config.base.flags = MTL_SESSION_FLAG_BLOCK_GET;

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

    if (ctx.rx_dump) {
      ret = rx_open_dest(app[i], ctx.rx_url);
      if (ret < 0) goto error;
    }

    ret = mtl_session_start(app[i]->session);
    if (ret < 0) {
      err("%s(%d), session start fail: %d\n", __func__, i, ret);
      goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_frame_thread, app[i]);
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
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_recv);
    rx_close_dest(app[i]);
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
