/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_video_app.h"

static int pg_convert_callback(void* priv, void* frame,
                               struct st20_rx_uframe_pg_meta* meta) {
  struct st_app_rx_video_session* s = priv;

  uint32_t offset = (meta->row_number * s->width + meta->row_offset) /
                    s->user_pg.coverage * s->user_pg.size;

  convert_uyvy10b_to_uyvy8b(frame + offset, meta->payload, meta->pg_cnt);

  return 0;
}

static inline bool app_rx_video_is_frame_type(enum st20_type type) {
  if ((type == ST20_TYPE_FRAME_LEVEL) || (type == ST20_TYPE_SLICE_LEVEL))
    return true;
  else
    return false;
}

static int app_rx_video_enqueue_frame(struct st_app_rx_video_session* s, void* frame,
                                      size_t size) {
  uint16_t producer_idx = s->framebuff_producer_idx;
  struct st_rx_frame* framebuff = &s->framebuffs[producer_idx];

  if (framebuff->frame) {
    return -EBUSY;
  }

  dbg("%s(%d), frame idx %d\n", __func__, s->idx, producer_idx);
  framebuff->frame = frame;
  framebuff->size = size;
  /* point to next */
  producer_idx++;
  if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
  s->framebuff_producer_idx = producer_idx;
  return 0;
}

static void app_rx_video_consume_frame(struct st_app_rx_video_session* s, void* frame,
                                       size_t frame_size) {
  struct st_display* d = s->display;

  if (d && d->front_frame) {
    if (st_pthread_mutex_trylock(&d->display_frame_mutex) == 0) {
      if (s->st20_pg.fmt == ST20_FMT_YUV_422_8BIT ||
          s->user_pg.fmt == USER_FMT_YUV_422_8BIT)
        mtl_memcpy(d->front_frame, frame, d->front_frame_size);
      else if (s->st20_pg.fmt == ST20_FMT_YUV_422_10BIT)
        st20_rfc4175_422be10_to_422le8(frame, d->front_frame, s->width, s->height);
      else /* fmt mismatch*/ {
        st_pthread_mutex_unlock(&d->display_frame_mutex);
        return;
      }
      st_pthread_mutex_unlock(&d->display_frame_mutex);
      st_pthread_mutex_lock(&d->display_wake_mutex);
      st_pthread_cond_signal(&d->display_wake_cond);
      st_pthread_mutex_unlock(&d->display_wake_mutex);
    }
  } else {
    if (s->st20_dst_cursor + frame_size > s->st20_dst_end)
      s->st20_dst_cursor = s->st20_dst_begin;
    dbg("%s(%d), dst %p src %p size %ld\n", __func__, s->idx, s->st20_dst_cursor, frame,
        frame_size);
    mtl_memcpy(s->st20_dst_cursor, frame, frame_size);
    s->st20_dst_cursor += frame_size;
  }
}

