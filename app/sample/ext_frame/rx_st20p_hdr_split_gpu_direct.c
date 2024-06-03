/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "../sample_util.h"

/* usage:
 * tx: ./build/app/TxSt20PipelineSample --p_sip 192.168.70.12 --p_tx_ip 239.168.70.100
 * --p_port 0000:18:00.0 --udp_port 6970
 *
 * rx: sudo ./build/app/RxSt20pHdrSplitGpuDirect --p_sip 192.168.70.13 --p_rx_ip
 * 239.168.70.100 --p_port 0000:18:00.1  --udp_port 6970 --gddr_pa 0x394200000000
 * --pipeline_fmt YUV422RFC4175PG2BE10
 */

struct rx_st20p_hg_ctx {
  int idx;
  st20p_rx_handle handle;

  bool stop;
  pthread_t frame_thread;

  int fb_recv;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t frame_size;
  int dst_fd;
  uint8_t* dst_begin;
  uint8_t* dst_end;
  uint8_t* dst_cursor;

  int fb_cnt;
  size_t pg_sz;

  struct st_ext_frame gddr_frame;
  bool use_cpu_copy;
  off_t cpu_copy_offset;
};

static int gaddr_profiling(struct rx_st20p_hg_ctx* ctx) {
  clock_t start, end;
  float sec;
  struct st_ext_frame* frame = &ctx->gddr_frame;
  int loop_cnt;
  float throughput_bit;
  uint8_t buf[256];

  info("%s, start on %p, size %" PRIu64 "\n", __func__, frame->addr[0], frame->size);
  /* read */
  loop_cnt = 3;
  start = clock();
  /* read is very slow, not known why */
  size_t r_sz = 0x100000;
  if (frame->size < r_sz) r_sz = frame->size;
  for (int loop = 0; loop < loop_cnt; loop++) {
    uint8_t* addr = (uint8_t*)frame->addr[0];
    for (size_t i = 0; i < r_sz; i++) {
      buf[i & 0xFF] = addr[i];
      dbg("%s, value %u at %d\n", __func__, addr[i], (int)i);
    }
  }
  end = clock();
  sec = (float)(end - start) / CLOCKS_PER_SEC;
  throughput_bit = (float)r_sz * 8 * loop_cnt;
  info("%s, read throughput: %f Mbps, time %fs\n", __func__,
       throughput_bit / sec / 1000 / 1000, sec);

  /* write */
  loop_cnt = 20;
  start = clock();
  for (int loop = 0; loop < loop_cnt; loop++) {
    uint8_t* addr = (uint8_t*)frame->addr[0];
    for (size_t i = 0; i < frame->size; i++) {
      addr[i] = buf[i & 0xFF];
    }
  }
  end = clock();
  sec = (float)(end - start) / CLOCKS_PER_SEC;
  throughput_bit = (float)frame->size * 8 * loop_cnt;
  info("%s, write throughput: %f Mbps, time %fs\n", __func__,
       throughput_bit / sec / 1000 / 1000, sec);
  return 0;
}

static int gddr_map(struct st_sample_context* ctx, struct st_ext_frame* frame, size_t sz,
                    int fd) {
  off_t off = ctx->gddr_pa + ctx->gddr_offset;
  void* map = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
  if (MAP_FAILED == map) {
    err("%s, map size %" PRIu64 " fail\n", __func__, sz);
    return -EIO;
  }
  info("%s, map %p with size %" PRIu64 " offset %" PRIx64 "\n", __func__, map, sz, off);

  mtl_iova_t iova;
  if (mtl_iova_mode_get(ctx->st) == MTL_IOVA_MODE_PA) {
    iova = off; /* use PA */
    dbg("%s, iova pa mode\n", __func__);
  } else {
    dbg("%s, iova va mode\n", __func__);
    iova = mtl_dma_map(ctx->st, map, sz);
    if (MTL_BAD_IOVA == iova) {
      err("%s, dma map fail for va %p sz %" PRIu64 " fail\n", __func__, map, sz);
      munmap(map, sz);
      return -EIO;
    }
  }

  frame->size = sz;
  frame->addr[0] = map;
  frame->iova[0] = iova;
  ctx->gddr_offset += sz;
  return 0;
}

