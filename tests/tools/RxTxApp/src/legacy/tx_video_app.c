/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_video_app.h"

static int app_tx_video_notify_event(void* priv, enum st_event event, void* args) {
  struct st_app_tx_video_session* s = priv;
  if (event == ST_EVENT_VSYNC) {
    struct st10_vsync_meta* meta = args;
    info("%s(%d), epoch %" PRIu64 "\n", __func__, s->idx, meta->epoch);
  } else if (event == ST_EVENT_FATAL_ERROR) {
    err("%s(%d), ST_EVENT_FATAL_ERROR\n", __func__, s->idx);
    /* add a exist routine */
  } else if (event == ST_EVENT_RECOVERY_ERROR) {
    info("%s(%d), ST_EVENT_RECOVERY_ERROR\n", __func__, s->idx);
  }
  return 0;
}

static void app_tx_video_display_frame(struct st_app_tx_video_session* s, void* frame) {
  struct st_display* d = s->display;

  if (d && d->front_frame) {
    if (st_pthread_mutex_trylock(&d->display_frame_mutex) == 0) {
      if (s->st20_pg.fmt == ST20_FMT_YUV_422_8BIT)
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
  }
}

static int app_tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  struct st_app_tx_video_session* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, consumer_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, ST10_VIDEO_SAMPLING_RATE_90K));
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    meta->second_field = framebuff->second_field;
    if (s->sha_check) {
      meta->user_meta = framebuff->shas;
      meta->user_meta_size = sizeof(framebuff->shas);
    }
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
    dbg("%s(%d), idx %u err stat %d\n", __func__, s->idx, consumer_idx, framebuff->stat);
  }
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  return ret;
}

static int app_tx_video_frame_done(void* priv, uint16_t frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  struct st_app_tx_video_session* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    dbg("%s(%d), done frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, frame_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, ST10_VIDEO_SAMPLING_RATE_90K));
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
        frame_idx);
  }
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  s->st20_frame_done_cnt++;
  if (!s->stat_frame_first_tx_time)
    s->stat_frame_first_tx_time = st_app_get_monotonic_time();

  return ret;
}

static int app_tx_video_frame_lines_ready(void* priv, uint16_t frame_idx,
                                          struct st20_tx_slice_meta* meta) {
  struct st_app_tx_video_session* s = priv;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  framebuff->slice_trigger = true;
  meta->lines_ready = framebuff->lines_ready;
  dbg("%s(%d), frame %u ready %d lines\n", __func__, s->idx, frame_idx,
      framebuff->lines_ready);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  return 0;
}

static int app_tx_video_rtp_done(void* priv) {
  struct st_app_tx_video_session* s = priv;

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);
  s->st20_packet_done_cnt++;
  return 0;
}

static void app_tx_video_thread_bind(struct st_app_tx_video_session* s) {
  if (s->lcore != -1) {
    mtl_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }
}

static void app_tx_video_check_lcore(struct st_app_tx_video_session* s, bool rtp) {
  int sch_idx = st20_tx_get_sch_idx(s->handle);

  if (s->ctx->app_bind_lcore && (s->handle_sch_idx != sch_idx)) {
    s->handle_sch_idx = sch_idx;
    unsigned int lcore;
    int ret = st_app_video_get_lcore(s->ctx, s->handle_sch_idx, rtp, &lcore);
    if ((ret >= 0) && (lcore != s->lcore)) {
      s->lcore = lcore;
      app_tx_video_thread_bind(s);
      info("%s(%d), bind to new lcore %d\n", __func__, s->idx, lcore);
    }
  }
}

static void app_tx_video_build_frame(struct st_app_tx_video_session* s, void* frame,
                                     size_t frame_size) {
  uint8_t* src = s->st20_frame_cursor;

  if (!s->ctx->tx_copy_once || !s->st20_frames_copied) {
    mtl_memcpy(frame, src, frame_size);
  }
  /* point to next frame */
  s->st20_frame_cursor += frame_size;
  if (s->st20_frame_cursor + frame_size > s->st20_source_end) {
    s->st20_frame_cursor = s->st20_source_begin;
    s->st20_frames_copied = true;
  }

  app_tx_video_display_frame(s, frame);
}

