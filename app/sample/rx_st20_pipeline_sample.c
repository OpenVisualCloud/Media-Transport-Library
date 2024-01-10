/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct rx_st20p_sample_ctx {
  int idx;
  st20p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;

  size_t frame_size;
  int dst_fd;
  uint8_t* dst_begin;
  uint8_t* dst_end;
  uint8_t* dst_cursor;

  int fb_cnt;
};

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
  s->fb_recv++;
  if (s->dst_fd < 0) return; /* no dump */

  if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;
  mtl_memcpy(s->dst_cursor, frame->addr[0], s->frame_size);
  s->dst_cursor += s->frame_size;
}

static void* rx_st20p_frame_thread(void* arg) {
  struct rx_st20p_sample_ctx* s = arg;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }
    dbg("%s(%d), one new frame\n", __func__, s->idx);
    if (frame->user_meta) {
      const struct st_frame_user_meta* user_meta = frame->user_meta;
      if (frame->user_meta_size != sizeof(*user_meta)) {
        err("%s(%d), user_meta_size wrong\n", __func__, s->idx);
      }
      info("%s(%d), user_meta %d %s\n", __func__, s->idx, user_meta->idx,
           user_meta->dummy);
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

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_st20p_sample_ctx* app[session_num];

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
    app[i]->dst_fd = -1;
    app[i]->fb_cnt = ctx.framebuff_cnt;

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = ctx.param.num_ports;
    memcpy(ops_rx.port.sip_addr[MTL_SESSION_PORT_P], ctx.rx_sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    if (ops_rx.port.num_port > 1) {
      memcpy(ops_rx.port.sip_addr[MTL_SESSION_PORT_R], ctx.rx_sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      ops_rx.port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }
    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.transport_fmt = ctx.fmt;
    ops_rx.output_fmt = ctx.output_fmt;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.framebuff_cnt = app[i]->fb_cnt;
    ops_rx.flags = ST20P_RX_FLAG_BLOCK_GET;

    st20p_rx_handle rx_handle = st20p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%d), st20p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

    app[i]->frame_size = st20p_rx_frame_size(rx_handle);
    info("%s(%d), frame_size %" PRId64 "\n", __func__, i, app[i]->frame_size);
    if (ctx.rx_dump) {
      ret = rx_st20p_open_source(app[i], ctx.rx_url);
      if (ret < 0) {
        goto error;
      }
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st20p_frame_thread, app[i]);
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
    if (app[i]->handle) st20p_rx_wake_block(app[i]->handle);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_recv);

    rx_st20p_close_source(app[i]);
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
      if (app[i]->handle) st20p_rx_free(app[i]->handle);
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