static int rx_st20p_frame_available(void* priv) {
  struct rx_st20p_hg_ctx* s = priv;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st20p_close_source(struct rx_st20p_hg_ctx* s) {
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

static int rx_st20p_open_source(struct rx_st20p_hg_ctx* s, const char* file) {
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

static void rx_st20p_consume_frame(struct rx_st20p_hg_ctx* s, struct st_frame* frame) {
  if (s->dst_fd > 0) {
    if (s->dst_cursor + s->frame_size > s->dst_end) s->dst_cursor = s->dst_begin;
    mtl_memcpy(s->dst_cursor, frame->addr[0], s->frame_size);
    s->dst_cursor += s->frame_size;
  } else {
    uint32_t* d;
    if (s->use_cpu_copy) {
      if (s->cpu_copy_offset + s->frame_size > s->gddr_frame.size) s->cpu_copy_offset = 0;
      void* gddr = s->gddr_frame.addr[0] + s->cpu_copy_offset;
      mtl_memcpy(gddr, frame->addr[0], s->frame_size);
      d = (uint32_t*)gddr;
      s->cpu_copy_offset += s->frame_size;
    } else {
      d = (uint32_t*)frame->addr[0];
    }
    if (0 == (s->fb_recv % 60)) {
      info("%s(%d), frame %p, value 0x%x 0x%x\n", __func__, s->idx, d, d[0], d[1]);
    }
  }
  s->fb_recv++;
}

static void* rx_st20p_frame_thread(void* arg) {
  struct rx_st20p_hg_ctx* s = arg;
  st20p_rx_handle handle = s->handle;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
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
  int dev_mem_fd = -EIO;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  if (!ctx.use_cpu_copy) {
    /* enable hdr split */
    ctx.param.nb_rx_hdr_split_queues = ctx.sessions;
  }

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  struct rx_st20p_hg_ctx* app[session_num];

  memset(app, 0x0, sizeof(app));

  ret = open("/dev/mem", O_RDWR);
  if (ret < 0) {
    err("%s, open /dev/mem fail %d\n", __func__, ret);
    goto error;
  }
  dev_mem_fd = ret;
  dbg("%s, gddr_pa 0x%" PRIx64 "\n", __func__, ctx.gddr_pa);

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(struct rx_st20p_hg_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_st20p_hg_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->dst_fd = -1;
    app[i]->fb_cnt = ctx.framebuff_cnt;
    app[i]->pg_sz = mtl_page_size(ctx.st);
    app[i]->use_cpu_copy = ctx.use_cpu_copy;

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.port.num_port = 1;
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_rx.port.payload_type = ctx.payload_type;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.transport_fmt = ctx.fmt;
    ops_rx.output_fmt = ctx.output_fmt;
    ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_rx.framebuff_cnt = app[i]->fb_cnt;
    ops_rx.notify_frame_available = rx_st20p_frame_available;

    /* map gddr */
    app[i]->frame_size =
        st_frame_size(ops_rx.output_fmt, ops_rx.width, ops_rx.height, ops_rx.interlaced);
    size_t fb_sz = app[i]->frame_size * (app[i]->fb_cnt + 1) + app[i]->pg_sz * 2;
    fb_sz = mtl_size_page_align(fb_sz, app[i]->pg_sz);
    ret = gddr_map(&ctx, &app[i]->gddr_frame, fb_sz, dev_mem_fd);
    if (ret < 0) goto error;
    if (ctx.profiling_gddr) gaddr_profiling(app[i]);

    if (!ctx.use_cpu_copy) {
      ops_rx.flags |= ST20P_RX_FLAG_HDR_SPLIT;
      ops_rx.ext_frames = &app[i]->gddr_frame;
    }

    st20p_rx_handle rx_handle = st20p_rx_create(ctx.st, &ops_rx);
    if (!rx_handle) {
      err("%s(%d), st20p_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle;

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
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_recv);

    rx_st20p_close_source(app[i]);
  }

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
      if (app[i]->gddr_frame.iova[0]) {
        if (mtl_iova_mode_get(ctx.st) != MTL_IOVA_MODE_PA) {
          mtl_dma_unmap(ctx.st, app[i]->gddr_frame.addr[0], app[i]->gddr_frame.iova[0],
                        app[i]->gddr_frame.size);
        }
      }
      if (app[i]->gddr_frame.addr[0]) {
        munmap(app[i]->gddr_frame.addr[0], app[i]->gddr_frame.size);
      }
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
      free(app[i]);
    }
  }

  if (dev_mem_fd > 0) close(dev_mem_fd);
  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