static void app_tx_video_build_slice(struct st_app_tx_video_session* s,
                                     struct st_tx_frame* framebuff, void* frame_addr) {
  int lines_build = 0;
  int bytes_per_slice = framebuff->size / s->height * s->lines_per_slice;
  int frame_size = framebuff->size;

  if (s->st20_frame_cursor + frame_size > s->st20_source_end) {
    s->st20_frame_cursor = s->st20_source_begin;
  }
  uint8_t* src = s->st20_frame_cursor;
  uint8_t* dst = frame_addr;
  /* point to next frame */
  s->st20_frame_cursor += frame_size;

  /* simulate the timing */
  while (!framebuff->slice_trigger) {
    st_usleep(1);
  }

  mtl_memcpy(dst, src, bytes_per_slice);
  dst += bytes_per_slice;
  src += bytes_per_slice;
  lines_build += s->lines_per_slice;

  st_pthread_mutex_lock(&s->st20_wake_mutex);
  framebuff->lines_ready = lines_build;
  st_pthread_mutex_unlock(&s->st20_wake_mutex);

  while (lines_build < s->height) {
    int lines = s->lines_per_slice;
    if ((lines_build + lines) > s->height) lines = s->height - lines_build;
    int bytes_slice = framebuff->size / s->height * lines;

    lines_build += lines;
    mtl_memcpy(dst, src, bytes_slice);
    dst += bytes_slice;
    src += bytes_slice;

    st_pthread_mutex_lock(&s->st20_wake_mutex);
    framebuff->lines_ready = lines_build;
    st_pthread_mutex_unlock(&s->st20_wake_mutex);
  }
}