static void* app_rx_video_frame_thread(void* arg) {
  struct st_app_rx_video_session* s = arg;
  int idx = s->idx;
  int consumer_idx;
  struct st_rx_frame* framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    st_pthread_mutex_lock(&s->st20_wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[consumer_idx];
    if (!framebuff->frame) {
      /* no ready frame */
      if (!s->st20_app_thread_stop)
        st_pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
      st_pthread_mutex_unlock(&s->st20_wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->st20_wake_mutex);

    dbg("%s(%d), frame idx %d\n", __func__, idx, consumer_idx);
    app_rx_video_consume_frame(s, framebuff->frame, framebuff->size);
    st20_rx_put_framebuff(s->handle, framebuff->frame);
    /* point to next */
    st_pthread_mutex_lock(&s->st20_wake_mutex);
    framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->st20_wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_video_handle_rtp(struct st_app_rx_video_session* s,
                                   struct st20_rfc4175_rtp_hdr* hdr) {
  int idx = s->idx;
  uint32_t tmstamp = ntohl(hdr->base.tmstamp);
  struct st20_rfc4175_extra_rtp_hdr* e_hdr = NULL;
  uint16_t row_number; /* 0 to 1079 for 1080p */
  uint16_t row_offset; /* [0, 480, 960, 1440] for 1080p */
  uint16_t row_length; /* 1200 for 1080p */
  uint8_t* frame;
  uint8_t* payload;

  dbg("%s(%d),tmstamp: 0x%x\n", __func__, idx, tmstamp);
  if (tmstamp != s->st20_last_tmstamp) {
    /* new frame received */
    s->st20_last_tmstamp = tmstamp;
    s->stat_frame_received++;
    s->stat_frame_total_received++;
    if (!s->stat_frame_frist_rx_time)
      s->stat_frame_frist_rx_time = st_app_get_monotonic_time();

    s->st20_dst_cursor += s->st20_frame_size;
    if ((s->st20_dst_cursor + s->st20_frame_size) > s->st20_dst_end)
      s->st20_dst_cursor = s->st20_dst_begin;
  }

  if (s->st20_dst_fd < 0) return 0;

  frame = s->st20_dst_cursor;
  payload = (uint8_t*)hdr + sizeof(*hdr);
  row_number = ntohs(hdr->row_number);
  row_offset = ntohs(hdr->row_offset);
  row_length = ntohs(hdr->row_length);
  dbg("%s(%d), row: %d %d %d\n", __func__, idx, row_number, row_offset, row_length);
  if (row_offset & ST20_SRD_OFFSET_CONTINUATION) {
    /* additional Sample Row Data */
    row_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    e_hdr = (struct st20_rfc4175_extra_rtp_hdr*)payload;
    payload += sizeof(*e_hdr);
  }

  if (row_number & ST20_SECOND_FIELD) {
    row_number &= ~ST20_SECOND_FIELD;
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  if ((offset + row_length) > s->st20_frame_size) {
    err("%s(%d: invalid offset %u frame size %d\n", __func__, idx, offset,
        s->st20_frame_size);
    return -EIO;
  }
  mtl_memcpy(frame + offset, payload, row_length);
  if (e_hdr) {
    uint16_t row2_number = ntohs(e_hdr->row_number);
    uint16_t row2_offset = ntohs(e_hdr->row_offset);
    uint16_t row2_length = ntohs(e_hdr->row_length);

    if (row2_number & ST20_SECOND_FIELD) {
      row2_number &= ~ST20_SECOND_FIELD;
    }

    dbg("%s(%d), row: %d %d %d\n", __func__, idx, row2_number, row2_offset, row2_length);
    uint32_t offset2 =
        (row2_number * s->width + row2_offset) / s->st20_pg.coverage * s->st20_pg.size;
    if ((offset2 + row2_length) > s->st20_frame_size) {
      err("%s(%d: invalid offset %u frame size %d for extra hdr\n", __func__, idx,
          offset2, s->st20_frame_size);
      return -EIO;
    }
    mtl_memcpy(frame + offset2, payload + row_length, row2_length);
  }

  return 0;
}

static void* app_rx_video_rtp_thread(void* arg) {
  struct st_app_rx_video_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st20_rfc4175_rtp_hdr* hdr;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    mbuf = st20_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      st_pthread_mutex_lock(&s->st20_wake_mutex);
      if (!s->st20_app_thread_stop)
        st_pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
      st_pthread_mutex_unlock(&s->st20_wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    app_rx_video_handle_rtp(s, hdr);
    /* free to lib */
    st20_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_video_close_source(struct st_app_rx_video_session* s) {
  if (s->st20_dst_fd >= 0) {
    munmap(s->st20_dst_begin, s->st20_dst_end - s->st20_dst_begin);
    close(s->st20_dst_fd);
    s->st20_dst_fd = -1;
  }

  return 0;
}

static int app_rx_video_open_source(struct st_app_rx_video_session* s) {
  int fd, ret, idx = s->idx;
  off_t f_size;

  /* user do not require fb save to file */
  if (s->st20_dst_fb_cnt < 1) return 0;

  fd = st_open_mode(s->st20_dst_url, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, s->st20_dst_url);
    return -EIO;
  }

  f_size = s->st20_dst_fb_cnt * s->st20_frame_size;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, s->st20_dst_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, s->st20_dst_url);
    close(fd);
    return -EIO;
  }

  s->st20_dst_begin = m;
  s->st20_dst_cursor = m;
  s->st20_dst_end = m + f_size;
  s->st20_dst_fd = fd;
  info("%s(%d), save %d framebuffers to file %s(%p,%" PRIu64 ")\n", __func__, idx,
       s->st20_dst_fb_cnt, s->st20_dst_url, m, f_size);

  return 0;
}

static int app_rx_video_init_frame_thread(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  /* user do not require fb save to file or display */
  if (s->st20_dst_fb_cnt < 1 && s->display == NULL) return 0;

  ret = pthread_create(&s->st20_app_thread, NULL, app_rx_video_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st20_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_video_init_rtp_thread(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st20_app_thread, NULL, app_rx_video_rtp_thread, s);
  if (ret < 0) {
    err("%s(%d), st20_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_video_frame_ready(void* priv, void* frame,
                                    struct st20_rx_frame_meta* meta) {
  struct st_app_rx_video_session* s = priv;
  int ret;

  if (!s->handle) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  s->stat_frame_received++;
  if (s->measure_latency) {
    uint64_t latency_ns;
    uint64_t ptp_ns = st_ptp_read_time(s->st);
    uint32_t sampling_rate = 90 * 1000;

    if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
      uint32_t latency_media_clk =
          st10_tai_to_media_clk(ptp_ns, sampling_rate) - meta->timestamp;
      latency_ns = st10_media_clk_to_ns(latency_media_clk, sampling_rate);
    } else {
      latency_ns = ptp_ns - meta->timestamp;
    }
    dbg("%s, latency_us %lu\n", __func__, latency_ns / 1000);
    s->stat_latency_us_sum += latency_ns / 1000;
  }
  s->stat_frame_total_received++;
  if (!s->stat_frame_frist_rx_time)
    s->stat_frame_frist_rx_time = st_app_get_monotonic_time();

  if (s->st20_dst_fd < 0 && s->display == NULL) {
    /* free the queue directly as no read thread is running */
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  ret = app_rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
    st_pthread_mutex_unlock(&s->st20_wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  return 0;
}

static int app_rx_video_slice_ready(void* priv, void* frame,
                                    struct st20_rx_slice_meta* meta) {
  struct st_app_rx_video_session* s = priv;
  int idx = s->idx;
  size_t frame_ready_size;

  if (!s->handle) return -EIO;

  frame_ready_size =
      meta->frame_recv_lines * s->width * s->st20_pg.size / s->st20_pg.coverage;

  dbg("%s(%d), lines %u ready %" PRIu64 " recv_size %" PRIu64 " \n", __func__, idx,
      meta->frame_recv_lines, frame_ready_size, meta->frame_recv_size);
  if (meta->frame_recv_size < frame_ready_size) {
    err("%s(%d), lines %u ready %" PRIu64 " recv_size %" PRIu64 " error\n", __func__, idx,
        meta->frame_recv_lines, frame_ready_size, meta->frame_recv_size);
  }

  return 0;
}

static int app_rx_video_rtp_ready(void* priv) {
  struct st_app_rx_video_session* s = priv;

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  return 0;
}

static int app_rx_video_detected(void* priv, const struct st20_detect_meta* meta,
                                 struct st20_detect_reply* reply) {
  struct st_app_rx_video_session* s = priv;

  if (s->slice) reply->slice_lines = meta->height / 32;
  if (s->user_pg.fmt != USER_FMT_MAX) {
    int ret = user_get_pgroup(s->user_pg.fmt, &s->user_pg);
    if (ret < 0) return ret;
    reply->uframe_size =
        meta->width * meta->height * s->user_pg.size / s->user_pg.coverage;
  }

  return 0;
}

static int app_rx_video_uinit(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  st_app_uinit_display(s->display);
  if (s->display) {
    st_app_free(s->display);
  }

  s->st20_app_thread_stop = true;
  if (s->st20_app_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st20_wake_mutex);
    st_pthread_cond_signal(&s->st20_wake_cond);
    st_pthread_mutex_unlock(&s->st20_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st20_app_thread, NULL);
  }

  st_pthread_mutex_destroy(&s->st20_wake_mutex);
  st_pthread_cond_destroy(&s->st20_wake_cond);

  if (s->handle) {
    ret = st20_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st20_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  app_rx_video_close_source(s);
  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }

  return 0;
}

static int app_rx_video_init(struct st_app_context* ctx, st_json_video_session_t* video,
                             struct st_app_rx_video_session* s) {
  int idx = s->idx, ret;
  struct st20_rx_ops ops;
  char name[32];
  st20_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_video_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = video ? video->base.num_inf : ctx->para.num_ports;
  memcpy(ops.sip_addr[MTL_PORT_P],
         video ? video->base.ip[MTL_PORT_P] : ctx->rx_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  strncpy(ops.port[MTL_PORT_P],
          video ? video->base.inf[MTL_PORT_P]->name : ctx->para.port[MTL_PORT_P],
          MTL_PORT_MAX_LEN);
  ops.udp_port[MTL_PORT_P] = video ? video->base.udp_port : (10000 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.sip_addr[MTL_PORT_R],
           video ? video->base.ip[MTL_PORT_R] : ctx->rx_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    strncpy(ops.port[MTL_PORT_R],
            video ? video->base.inf[MTL_PORT_R]->name : ctx->para.port[MTL_PORT_R],
            MTL_PORT_MAX_LEN);
    ops.udp_port[MTL_PORT_R] = video ? video->base.udp_port : (10000 + s->idx);
  }
  ops.pacing = ST21_PACING_NARROW;
  if (ctx->rx_video_rtp_ring_size > 0)
    ops.type = ST20_TYPE_RTP_LEVEL;
  else
    ops.type = video ? video->info.type : ST20_TYPE_FRAME_LEVEL;
  ops.flags = ST20_RX_FLAG_DMA_OFFLOAD;
  if (video && video->info.video_format == VIDEO_FORMAT_AUTO) {
    ops.flags |= ST20_RX_FLAG_AUTO_DETECT;
    ops.width = 1920;
    ops.height = 1080;
    ops.fps = ST_FPS_P59_94;
  } else {
    ops.width = video ? st_app_get_width(video->info.video_format) : 1920;
    ops.height = video ? st_app_get_height(video->info.video_format) : 1080;
    ops.fps = video ? st_app_get_fps(video->info.video_format) : ST_FPS_P59_94;
  }
  ops.fmt = video ? video->info.pg_format : ST20_FMT_YUV_422_10BIT;
  ops.payload_type = video ? video->base.payload_type : ST_APP_PAYLOAD_TYPE_VIDEO;
  ops.interlaced = video ? st_app_get_interlaced(video->info.video_format) : false;
  ops.notify_frame_ready = app_rx_video_frame_ready;
  ops.slice_lines = ops.height / 32;
  ops.notify_slice_ready = app_rx_video_slice_ready;
  ops.notify_rtp_ready = app_rx_video_rtp_ready;
  ops.notify_detected = app_rx_video_detected;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.rtp_ring_size = ctx->rx_video_rtp_ring_size;
  if (!ops.rtp_ring_size) ops.rtp_ring_size = 1024;
  if (ops.type == ST20_TYPE_SLICE_LEVEL) {
    ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    s->slice = true;
  } else {
    s->slice = false;
  }
  if (ctx->enable_hdr_split) ops.flags |= ST20_RX_FLAG_HDR_SPLIT;

  st_pthread_mutex_init(&s->st20_wake_mutex, NULL);
  st_pthread_cond_init(&s->st20_wake_cond, NULL);

  if (mtl_pmd_by_port_name(ops.port[MTL_PORT_P]) == MTL_PMD_DPDK_AF_XDP) {
    snprintf(s->st20_dst_url, ST_APP_URL_MAX_LEN, "st_app%d_%d_%d_%s.yuv", idx, ops.width,
             ops.height, ops.port[MTL_PORT_P]);
  } else {
    uint32_t soc = 0, b = 0, d = 0, f = 0;
    sscanf(ops.port[MTL_PORT_P], "%x:%x:%x.%x", &soc, &b, &d, &f);
    snprintf(s->st20_dst_url, ST_APP_URL_MAX_LEN,
             "st_app%d_%d_%d_%02x_%02x_%02x-%02x.yuv", idx, ops.width, ops.height, soc, b,
             d, f);
  }
  ret = st20_get_pgroup(ops.fmt, &s->st20_pg);
  if (ret < 0) return ret;
  s->user_pg.fmt = video ? video->user_pg_format : USER_FMT_MAX;
  if (s->user_pg.fmt != USER_FMT_MAX) {
    ret = user_get_pgroup(s->user_pg.fmt, &s->user_pg);
    if (ret < 0) return ret;
    ops.uframe_size = ops.width * ops.height * s->user_pg.size / s->user_pg.coverage;
    ops.uframe_pg_callback = pg_convert_callback;
  }

  s->width = ops.width;
  s->height = ops.height;
  if (ops.interlaced) {
    s->height >>= 1;
  }
  s->expect_fps = st_frame_rate(ops.fps);
  s->pcapng_max_pkts = ctx->pcapng_max_pkts;

  s->framebuff_producer_idx = 0;
  s->framebuff_consumer_idx = 0;
  s->framebuffs =
      (struct st_rx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) return -ENOMEM;
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].frame = NULL;
  }

  if (ctx->has_sdl && video && video->display) {
    struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
    ret = st_app_init_display(d, name, s->width, s->height, ctx->ttf_file);
    if (ret < 0) {
      err("%s(%d), st_app_init_display fail %d\n", __func__, idx, ret);
      app_rx_video_uinit(s);
      return -EIO;
    }
    s->display = d;
  }

  s->measure_latency = video ? video->measure_latency : true;

  handle = st20_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_rx_create fail\n", __func__, idx);
    app_rx_video_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st20_frame_size = st20_rx_get_framebuffer_size(handle);

  ret = app_rx_video_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_rx_video_open_source fail %d\n", __func__, idx, ret);
    app_rx_video_uinit(s);
    return -EIO;
  }

  if (app_rx_video_is_frame_type(ops.type)) {
    ret = app_rx_video_init_frame_thread(s);
  } else if (ops.type == ST20_TYPE_RTP_LEVEL) {
    ret = app_rx_video_init_rtp_thread(s);
  } else {
    ret = -EINVAL;
  }
  if (ret < 0) {
    err("%s(%d), app_rx_video_init_thread fail %d, type %d\n", __func__, idx, ret,
        ops.type);
    app_rx_video_uinit(s);
    return -EIO;
  }

  s->stat_frame_received = 0;
  s->stat_last_time = st_app_get_monotonic_time();

  return 0;
}

static int app_rx_video_stat(struct st_app_rx_video_session* s) {
  uint64_t cur_time_ns = st_app_get_monotonic_time();
#ifdef DEBUG
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = s->stat_frame_received / time_sec;
  dbg("%s(%d), fps %f, %d frame received\n", __func__, s->idx, framerate,
      s->stat_frame_received);
#endif
  if (s->measure_latency && s->stat_frame_received) {
    double latency_ms = (double)s->stat_latency_us_sum / s->stat_frame_received / 1000;
    info("%s(%d), avrage latency %fms\n", __func__, s->idx, latency_ms);
    s->stat_latency_us_sum = 0;
  }
  s->stat_frame_received = 0;
  s->stat_last_time = cur_time_ns;

  return 0;
}

static int app_rx_video_result(struct st_app_rx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_frist_rx_time) / NS_PER_S;
  double framerate = s->stat_frame_total_received / time_sec;

  if (!s->stat_frame_total_received) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frame received\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->stat_frame_total_received);
  return 0;
}

static int app_rx_video_pcap(struct st_app_rx_video_session* s) {
  if (s->pcapng_max_pkts) st20_rx_pcapng_dump(s->handle, s->pcapng_max_pkts, false, NULL);
  return 0;
}

int st_app_rx_video_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_video_session* s;
  int fb_cnt = ctx->rx_video_fb_cnt;
  if (fb_cnt <= 0) fb_cnt = 6;

  ctx->rx_video_sessions = (struct st_app_rx_video_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_video_session) * ctx->rx_video_session_cnt);
  if (!ctx->rx_video_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    s->idx = i;
    s->st = ctx->st;
    s->framebuff_cnt = fb_cnt;
    s->st20_dst_fb_cnt = ctx->rx_video_file_frames;
    s->st20_dst_fd = -1;

    ret = app_rx_video_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_video_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_video_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_video_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_video_session* s;
  if (!ctx->rx_video_sessions) return 0;
  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    app_rx_video_uinit(s);
  }
  st_app_free(ctx->rx_video_sessions);

  return 0;
}

int st_app_rx_video_sessions_stat(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_video_session* s;
  if (!ctx->rx_video_sessions) return 0;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    app_rx_video_stat(s);
  }

  return 0;
}

int st_app_rx_video_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_video_session* s;

  if (!ctx->rx_video_sessions) return 0;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    ret += app_rx_video_result(s);
  }

  return ret;
}

int st_app_rx_video_sessions_pcap(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_video_session* s;

  if (!ctx->rx_video_sessions) return 0;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    app_rx_video_pcap(s);
  }

  return 0;
}
