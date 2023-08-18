/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct tv_split_sample_ctx {
  int idx;
  int fb_cnt;
  int fb_send;
  int nfi; /* next_frame_idx */
  st20_tx_handle handle;
  struct st20_tx_ops ops;

  size_t frame_size; /* 1080p */
  size_t fb_size;    /* whole 4k */
  int fb_idx;        /* current frame buffer index */
  int fb_total;      /* total frame buffers read from yuv file */
  size_t fb_offset;

  mtl_dma_mem_handle dma_mem;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tv_split_sample_ctx* s = priv;
  int ret = 0;

  if (!s->handle) return -EIO; /* not ready */

  /* the fb_idx of each session is not synced here,
   * which may need to consider in real production */
  struct st20_ext_frame ext_frame;
  ext_frame.buf_addr =
      mtl_dma_mem_addr(s->dma_mem) + s->fb_idx * s->fb_size + s->fb_offset;
  ext_frame.buf_iova =
      mtl_dma_mem_iova(s->dma_mem) + s->fb_idx * s->fb_size + s->fb_offset;
  ext_frame.buf_len = s->frame_size * 2;
  st20_tx_set_ext_frame(s->handle, s->nfi, &ext_frame);

  *next_frame_idx = s->nfi;
  s->nfi++;
  if (s->nfi >= s->fb_cnt) s->nfi = 0;

  s->fb_idx++;
  if (s->fb_idx >= s->fb_total) s->fb_idx = 0;

  return ret;
}

int tx_video_frame_done(void* priv, uint16_t frame_idx, struct st20_tx_frame_meta* meta) {
  struct tv_split_sample_ctx* s = priv;
  s->fb_send++;

  /* when using ext frame, the frame lifetime should be considered */
  return 0;
}

int main(int argc, char** argv) {
  int session_num = 4;
  mtl_dma_mem_handle dma_mem = NULL;
  uint8_t* m = NULL;
  size_t map_size = 0;
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  sample_parse_args(&ctx, argc, argv, true, false, false);
  ctx.sessions = session_num;
  sample_tx_queue_cnt_set(&ctx, session_num);

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  st20_tx_handle tx_handle[session_num];
  memset(tx_handle, 0, sizeof(tx_handle));
  struct tv_split_sample_ctx* app[session_num];
  memset(app, 0, sizeof(app));

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct tv_split_sample_ctx*)malloc(sizeof(struct tv_split_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tv_split_sample_ctx));
    app[i]->idx = i;
    app[i]->nfi = 0;
    app[i]->fb_idx = 0;
    app[i]->fb_total = 0;
    app[i]->fb_cnt = ctx.framebuff_cnt;

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_tx";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);

    struct st20_pgroup st20_pg;
    st20_get_pgroup(ST20_FMT_YUV_422_10BIT, &st20_pg);

    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_GPM_SL;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.linesize = ops_tx.width * 2 * st20_pg.size / st20_pg.coverage;
    ops_tx.fps = ctx.fps;
    ops_tx.interlaced = ctx.interlaced;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.framebuff_cnt = app[i]->fb_cnt;
    // app register non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_video_next_frame;
    ops_tx.notify_frame_done = tx_video_frame_done;
    tx_handle[i] = st20_tx_create(ctx.st, &ops_tx);
    if (!tx_handle[i]) {
      err("%s(%d), st20_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->ops = ops_tx;
    app[i]->frame_size = (size_t)ops_tx.linesize * ops_tx.height / 2;
    app[i]->fb_size = (size_t)ops_tx.linesize * ops_tx.height * 2;

    if (!dma_mem) {
      /* open yuv file and map to memory */
      int fd = -EIO;
      fd = st_open(ctx.tx_url, O_RDONLY);
      if (fd < 0) {
        info("%s, open %s fail, use blank video\n", __func__, ctx.tx_url);
        map_size = app[i]->fb_size * app[i]->fb_cnt;
        goto dma_alloc;
      }
      struct stat st;
      fstat(fd, &st);
      if (st.st_size < (app[i]->fb_size * app[i]->fb_cnt)) {
        err("%s, %s file size too small %" PRIu64 "\n", __func__, ctx.tx_url, st.st_size);
        close(fd);
        ret = -EIO;
        goto error;
      }
      map_size = st.st_size;
      m = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
      if (MAP_FAILED == m) {
        err("%s, mmap %s fail\n", __func__, ctx.tx_url);
        close(fd);
        ret = -EIO;
        goto error;
      }
      close(fd);
    dma_alloc:
      dma_mem = mtl_dma_mem_alloc(ctx.st, map_size);
      if (!dma_mem) {
        err("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      if (m) {
        void* dst = mtl_dma_mem_addr(dma_mem);
        mtl_memcpy(dst, m, map_size);
        munmap(m, map_size);
      }
    }
    app[i]->dma_mem = dma_mem;
    app[i]->fb_total = map_size / app[i]->fb_size;

    app[i]->handle = tx_handle[i];
  }
  /* square division on the 4k frame */
  app[0]->fb_offset = 0;
  app[1]->fb_offset = app[0]->ops.linesize / 2;
  app[2]->fb_offset = app[0]->frame_size * 2;
  app[3]->fb_offset = app[2]->fb_offset + app[1]->fb_offset;

  // start tx
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
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
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->handle) st20_tx_free(app[i]->handle);
    info("%s(%d), sent frames %d\n", __func__, i, app[i]->fb_send);
    free(app[i]);
  }
  if (dma_mem) mtl_dma_mem_free(ctx.st, dma_mem);

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