static void* app_tx_video_frame_thread(void* arg) {
  struct st_app_tx_video_session* s = arg;
  int idx = s->idx;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  app_tx_video_thread_bind(s);

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    st_pthread_mutex_lock(&s->st20_wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->st20_app_thread_stop)
        st_pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
      st_pthread_mutex_unlock(&s->st20_wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->st20_wake_mutex);

    app_tx_video_check_lcore(s, false);

    void* frame_addr = st20_tx_get_framebuffer(s->handle, producer_idx);
    if (!s->slice) {
      /* interlaced use different layout? */
      app_tx_video_build_frame(s, frame_addr, s->st20_frame_size);
    }
    if (s->sha_check) {
      st_sha256((unsigned char*)frame_addr, s->st20_frame_size, framebuff->shas);
      // st_sha_dump("frame sha:", framebuff->shas);
    }

    st_pthread_mutex_lock(&s->st20_wake_mutex);
    framebuff->size = s->st20_frame_size;
    framebuff->second_field = s->second_field;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    if (s->interlaced) {
      s->second_field = !s->second_field;
    }
    st_pthread_mutex_unlock(&s->st20_wake_mutex);

    if (s->slice) {
      app_tx_video_build_slice(s, framebuff, frame_addr);
    }
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void* app_tx_video_pcap_thread(void* arg) {
  struct st_app_tx_video_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  struct pcap_pkthdr hdr;
  uint8_t* packet;
  struct ether_header* eth_hdr;
  struct ip* ip_hdr;
  struct udphdr* udp_hdr;
  uint16_t udp_data_len;

  app_tx_video_thread_bind(s);

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&s->st20_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st20_wake_mutex);
      } else {
        if (!s->st20_app_thread_stop)
          st_pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
        st_pthread_mutex_unlock(&s->st20_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st20_pcap, &hdr);
    dbg("%s(%d), packet %p\n", __func__, idx, packet);
    if (packet) {
      eth_hdr = (struct ether_header*)packet;
      if (ntohs(eth_hdr->ether_type) == ETHERTYPE_IP) {
        ip_hdr = (struct ip*)(packet + sizeof(struct ether_header));
        if (ip_hdr->ip_p == IPPROTO_UDP) {
          udp_hdr =
              (struct udphdr*)(packet + sizeof(struct ether_header) + sizeof(struct ip));
          udp_data_len = ntohs(udp_hdr->len) - sizeof(struct udphdr);
          dbg("%s(%d), packet %p udp_data_len %u\n", __func__, idx, packet, udp_data_len);
          mtl_memcpy(usrptr,
                     packet + sizeof(struct ether_header) + sizeof(struct ip) +
                         sizeof(struct udphdr),
                     udp_data_len);
        }
      }
    } else {
      char err_buf[PCAP_ERRBUF_SIZE];
      pcap_close(s->st20_pcap);
      /* open capture file for offline processing */
      s->st20_pcap = pcap_open_offline(s->st20_source_url, err_buf);
      if (s->st20_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st20_source_url, err_buf);
        return NULL;
      }
    }

    struct st_rfc3550_rtp_hdr* hdr = (struct st_rfc3550_rtp_hdr*)usrptr;
    if (hdr->payload_type != s->payload_type) {
      udp_data_len = 0;
      err("%s(%d), expect payload_type %u but pcap is %u, please correct the "
          "payload_type in json\n",
          __func__, idx, s->payload_type, hdr->payload_type);
    }

    st20_tx_put_mbuf(s->handle, mbuf, udp_data_len);

    app_tx_video_check_lcore(s, true);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_video_init_rtp(struct st_app_tx_video_session* s,
                                 struct st20_tx_ops* ops) {
  int idx = s->idx;
  struct st20_rfc4175_rtp_hdr* rtp = &s->st20_rtp_base;

  /* 4800 if 1080p yuv422 */
  /* Calculate bytes per line, rounding up if there's a remainder */
  size_t raw_bytes_size = (size_t)ops->width * s->st20_pg.size;
  s->st20_bytes_in_line =
      (raw_bytes_size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
  s->st20_pkt_idx = 0;
  s->st20_seq_id = 1;
  int height = ops->height;
  if (ops->interlaced) height = height >> 1;

  if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line for rtp */
    size_t bytes_in_pkt = MTL_PKT_MAX_RTP_BYTES - sizeof(*rtp);
    s->st20_pkts_in_line = (s->st20_bytes_in_line / bytes_in_pkt) + 1;
    s->st20_total_pkts = height * s->st20_pkts_in_line;
    int pixels_in_pkts = (ops->width + s->st20_pkts_in_line - 1) / s->st20_pkts_in_line;
    s->st20_pkt_data_len = (pixels_in_pkts + s->st20_pg.coverage - 1) /
                           s->st20_pg.coverage * s->st20_pg.size;
    info("%s(%d), %d pkts(%d) in line\n", __func__, idx, s->st20_pkts_in_line,
         s->st20_pkt_data_len);
  } else if (ops->packing == ST20_PACKING_BPM) {
    s->st20_pkt_data_len = 1260;
    int pixels_in_pkts = s->st20_pkt_data_len * s->st20_pg.coverage / s->st20_pg.size;
    s->st20_total_pkts = ceil((double)ops->width * height / pixels_in_pkts);
    info("%s(%d), %d pkts(%d) in frame\n", __func__, idx, s->st20_total_pkts,
         s->st20_pkt_data_len);
  } else if (ops->packing == ST20_PACKING_GPM) {
    int max_data_len =
        MTL_PKT_MAX_RTP_BYTES - sizeof(*rtp) - sizeof(struct st20_rfc4175_extra_rtp_hdr);
    int pg_per_pkt = max_data_len / s->st20_pg.size;
    s->st20_total_pkts =
        (ceil)((double)ops->width * height / (s->st20_pg.coverage * pg_per_pkt));
    s->st20_pkt_data_len = pg_per_pkt * s->st20_pg.size;
  } else {
    err("%s(%d), invalid packing mode: %d\n", __func__, idx, ops->packing);
    return -EIO;
  }

  ops->rtp_frame_total_pkts = s->st20_total_pkts;
  if (s->st20_pcap_input)
    ops->rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES;
  else {
    ops->rtp_pkt_size = s->st20_pkt_data_len + sizeof(*rtp);
    if (ops->packing != ST20_PACKING_GPM_SL) /* no extra for GPM_SL */
      ops->rtp_pkt_size += sizeof(struct st20_rfc4175_extra_rtp_hdr);
  }

  memset(rtp, 0, sizeof(*rtp));
  rtp->base.version = 2;
  rtp->base.payload_type = ST_APP_PAYLOAD_TYPE_VIDEO;
  rtp->base.ssrc = htonl(s->idx + 0x423450);
  rtp->row_length = htons(s->st20_pkt_data_len);
  return 0;
}

