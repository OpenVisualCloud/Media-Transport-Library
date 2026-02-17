/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "sample_util.h"

#define ST40P_SAMPLE_MAX_UDW_SIZE 2048
#define ST40P_SAMPLE_RTP_RING_SIZE 2048

struct rx_st40p_sample_ctx {
  int idx;
  st40p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;

  int dump_fd;
};

static void rx_st40p_close_dump(struct rx_st40p_sample_ctx* s) {
  if (s->dump_fd >= 0) {
    close(s->dump_fd);
    s->dump_fd = -1;
  }
}

static int rx_st40p_open_dump(struct rx_st40p_sample_ctx* s, const char* file) {
  s->dump_fd = st_open_mode(file, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (s->dump_fd < 0) {
    err("%s(%d), open %s fail\n", __func__, s->idx, file);
    return -EIO;
  }
  return 0;
}

static void rx_st40p_dump_frame(struct rx_st40p_sample_ctx* s,
                                const struct st40_frame_info* frame_info) {
#ifndef WINDOWSENV
  if (s->dump_fd < 0) return;

  dprintf(s->dump_fd, "frame %d meta_num %u udw_bytes %u\n", s->idx, frame_info->meta_num,
          frame_info->udw_buffer_fill);
  for (uint32_t i = 0; i < frame_info->meta_num; i++) {
    const struct st40_meta* meta = &frame_info->meta[i];
    dprintf(
        s->dump_fd,
        "  meta[%u]: line=%u offset=%u stream=%u did=0x%02x sdid=0x%02x udw_size=%u\n", i,
        meta->line_number, meta->hori_offset, meta->stream_num, meta->did, meta->sdid,
        meta->udw_size);
  }
  if (frame_info->udw_buffer_fill) {
    dprintf(s->dump_fd, "  udw:");
    for (uint32_t i = 0; i < frame_info->udw_buffer_fill; i++) {
      if ((i % 32) == 0) dprintf(s->dump_fd, "\n    ");
      dprintf(s->dump_fd, "%02x ", frame_info->udw_buff_addr[i]);
    }
    dprintf(s->dump_fd, "\n");
  }
  dprintf(s->dump_fd, "\n");
#else
  return;
#endif
}

static void rx_st40p_consume_frame(struct rx_st40p_sample_ctx* s,
                                   struct st40_frame_info* frame_info) {
  s->fb_recv++;
  info("%s(%d), frame %d meta_num %u udw_bytes %u\n", __func__, s->idx, s->fb_recv,
       frame_info->meta_num, frame_info->udw_buffer_fill);
  rx_st40p_dump_frame(s, frame_info);
}

static void* rx_st40p_frame_thread(void* arg) {
  struct rx_st40p_sample_ctx* s = arg;
  st40p_rx_handle handle = s->handle;
  struct st40_frame_info* frame_info;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) {
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }

    rx_st40p_consume_frame(s, frame_info);
    st40p_rx_put_frame(handle, frame_info);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret = 0;

  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_st40p_sample_ctx* app[session_num];
  memset(app, 0, sizeof(app));

  for (uint32_t i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(*app[i]));
    if (!app[i]) {
      err("%s(%u), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(*app[i]));
    app[i]->idx = i;
    app[i]->dump_fd = -1;

    struct st40p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st40p_rx_sample";
    ops_rx.priv = app[i];
    ops_rx.port.num_port = ctx.param.num_ports;

    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;

    if (ops_rx.port.num_port > 1) {
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_R], ctx.rx_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
               ctx.param.port[MTL_PORT_R]);
      ops_rx.port.udp_port[MTL_SESSION_PORT_R] = ctx.udp_port + i * 2;
    }

    if (ctx.multi_inc_addr) {
      ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
      ops_rx.port.ip_addr[MTL_SESSION_PORT_P][3] += i;
      if (ops_rx.port.num_port > 1) ops_rx.port.ip_addr[MTL_SESSION_PORT_R][3] += i;
    }

    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.framebuff_cnt = ctx.framebuff_cnt;
    ops_rx.max_udw_buff_size = ST40P_SAMPLE_MAX_UDW_SIZE;
    ops_rx.rtp_ring_size = ST40P_SAMPLE_RTP_RING_SIZE;
    ops_rx.flags = ST40P_RX_FLAG_BLOCK_GET;

    st40p_rx_handle rx_handle = st40p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%u), st40p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

    if (ctx.rx_dump) {
      char dump_file[ST_SAMPLE_URL_MAX_LEN];
      if (session_num == 1) {
        snprintf(dump_file, sizeof(dump_file), "%s", ctx.rx_url);
      } else {
        const size_t suffix_reserve = 16;
        size_t copy_len =
            sizeof(dump_file) > suffix_reserve ? sizeof(dump_file) - suffix_reserve : 0;
        snprintf(dump_file, sizeof(dump_file), "%.*s_%u", (int)copy_len, ctx.rx_url, i);
      }
      ret = rx_st40p_open_dump(app[i], dump_file);
      if (ret < 0) goto error;
    }

    ret = pthread_create(&app[i]->frame_thread, NULL, rx_st40p_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%u), thread create fail %d\n", __func__, i, ret);
      ret = -EIO;
      goto error;
    }
  }

  while (!ctx.exit) {
    sleep(1);
  }

  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    app[i]->stop = true;
    if (app[i]->handle) st40p_rx_wake_block(app[i]->handle);
    pthread_join(app[i]->frame_thread, NULL);
    info("%s(%u), received frames %d\n", __func__, i, app[i]->fb_recv);
    rx_st40p_close_dump(app[i]);
  }

  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->fb_recv <= 0) {
      err("%s(%u), error, no received frames\n", __func__, i);
      ret = -EIO;
    }
  }

error:
  for (uint32_t i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->frame_thread) {
      app[i]->stop = true;
      if (app[i]->handle) st40p_rx_wake_block(app[i]->handle);
      pthread_join(app[i]->frame_thread, NULL);
    }
    if (app[i]->handle) {
      st40p_rx_free(app[i]->handle);
      app[i]->handle = NULL;
    }
    rx_st40p_close_dump(app[i]);
    free(app[i]);
    app[i] = NULL;
  }

  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }

  return ret;
}
