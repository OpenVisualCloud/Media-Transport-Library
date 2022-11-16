/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <rte_ring.h>

#include "sample_util.h"

#define FB_CNT (4) /* 2 is not enough for this case */

struct frame_info {
  void* frame_addr;
  int refcnt;
  uint64_t tmstamp;
};

struct tx {
  void* app;
  st20_tx_handle tx_handle;
  size_t fb_offset;
  int fb_idx;
};

struct split_fwd_sample_ctx {
  mtl_handle st;
  st20_rx_handle rx_handle;

  struct rte_ring* frame_ring;
  struct frame_info* sending_frames[FB_CNT];

  struct tx tx[4];
  size_t fb_size;

  bool ready;

  int fb_fwd;
};

static int sending_frames_insert(struct split_fwd_sample_ctx* app,
                                 struct frame_info* fi) {
  int i;
  for (i = 0; i < FB_CNT; i++) {
    if (app->sending_frames[i] == NULL) {
      app->sending_frames[i] = fi;
      break;
    }
  }
  if (i >= FB_CNT) {
    err("%s, no slot\n", __func__);
    return -EIO;
  }
  return 0;
}

static int sending_frames_delete(struct split_fwd_sample_ctx* app, void* frame_addr) {
  int i;
  for (i = 0; i < FB_CNT; i++) {
    struct frame_info* fi = app->sending_frames[i];
    if (fi && fi->frame_addr <= frame_addr &&
        frame_addr < fi->frame_addr + app->fb_size) {
      fi->refcnt--;
      if (fi->refcnt == 0) {
        /* all tx sent, release the frame */
        st20_rx_put_framebuff(app->rx_handle, fi->frame_addr);
        free(fi);
        app->sending_frames[i] = NULL;
        app->fb_fwd++;
      }
      break;
    }
  }
  if (i >= FB_CNT) {
    err("%s, frame %p not found\n", __func__, frame_addr);
    return -EIO;
  }
  return 0;
}

static int rx_st20_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  struct split_fwd_sample_ctx* s = (struct split_fwd_sample_ctx*)priv;

  if (!s->ready) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status) || meta->tfmt != ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return -EIO;
  }

  struct frame_info* fi = (struct frame_info*)malloc(sizeof(struct frame_info));
  if (!fi) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return -EIO;
  }
  memset(fi, 0, sizeof(*fi));

  fi->frame_addr = frame;
  fi->refcnt = 0;
  fi->tmstamp = meta->timestamp;

  if (-ENOBUFS == rte_ring_sp_enqueue(s->frame_ring, fi)) {
    err("%s, enqueue frame fail\n", __func__);
    return -EIO;
  }

  return 0;
}

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tx* s = priv;
  struct split_fwd_sample_ctx* app = s->app;

  if (!app->ready) return -EIO;

  int ret;
  int consumer_idx = s->fb_idx;

  struct frame_info* fi[1];
  uint32_t n = rte_ring_dequeue_bulk_start(app->frame_ring, (void**)&fi, 1, NULL);
  if (n != 0) { /* peak the frame from ring */
    ret = 0;
    *next_frame_idx = consumer_idx;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = fi[0]->tmstamp;

    struct st20_ext_frame ext_frame;
    ext_frame.buf_addr = fi[0]->frame_addr + s->fb_offset;
    ext_frame.buf_iova = st_hp_virt2iova(app->st, fi[0]->frame_addr) + s->fb_offset;
    ext_frame.buf_len = 3840 * 2160 * 5 / 4; /* 2 1080p framesize */
    st20_tx_set_ext_frame(s->tx_handle, consumer_idx, &ext_frame);

    fi[0]->refcnt++;
    if (fi[0]->refcnt < 4) {
      /* keep it */
      rte_ring_dequeue_finish(app->frame_ring, 0);
    } else {
      /* all tx set, remove from ring */
      rte_ring_dequeue_finish(app->frame_ring, n);
      /* track the sending frame */
      ret = sending_frames_insert(app, fi[0]);
      if (ret < 0) {
        st20_rx_put_framebuff(app->rx_handle, fi[0]->frame_addr);
        free(fi[0]);
      }
    }

    consumer_idx++;
    if (consumer_idx >= FB_CNT) consumer_idx = 0;
    s->fb_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
  }

  return ret;
}