static int app_tx_video_build_rtp_packet(struct st_app_tx_video_session* s,
                                         struct st20_rfc4175_rtp_hdr* rtp,
                                         uint16_t* pkt_len) {
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  uint32_t offset;
  uint16_t row_number, row_offset;
  uint8_t* frame = s->st20_frame_cursor;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);

  if (s->single_line) {
    row_number = s->st20_pkt_idx / s->st20_pkts_in_line;
    int pixels_in_pkt = s->st20_pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
    row_offset = pixels_in_pkt * (s->st20_pkt_idx % s->st20_pkts_in_line);
    offset = (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->st20_pkt_data_len * s->st20_pkt_idx;
    row_number = offset / s->st20_bytes_in_line;
    row_offset = (offset % s->st20_bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->st20_pkt_data_len > (row_number + 1) * s->st20_bytes_in_line) &&
        (offset + s->st20_pkt_data_len < s->st20_frame_size)) {
      e_rtp = (struct st20_rfc4175_extra_rtp_hdr*)payload;
      payload += sizeof(*e_rtp);
    }
  }

  /* copy base hdr */
  mtl_memcpy(rtp, &s->st20_rtp_base, sizeof(*rtp));
  /* update hdr */
  if (s->st20_second_field)
    rtp->row_number = htons(row_number | ST20_SECOND_FIELD);
  else
    rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->st20_rtp_tmstamp);
  rtp->base.seq_number = htons(s->st20_seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->st20_seq_id >> 16));
  s->st20_seq_id++;

  int temp = s->single_line
                 ? ((s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size)
                 : (s->st20_frame_size - offset);
  uint16_t data_len = s->st20_pkt_data_len > temp ? temp : s->st20_pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);
  if (e_rtp) {
    uint16_t row_length_0 = (row_number + 1) * s->st20_bytes_in_line - offset;
    uint16_t row_length_1 = s->st20_pkt_data_len - row_length_0;
    rtp->row_length = htons(row_length_0);
    e_rtp->row_length = htons(row_length_1);
    e_rtp->row_offset = htons(0);
    if (!s->st20_second_field)
      e_rtp->row_number = htons(row_number + 1);
    else
      e_rtp->row_number = htons((row_number + 1) | ST20_SECOND_FIELD);

    rtp->row_offset = htons(row_offset | ST20_SRD_OFFSET_CONTINUATION);
    *pkt_len += sizeof(*e_rtp);
  }

  if (!s->interlaced)
    mtl_memcpy(payload, frame + offset, data_len);
  else {
    if (s->st20_second_field)
      mtl_memcpy(payload,
                 frame + ((2 * row_number + 1) * s->width + row_offset) /
                             s->st20_pg.coverage * s->st20_pg.size,
                 data_len);
    else
      mtl_memcpy(payload,
                 frame + (2 * row_number * s->width + row_offset) / s->st20_pg.coverage *
                             s->st20_pg.size,
                 data_len);
  }

  s->st20_pkt_idx++;
  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, s->idx, s->st21_frame_idx);
    /* end of current frame */
    rtp->base.marker = 1;

    s->st20_pkt_idx = 0;
    s->st20_rtp_tmstamp++;
    s->st20_frame_done_cnt++;
    if (!s->stat_frame_first_tx_time)
      s->stat_frame_first_tx_time = st_app_get_monotonic_time();
    int frame_size = s->interlaced ? s->st20_frame_size * 2 : s->st20_frame_size;
    if (!s->interlaced) {
      s->st20_frame_cursor += frame_size;
    } else {
      if (s->st20_second_field) s->st20_frame_cursor += frame_size;
      s->st20_second_field = !s->st20_second_field;
    }
    if ((s->st20_frame_cursor + frame_size) > s->st20_source_end)
      s->st20_frame_cursor = s->st20_source_begin;
  }

  return 0;
}

