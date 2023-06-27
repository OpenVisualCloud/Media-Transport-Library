/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <sys/queue.h>

#include "../sample_util.h"

#define FB_CNT (4) /* 2 is not enough for this case */

struct frame_info {
  void* frame_addr;
  atomic_int refcnt;
  uint64_t tmstamp;
  TAILQ_ENTRY(frame_info) tailq;
};

TAILQ_HEAD(frameq, frame_info);

struct tx_ctx {
  void* app;
  st20_tx_handle tx_handle;
  size_t fb_offset;
  int fb_idx;
};

struct split_fwd_sample_ctx {
  mtl_handle st;
  st20_rx_handle rx_handle;

  struct frameq q;
  struct frame_info* sending_frames[FB_CNT];

  struct tx_ctx tx[4];
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

static int sending_frames_delete(struct split_fwd_sample_ctx* app, uint64_t tmstamp) {
  int i;
  for (i = 0; i < FB_CNT; i++) {
    struct frame_info* fi = app->sending_frames[i];
    if (fi && fi->tmstamp == tmstamp) {
      atomic_fetch_sub(&fi->refcnt, 1);
      if (atomic_load(&fi->refcnt) == 0) {
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
    err("%s, frame %" PRIu64 " not found\n", __func__, tmstamp);
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

  TAILQ_INSERT_TAIL(&s->q, fi, tailq);

  return 0;
}

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tx_ctx* s = priv;
  struct split_fwd_sample_ctx* app = s->app;

  if (!app->ready) return -EIO;

  int ret;
  int consumer_idx = s->fb_idx;

  struct frame_info* fi = TAILQ_FIRST(&app->q);
  if (fi) {
    ret = 0;
    *next_frame_idx = consumer_idx;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = fi->tmstamp;

    struct st20_ext_frame ext_frame;
    ext_frame.buf_addr = fi->frame_addr + s->fb_offset;
    ext_frame.buf_iova = mtl_hp_virt2iova(app->st, fi->frame_addr) + s->fb_offset;
    ext_frame.buf_len = app->fb_size / 2;
    st20_tx_set_ext_frame(s->tx_handle, consumer_idx, &ext_frame);

    atomic_fetch_add(&fi->refcnt, 1);
    if (atomic_load(&fi->refcnt) == 4) {
      /* all tx set, remove from queue */
      TAILQ_REMOVE(&app->q, fi, tailq);
      /* track the sending frame */
      ret = sending_frames_insert(app, fi);
      if (ret < 0) {
        st20_rx_put_framebuff(app->rx_handle, fi->frame_addr);
        free(fi);
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
  struct tx_ctx* s = priv;
  struct split_fwd_sample_ctx* app = s->app;

  /* try to release the sending frame */
  if (app->ready) sending_frames_delete(app, meta->timestamp);

  return 0;
}

static int split_fwd_sample_free_app(struct split_fwd_sample_ctx* app) {
  for (int i = 0; i < 4; i++) {
    if (app->tx[i].tx_handle) {
      st20_tx_free(app->tx[i].tx_handle);
      app->tx[i].tx_handle = NULL;
    }
  }

  struct frame_info* fi = NULL;
  while (!TAILQ_EMPTY(&app->q)) {
    fi = TAILQ_FIRST(&app->q);
    TAILQ_REMOVE(&app->q, fi, tailq);
    st20_rx_put_framebuff(app->rx_handle, fi->frame_addr);
    free(fi);
  }
  if (app->rx_handle) {
    st20_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }

  return 0;
}

int main(int argc, char** argv) {
  int session_num = 4;
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  sample_parse_args(&ctx, argc, argv, true, false, false);
  ctx.sessions = session_num;
  ctx.param.tx_sessions_cnt_max = session_num;
  ctx.param.rx_sessions_cnt_max = 1;

  struct st20_pgroup st20_pg;
  st20_get_pgroup(ctx.fmt, &st20_pg);

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  struct split_fwd_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.st = ctx.st;

  TAILQ_INIT(&app.q);

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20_fwd";
  ops_rx.priv = &app;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx.rx_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  strncpy(ops_rx.port[MTL_SESSION_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
  ops_rx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = ctx.width;
  ops_rx.height = ctx.height;
  ops_rx.fps = ctx.fps;
  ops_rx.interlaced = ctx.interlaced;
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
    struct tx_ctx* tx = &app.tx[i];
    tx->app = &app;
    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_fwd";
    ops_tx.priv = tx;
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx.fwd_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port[MTL_SESSION_PORT_P], ctx.param.port[MTL_PORT_P],
            MTL_PORT_MAX_LEN);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = ctx.width / 2;
    ops_tx.height = ctx.height / 2;
    ops_tx.linesize = ctx.width * st20_pg.size / st20_pg.coverage;
    ops_tx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
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

  app.tx[0].fb_offset = 0; /* origin */
  app.tx[1].fb_offset = (ctx.width / 2) * st20_pg.size / st20_pg.coverage;
  app.tx[2].fb_offset = (ctx.width / 2) * ctx.height * st20_pg.size / st20_pg.coverage;
  app.tx[3].fb_offset = app.tx[2].fb_offset + app.tx[1].fb_offset;
  app.fb_size = ctx.width * ctx.height * st20_pg.size / st20_pg.coverage;

  app.ready = true;

  // start dev
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop dev
  ret = mtl_stop(ctx.st);
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
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}