static int tx_video_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tx* s = priv;
  struct split_fwd_sample_ctx* app = s->app;
  void* frame_addr = st20_tx_get_framebuffer(s->tx_handle, frame_idx);

  /* try to release the sending frame */
  if (app->ready) sending_frames_delete(s->app, frame_addr);

  return 0;
}

static int split_fwd_sample_free_app(struct split_fwd_sample_ctx* app) {
  for (int i = 0; i < 4; i++) {
    if (app->tx[i].tx_handle) {
      st20_tx_free(app->tx[i].tx_handle);
      app->tx[i].tx_handle = NULL;
    }
  }
  if (app->rx_handle) {
    st20_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  if (app->frame_ring) {
    struct frame_info* fi = NULL;
    int ret;
    /* dequeue and free all frames in the ring */
    do {
      ret = rte_ring_sc_dequeue(app->frame_ring, (void**)&fi);
      if (ret < 0) break;
      dbg("%s, fi %p in frame_ring\n", __func__, fi);
      free(fi);
    } while (1);

    rte_ring_free(app->frame_ring);
    app->frame_ring = NULL;
  }

  return 0;
}

int main(int argc, char** argv) {
  int session_num = 4;
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  st_sample_init(&ctx, argc, argv, true, false);
  ctx.sessions = session_num;
  ctx.param.tx_sessions_cnt_max = session_num;
  ctx.param.rx_sessions_cnt_max = 1;
  ret = st_sample_start(&ctx);
  if (ret < 0) return ret;

  struct split_fwd_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.st = ctx.st;

  /* this sample schedules all sessions on single core,
   * so the ring created is not multi-thread safe */
  struct rte_ring* frame_ring =
      rte_ring_create("frame_ring", FB_CNT, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!frame_ring) {
    err("%s, ring create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.frame_ring = frame_ring;

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20_fwd";
  ops_rx.priv = &app;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[MTL_PORT_P], ctx.rx_sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  strncpy(ops_rx.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
  ops_rx.udp_port[MTL_PORT_P] = ctx.udp_port;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = 3840;
  ops_rx.height = 2160;
  ops_rx.fps = ctx.fps;
  ops_rx.fmt = ctx.fmt;
  ops_rx.framebuff_cnt = FB_CNT;
  ops_rx.payload_type = ctx.payload_type;
  ops_rx.flags = 0;
  ops_rx.notify_frame_ready = rx_st20_frame_ready;
  st20_rx_handle rx_handle = st20_rx_create(ctx.st, &ops_rx);
  if (!rx_handle) {
    err("%s, st20_rx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.rx_handle = rx_handle;

  for (int i = 0; i < 4; i++) {
    struct tx* tx = &app.tx[i];
    tx->app = &app;
    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_fwd";
    ops_tx.priv = tx;
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_PORT_P], ctx.fwd_dip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_tx.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.linesize = 9600; /* double 1080p linesize */
    ops_tx.fps = ctx.fps;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.framebuff_cnt = FB_CNT;
    ops_tx.get_next_frame = tx_video_next_frame;
    ops_tx.notify_frame_done = tx_video_frame_done;
    st20_tx_handle tx_handle = st20_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s, st20_tx_create fail\n", __func__);
      ret = -EIO;
      goto error;
    }
    tx->tx_handle = tx_handle;
  }

  app.tx[0].fb_offset = 0;               /* origin */
  app.tx[1].fb_offset = 4800;            /* 1080p linesize */
  app.tx[2].fb_offset = 1920 * 1080 * 5; /* 2 1080p framesize */
  app.tx[3].fb_offset = app.tx[2].fb_offset + app.tx[1].fb_offset;
  app.fb_size = 3840 * 2160 * 5 / 2; /* 4k framesize */

  app.ready = true;

  // start dev
  ret = st_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop dev
  ret = st_stop(ctx.st);
  info("%s, fb_fwd %d\n", __func__, app.fb_fwd);
  app.ready = false;

  // check result
  if (app.fb_fwd <= 0) {
    err("%s, error, no fwd frames %d\n", __func__, app.fb_fwd);
    ret = -EIO;
  }

error:
  // release session
  split_fwd_sample_free_app(&app);

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}