static void* app_tx_video_rtp_thread(void* arg) {
  struct st_app_tx_video_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;

  app_tx_video_thread_bind(s);

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&s->st20_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st20_wake_mutex);
      } else {
        if (!s->st20_app_thread_stop)
          st_pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
        st_pthread_mutex_unlock(&s->st20_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_video_build_rtp_packet(s, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf(s->handle, mbuf, mbuf_len);

    app_tx_video_check_lcore(s, true);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_video_open_source(struct st_app_tx_video_session* s) {
  if (!s->st20_pcap_input) {
    int fd;
    struct stat i;

    fd = st_open(s->st20_source_url, O_RDONLY);
    if (fd < 0) {
      err("%s, open fail '%s'\n", __func__, s->st20_source_url);
      return -EIO;
    }

    if (fstat(fd, &i) < 0) {
      err("%s, fstat %s fail\n", __func__, s->st20_source_url);
      close(fd);
      return -EIO;
    }
    if (i.st_size < s->st20_frame_size) {
      err("%s, %s file size small then a frame %d\n", __func__, s->st20_source_url,
          s->st20_frame_size);
      close(fd);
      return -EIO;
    }

    uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (MAP_FAILED == m) {
      err("%s, mmap fail '%s'\n", __func__, s->st20_source_url);
      close(fd);
      return -EIO;
    }

    s->st20_source_begin = mtl_hp_malloc(s->st, i.st_size, MTL_PORT_P);
    if (!s->st20_source_begin) {
      warn("%s, source malloc on hugepage fail\n", __func__);
      s->st20_source_begin = m;
      s->st20_frame_cursor = m;
      s->st20_source_end = m + i.st_size;
      s->st20_source_fd = fd;
    } else {
      s->st20_frame_cursor = s->st20_source_begin;
      mtl_memcpy(s->st20_source_begin, m, i.st_size);
      s->st20_source_end = s->st20_source_begin + i.st_size;
      close(fd);
    }
  } else {
    char err_buf[PCAP_ERRBUF_SIZE];

    /* open capture file for offline processing */
    s->st20_pcap = pcap_open_offline(s->st20_source_url, err_buf);
    if (!s->st20_pcap) {
      err("pcap_open_offline %s() failed: %s\n:", s->st20_source_url, err_buf);
      return -EIO;
    }
  }

  return 0;
}

static int app_tx_video_start_source(struct st_app_tx_video_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  if (s->st20_pcap_input)
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_pcap_thread, s);
  else if (s->st20_rtp_input)
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_rtp_thread, s);
  else
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st20_app_thread create fail err = %d\n", __func__, idx, ret);
    return ret;
  }
  s->st20_app_thread_stop = false;

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_video_%d", idx);
  mtl_thread_setname(s->st20_app_thread, thread_name);

  return 0;
}

static void app_tx_video_stop_source(struct st_app_tx_video_session* s) {
  s->st20_app_thread_stop = true;
  /* wake up the thread */
  st_pthread_mutex_lock(&s->st20_wake_mutex);
  st_pthread_cond_signal(&s->st20_wake_cond);
  st_pthread_mutex_unlock(&s->st20_wake_mutex);
  if (s->st20_app_thread) {
    pthread_join(s->st20_app_thread, NULL);
    s->st20_app_thread = 0;
  }
}

static int app_tx_video_close_source(struct st_app_tx_video_session* s) {
  if (s->st20_source_fd < 0 && s->st20_source_begin) {
    mtl_hp_free(s->st, s->st20_source_begin);
    s->st20_source_begin = NULL;
  }
  if (s->st20_source_fd >= 0) {
    munmap(s->st20_source_begin, s->st20_source_end - s->st20_source_begin);
    close(s->st20_source_fd);
    s->st20_source_fd = -1;
  }
  if (s->st20_pcap) {
    pcap_close(s->st20_pcap);
    s->st20_pcap = NULL;
  }

  return 0;
}

static int app_tx_video_handle_free(struct st_app_tx_video_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st20_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st20_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_video_uinit(struct st_app_tx_video_session* s) {
  app_tx_video_stop_source(s);
  app_tx_video_handle_free(s);
  app_tx_video_close_source(s);

  st_app_uinit_display(s->display);
  if (s->display) {
    st_app_free(s->display);
  }

  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }
  st_pthread_mutex_destroy(&s->st20_wake_mutex);
  st_pthread_cond_destroy(&s->st20_wake_cond);

  return 0;
}

static int app_tx_video_result(struct st_app_tx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_tx_time) / NS_PER_S;
  double framerate = s->st20_frame_done_cnt / time_sec;

  if (!s->st20_frame_done_cnt) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frames send\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->st20_frame_done_cnt);
  return 0;
}

static int app_tx_video_io_stat(struct st_app_tx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time - s->last_stat_time_ns) / NS_PER_S;
  double tx_rate_m, fps;
  int ret;
  struct st20_tx_user_stats stats;

  if (!s->handle) return 0;

  for (uint8_t port = 0; port < s->num_port; port++) {
    ret = st20_tx_get_session_stats(s->handle, &stats);
    if (ret < 0) return ret;
    tx_rate_m = (double)stats.common.port[port].bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    fps = (double)stats.common.port[port].frames / time_sec;

    info("%s(%d,%u), tx %f Mb/s fps %f\n", __func__, idx, port, tx_rate_m, fps);
  }
  st20_tx_reset_session_stats(s->handle);

  s->last_stat_time_ns = cur_time;
  return 0;
}

static int app_tx_video_init(struct st_app_context* ctx, st_json_video_session_t* video,
                             struct st_app_tx_video_session* s) {
  int idx = s->idx, ret;
  struct st20_tx_ops ops;
  char name[32];
  st20_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->enable_vsync = false;
  s->last_stat_time_ns = st_app_get_monotonic_time();
  s->sha_check = ctx->video_sha_check;

  snprintf(name, 32, "app_tx_video_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = video ? video->base.num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P],
         video ? st_json_ip(ctx, &video->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(
      ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      video ? video->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = video ? video->base.udp_port : (10000 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST20_TX_FLAG_USER_P_MAC;
  }
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R],
           video ? st_json_ip(ctx, &video->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(
        ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        video ? video->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = video ? video->base.udp_port : (10000 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST20_TX_FLAG_USER_R_MAC;
    }
  }
  ops.pacing = video ? video->info.pacing : ST21_PACING_NARROW;
  if (ctx->tx_pacing_type) /* override if args has pacing defined */
    ops.pacing = ctx->tx_pacing_type;
  ops.packing = video ? video->info.packing : ST20_PACKING_BPM;
  ops.type = video ? video->info.type : ST20_TYPE_FRAME_LEVEL;
  ops.width = video ? st_app_get_width(video->info.video_format) : 1920;
  ops.height = video ? st_app_get_height(video->info.video_format) : 1080;
  ops.fps = video ? st_app_get_fps(video->info.video_format) : ST_FPS_P59_94;
  ops.fmt = video ? video->info.pg_format : ST20_FMT_YUV_422_10BIT;
  ops.interlaced = video ? st_app_get_interlaced(video->info.video_format) : false;
  ops.get_next_frame = app_tx_video_next_frame;
  ops.notify_frame_done = app_tx_video_frame_done;
  ops.query_frame_lines_ready = app_tx_video_frame_lines_ready;
  ops.notify_rtp_done = app_tx_video_rtp_done;
  ops.notify_event = app_tx_video_notify_event;
  ops.framebuff_cnt = 2;
  ops.payload_type = video ? video->base.payload_type : ST_APP_PAYLOAD_TYPE_VIDEO;
  ops.start_vrx = ctx->tx_start_vrx;
  ops.pad_interval = ctx->tx_pad_interval;
  ops.rtp_timestamp_delta_us = ctx->tx_ts_delta_us;
  if (s->enable_vsync) ops.flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  if (ctx->tx_static_pad) ops.flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;
  if (ctx->tx_no_bulk) ops.flags |= ST20_TX_FLAG_DISABLE_BULK;
  if (ctx->force_tx_video_numa >= 0) {
    ops.flags |= ST20_TX_FLAG_FORCE_NUMA;
    ops.socket_id = ctx->force_tx_video_numa;
  }

  if (video && video->enable_rtcp) {
    ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
    ops.rtcp.buffer_size = 1024;
  }

  ret = st20_get_pgroup(ops.fmt, &s->st20_pg);
  if (ret < 0) return ret;
  s->width = ops.width;
  s->height = ops.height;
  if (ops.interlaced) {
    s->height >>= 1;
  }
  s->interlaced = ops.interlaced ? true : false;
  s->num_port = ops.num_port;
  memcpy(s->st20_source_url, video ? video->info.video_url : ctx->tx_video_url,
         ST_APP_URL_MAX_LEN);
  s->st20_pcap_input = false;
  s->st20_rtp_input = false;
  s->st = ctx->st;
  s->single_line = (ops.packing == ST20_PACKING_GPM_SL);
  s->slice = (ops.type == ST20_TYPE_SLICE_LEVEL);
  s->expect_fps = st_frame_rate(ops.fps);
  s->payload_type = ops.payload_type;

  s->framebuff_cnt = ops.framebuff_cnt;
  s->lines_per_slice = ops.height / 30;
  s->st20_source_fd = -1;

  s->framebuffs =
      (struct st_tx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) {
    return -ENOMEM;
  }
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].stat = ST_TX_FRAME_FREE;
    s->framebuffs[j].lines_ready = 0;
  }

  st_pthread_mutex_init(&s->st20_wake_mutex, NULL);
  st_pthread_cond_init(&s->st20_wake_cond, NULL);

  /* select rtp type for pcap file or tx_video_rtp_ring_size */
  if (strstr(s->st20_source_url, ".pcap")) {
    ops.type = ST20_TYPE_RTP_LEVEL;
    s->st20_pcap_input = true;
  } else if (ctx->tx_video_rtp_ring_size > 0) {
    ops.type = ST20_TYPE_RTP_LEVEL;
    s->st20_rtp_input = true;
  }
  if (ops.type == ST20_TYPE_RTP_LEVEL) {
    s->st20_rtp_input = true;
    if (ctx->tx_video_rtp_ring_size > 0)
      ops.rtp_ring_size = ctx->tx_video_rtp_ring_size;
    else
      ops.rtp_ring_size = 1024;
    app_tx_video_init_rtp(s, &ops);
  }

  handle = st20_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_tx_create fail\n", __func__, idx);
    app_tx_video_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->st20_frame_size = st20_tx_get_framebuffer_size(handle);
  s->handle_sch_idx = st20_tx_get_sch_idx(handle);
  unsigned int lcore;
  bool rtp = false;
  if (ops.type == ST20_TYPE_RTP_LEVEL) rtp = true;

  if (ctx->app_bind_lcore) {
    ret = st_app_video_get_lcore(ctx, s->handle_sch_idx, rtp, &lcore);
    if (ret >= 0) s->lcore = lcore;
  }

  ret = app_tx_video_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_video_open_source fail %d\n", __func__, idx, ret);
    app_tx_video_uinit(s);
    return ret;
  }
  ret = app_tx_video_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_video_start_source fail %d\n", __func__, idx, ret);
    app_tx_video_uinit(s);
    return ret;
  }

  if ((video && video->display) || ctx->tx_display) {
    struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
    ret = st_app_init_display(d, name, s->width, s->height, ctx->ttf_file);
    if (ret < 0) {
      err("%s(%d), st_app_init_display fail %d\n", __func__, idx, ret);
      app_tx_video_uinit(s);
      return -EIO;
    }
    s->display = d;
  }

  return 0;
}

int st_app_tx_video_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_video_session* s;
  ctx->tx_video_sessions = (struct st_app_tx_video_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_video_session) * ctx->tx_video_session_cnt);
  if (!ctx->tx_video_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    s->idx = i;
    s->lcore = -1;
    ret = app_tx_video_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_video_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_video_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_video_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_video_session* s;
  if (!ctx->tx_video_sessions) return 0;
  for (int i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    app_tx_video_stop_source(s);
  }

  return 0;
}

int st_app_tx_video_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_video_session* s;
  if (!ctx->tx_video_sessions) return 0;
  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    app_tx_video_uinit(s);
  }
  st_app_free(ctx->tx_video_sessions);

  return 0;
}

int st_app_tx_video_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_tx_video_session* s;
  if (!ctx->tx_video_sessions) return 0;

  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    ret += app_tx_video_result(s);
  }

  return ret;
}

int st_app_tx_videos_io_stat(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_tx_video_session* s;
  if (!ctx->tx_video_sessions) return 0;

  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    ret += app_tx_video_io_stat(s);
  }

  return ret;
}
