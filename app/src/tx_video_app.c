/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "tx_video_app.h"

static int app_tx_video_next_frame(void* priv, uint16_t* next_frame_idx) {
  struct st_app_tx_video_session* s = priv;
  int i;
  pthread_mutex_lock(&s->st21_wake_mutex);
  for (i = 0; i < s->framebuff_cnt; i++) {
    if (s->st21_ready_framebuff[i] == 1) {
      s->st21_framebuff_idx = i;
      s->st21_ready_framebuff[i] = 0;
      break;
    }
  }
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);
  if (i == s->framebuff_cnt) return -EIO;
  *next_frame_idx = s->st21_framebuff_idx;

  dbg("%s(%d), next framebuffer index %d\n", __func__, s->idx, *next_frame_idx);
  return 0;
}

static int app_tx_video_frame_done(void* priv, uint16_t frame_idx) {
  struct st_app_tx_video_session* s = priv;

  pthread_mutex_lock(&s->st21_wake_mutex);
  s->st21_free_framebuff[frame_idx] = 1;
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);
  s->st21_frame_done_cnt++;
  if (!s->stat_frame_frist_tx_time)
    s->stat_frame_frist_tx_time = st_app_get_monotonic_time();
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return 0;
}

static int app_tx_video_rtp_done(void* priv) {
  struct st_app_tx_video_session* s = priv;

  pthread_mutex_lock(&s->st21_wake_mutex);
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);
  s->st21_packet_done_cnt++;
  return 0;
}

static void* app_tx_video_frame_thread(void* arg) {
  struct st_app_tx_video_session* s = arg;
  int idx = s->idx;
  uint16_t i;
  if (s->lcore != -1) {
    st_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }

  info("%s(%d), start\n", __func__, idx);
  while (!s->st21_app_thread_stop) {
    pthread_mutex_lock(&s->st21_wake_mutex);
    // guarantee the sequence
    bool has_ready = false;
    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st21_ready_framebuff[i] == 1) {
        has_ready = true;
        break;
      }
    }
    if (has_ready) {
      if (!s->st21_app_thread_stop)
        pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
      pthread_mutex_unlock(&s->st21_wake_mutex);
      continue;
    }

    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st21_free_framebuff[i] == 1) {
        s->st21_free_framebuff[i] = 0;
        break;
      }
    }
    if (i == s->framebuff_cnt) {
      if (!s->st21_app_thread_stop)
        pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
      pthread_mutex_unlock(&s->st21_wake_mutex);
      continue;
    }
    pthread_mutex_unlock(&s->st21_wake_mutex);
    uint8_t* src = s->st21_frame_cursor;
    uint8_t* dst = st20_tx_get_framebuffer(s->handle, i);
    if (s->st21_frame_cursor + s->st21_frame_size > s->st21_source_end) {
      s->st21_frame_cursor = s->st21_source_begin;
      src = s->st21_frame_cursor;
    }
    st_memcpy(dst, src, s->st21_frame_size);
    /* point to next frame */
    s->st21_frame_cursor += s->st21_frame_size;

    pthread_mutex_lock(&s->st21_wake_mutex);
    s->st21_ready_framebuff[i] = 1;
    pthread_mutex_unlock(&s->st21_wake_mutex);
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
  if (s->lcore != -1) {
    st_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }

  info("%s(%d), start\n", __func__, idx);
  while (!s->st21_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st21_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st21_wake_mutex);
      } else {
        if (!s->st21_app_thread_stop)
          pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
        pthread_mutex_unlock(&s->st21_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st21_pcap, &hdr);
    if (packet) {
      eth_hdr = (struct ether_header*)packet;
      if (ntohs(eth_hdr->ether_type) == ETHERTYPE_IP) {
        ip_hdr = (struct ip*)(packet + sizeof(struct ether_header));
        if (ip_hdr->ip_p == IPPROTO_UDP) {
          udp_hdr =
              (struct udphdr*)(packet + sizeof(struct ether_header) + sizeof(struct ip));
          udp_data_len = ntohs(udp_hdr->uh_ulen) - sizeof(struct udphdr);
          st_memcpy(usrptr,
                    packet + sizeof(struct ether_header) + sizeof(struct ip) +
                        sizeof(struct udphdr),
                    udp_data_len);
        }
      }
    } else {
      char err_buf[PCAP_ERRBUF_SIZE];
      pcap_close(s->st21_pcap);
      /* open capture file for offline processing */
      s->st21_pcap = pcap_open_offline(s->st21_source_url, err_buf);
      if (s->st21_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st21_source_url, err_buf);
        return NULL;
      }
    }

    st20_tx_put_mbuf(s->handle, mbuf, udp_data_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_video_init_rtp(struct st_app_tx_video_session* s,
                                 struct st20_tx_ops* ops) {
  int idx = s->idx;
  struct st20_rfc4175_rtp_hdr* rtp = &s->st20_rtp_base;

  /* 4800 if 1080p yuv422 */
  s->st21_bytes_in_line = ops->width * s->st21_pg.size / s->st21_pg.coverage;
  s->st21_pkt_idx = 0;
  s->st21_seq_id = 1;

  if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line for rtp */
    size_t bytes_in_pkt = ST_PKT_MAX_RTP_BYTES - sizeof(*rtp);
    s->st21_pkts_in_line = (s->st21_bytes_in_line / bytes_in_pkt) + 1;
    s->st21_total_pkts = ops->height * s->st21_pkts_in_line;
    int pixels_in_pkts = (ops->width + s->st21_pkts_in_line - 1) / s->st21_pkts_in_line;
    s->st21_pkt_data_len = (pixels_in_pkts + s->st21_pg.coverage - 1) /
                           s->st21_pg.coverage * s->st21_pg.size;
    info("%s(%d), %d pkts(%d) in line\n", __func__, idx, s->st21_pkts_in_line,
         s->st21_pkt_data_len);
  } else if (ops->packing == ST20_PACKING_BPM) {
    s->st21_pkt_data_len = 1260;
    int pixels_in_pkts = s->st21_pkt_data_len * s->st21_pg.coverage / s->st21_pg.size;
    s->st21_total_pkts = ceil((double)ops->width * ops->height / pixels_in_pkts);
    info("%s(%d), %d pkts(%d) in frame\n", __func__, idx, s->st21_total_pkts,
         s->st21_pkt_data_len);
  } else if (ops->packing == ST20_PACKING_GPM) {
    int max_data_len =
        ST_PKT_MAX_RTP_BYTES - sizeof(*rtp) - sizeof(struct st20_rfc4175_extra_rtp_hdr);
    int pg_per_pkt = max_data_len / s->st21_pg.size;
    s->st21_total_pkts =
        (ceil)((double)ops->width * ops->height / (s->st21_pg.coverage * pg_per_pkt));
    s->st21_pkt_data_len = pg_per_pkt * s->st21_pg.size;
  } else {
    err("%s(%d), invalid packing mode: %d\n", __func__, idx, ops->packing);
    return -EIO;
  }

  /* todo: how to get the pkts info for pcap */
  ops->rtp_frame_total_pkts = s->st21_total_pkts;
  ops->rtp_pkt_size = s->st21_pkt_data_len + sizeof(*rtp);

  memset(rtp, 0, sizeof(*rtp));
  rtp->base.version = 2;
  rtp->base.payload_type = 112;
  rtp->base.ssrc = htonl(s->idx + 0x423450);
  rtp->row_length = htons(s->st21_pkt_data_len);
  return 0;
}

static int app_tx_video_build_rtp_packet(struct st_app_tx_video_session* s,
                                         struct st20_rfc4175_rtp_hdr* rtp,
                                         uint16_t* pkt_len) {
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  uint32_t offset;
  uint16_t row_number, row_offset;
  uint8_t* frame = s->st21_frame_cursor;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);

  if (s->single_line) {
    row_number = s->st21_pkt_idx / s->st21_pkts_in_line;
    int pixels_in_pkt = s->st21_pkt_data_len / s->st21_pg.size * s->st21_pg.coverage;
    row_offset = pixels_in_pkt * (s->st21_pkt_idx % s->st21_pkts_in_line);
    offset = (row_number * s->width + row_offset) / s->st21_pg.coverage * s->st21_pg.size;
  } else {
    offset = s->st21_pkt_data_len * s->st21_pkt_idx;
    row_number = offset / s->st21_bytes_in_line;
    row_offset = (offset % s->st21_bytes_in_line) * s->st21_pg.coverage / s->st21_pg.size;
    if ((offset + s->st21_pkt_data_len > (row_number + 1) * s->st21_bytes_in_line) &&
        (offset + s->st21_pkt_data_len < s->st21_frame_size)) {
      e_rtp = (struct st20_rfc4175_extra_rtp_hdr*)payload;
      payload += sizeof(*e_rtp);
    }
  }

  /* copy base hdr */
  st_memcpy(rtp, &s->st20_rtp_base, sizeof(*rtp));
  /* update hdr */
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->st21_rtp_tmstamp);
  rtp->base.seq_number = htons(s->st21_seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->st21_seq_id >> 16));
  s->st21_seq_id++;

  int temp = s->single_line
                 ? ((s->width - row_offset) / s->st21_pg.coverage * s->st21_pg.size)
                 : (s->st21_frame_size - offset);
  uint16_t data_len = s->st21_pkt_data_len > temp ? temp : s->st21_pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);
  if (e_rtp) {
    uint16_t row_length_0 = (row_number + 1) * s->st21_bytes_in_line - offset;
    uint16_t row_length_1 = s->st21_pkt_data_len - row_length_0;
    rtp->row_length = htons(row_length_0);
    e_rtp->row_length = htons(row_length_1);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons(row_number + 1);
    rtp->row_offset = htons(row_offset | 0x8000);
    *pkt_len += sizeof(*e_rtp);
  }
  /* copy payload */
  st_memcpy(payload, frame + offset, data_len);

  s->st21_pkt_idx++;
  if (s->st21_pkt_idx >= s->st21_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, s->idx, s->st21_frame_idx);
    /* end of current frame */
    rtp->base.marker = 1;

    s->st21_pkt_idx = 0;
    s->st21_rtp_tmstamp++;
    s->st21_frame_done_cnt++;
    if (!s->stat_frame_frist_tx_time)
      s->stat_frame_frist_tx_time = st_app_get_monotonic_time();

    s->st21_frame_cursor += s->st21_frame_size;
    if ((s->st21_frame_cursor + s->st21_frame_size) > s->st21_source_end)
      s->st21_frame_cursor = s->st21_source_begin;
  }

  return 0;
}

static void* app_tx_video_rtp_thread(void* arg) {
  struct st_app_tx_video_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  if (s->lcore != -1) {
    st_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }

  info("%s(%d), start\n", __func__, idx);
  while (!s->st21_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st21_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st21_wake_mutex);
      } else {
        if (!s->st21_app_thread_stop)
          pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
        pthread_mutex_unlock(&s->st21_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_video_build_rtp_packet(s, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_video_open_source(struct st_app_tx_video_session* s) {
  if (!s->st21_pcap_input) {
    int fd;
    struct stat i;

    fd = open(s->st21_source_url, O_RDONLY);
    if (fd < 0) {
      err("%s, open fail '%s'\n", __func__, s->st21_source_url);
      return -EIO;
    }

    fstat(fd, &i);
    if (i.st_size < s->st21_frame_size) {
      err("%s, %s file size small then a frame %d\n", __func__, s->st21_source_url,
          s->st21_frame_size);
      close(fd);
      return -EIO;
    }

    uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (MAP_FAILED == m) {
      err("%s, mmap fail '%s'\n", __func__, s->st21_source_url);
      close(fd);
      return -EIO;
    }

    s->st21_source_begin = m;
    s->st21_frame_cursor = m;
    s->st21_source_end = m + i.st_size;
    s->st21_source_fd = fd;
  } else {
    char err_buf[PCAP_ERRBUF_SIZE];

    /* open capture file for offline processing */
    s->st21_pcap = pcap_open_offline(s->st21_source_url, err_buf);
    if (!s->st21_pcap) {
      err("pcap_open_offline %s() failed: %s\n:", s->st21_source_url, err_buf);
      return -EIO;
    }
  }

  return 0;
}

static int app_tx_video_start_source(struct st_app_context* ctx,
                                     struct st_app_tx_video_session* s) {
  int ret = -EINVAL;

  if (s->st21_pcap_input)
    ret = pthread_create(&s->st21_app_thread, NULL, app_tx_video_pcap_thread, s);
  else if (s->st21_rtp_input)
    ret = pthread_create(&s->st21_app_thread, NULL, app_tx_video_rtp_thread, s);
  else
    ret = pthread_create(&s->st21_app_thread, NULL, app_tx_video_frame_thread, s);
  if (ret < 0) {
    err("%s, st21_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st21_app_thread_stop = false;

  return 0;
}

static void app_tx_video_stop_source(struct st_app_tx_video_session* s) {
  s->st21_app_thread_stop = true;
  /* wake up the thread */
  pthread_mutex_lock(&s->st21_wake_mutex);
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);
  if (s->st21_app_thread) {
    pthread_join(s->st21_app_thread, NULL);
    s->st21_app_thread = 0;
  }
}

static int app_tx_video_close_source(struct st_app_tx_video_session* s) {
  if (s->st21_source_fd >= 0) {
    munmap(s->st21_source_begin, s->st21_source_end - s->st21_source_begin);
    close(s->st21_source_fd);
    s->st21_source_fd = -1;
  }
  if (s->st21_pcap) {
    pcap_close(s->st21_pcap);
    s->st21_pcap = NULL;
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

  if (s->st21_ready_framebuff) {
    free(s->st21_ready_framebuff);
    s->st21_ready_framebuff = NULL;
  }
  if (s->st21_free_framebuff) {
    free(s->st21_free_framebuff);
    s->st21_free_framebuff = NULL;
  }
  pthread_mutex_destroy(&s->st21_wake_mutex);
  pthread_cond_destroy(&s->st21_wake_cond);

  return 0;
}

static int app_tx_video_result(struct st_app_tx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_frist_tx_time) / NS_PER_S;
  double framerate = s->st21_frame_done_cnt / time_sec;

  if (!s->st21_frame_done_cnt) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frames send\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->st21_frame_done_cnt);
  return 0;
}

static int app_tx_video_init(struct st_app_context* ctx,
                             st_json_tx_video_session_t* video,
                             struct st_app_tx_video_session* s) {
  int idx = s->idx, ret;
  struct st20_tx_ops ops;
  char name[32];
  st20_tx_handle handle;

  snprintf(name, 32, "app_tx_video_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = video ? video->num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[ST_PORT_P],
         video ? video->dip[ST_PORT_P] : ctx->tx_dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          video ? video->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P],
          ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = video ? video->udp_port : (10000 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[ST_PORT_R],
           video ? video->dip[ST_PORT_R] : ctx->tx_dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            video ? video->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R],
            ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = video ? video->udp_port : (10000 + s->idx);
  }
  ops.pacing = ST21_PACING_NARROW;
  ops.packing = video ? video->packing : ST20_PACKING_GPM_SL;
  ops.type = video ? video->type : ST20_TYPE_FRAME_LEVEL;
  ops.width = video ? st_app_get_width(video->video_format) : 1920;
  ops.height = video ? st_app_get_height(video->video_format) : 1080;
  ops.fps = video ? st_app_get_fps(video->video_format) : ST_FPS_P59_94;
  ops.fmt = video ? video->pg_format : ST20_FMT_YUV_422_10BIT;
  ops.get_next_frame = app_tx_video_next_frame;
  ops.notify_frame_done = app_tx_video_frame_done;
  ops.notify_rtp_done = app_tx_video_rtp_done;
  ops.framebuff_cnt = 3;
  ops.payload_type = 112;

  ret = st20_get_pgroup(ops.fmt, &s->st21_pg);
  if (ret < 0) return ret;
  s->st21_frame_size = ops.width * ops.height * s->st21_pg.size / s->st21_pg.coverage;
  s->width = ops.width;
  s->height = ops.height;
  memcpy(s->st21_source_url, video ? video->video_url : ctx->tx_video_url,
         ST_APP_URL_MAX_LEN);
  s->st21_pcap_input = false;
  s->st21_rtp_input = false;
  s->st = ctx->st;
  s->single_line = (ops.packing == ST20_PACKING_GPM_SL);
  s->expect_fps = st_frame_rate(ops.fps);

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st21_free_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st21_free_framebuff) return -ENOMEM;
  for (int j = 0; j < s->framebuff_cnt; j++) s->st21_free_framebuff[j] = 1;
  s->st21_ready_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st21_ready_framebuff) {
    st_app_free(s->st21_free_framebuff);
    return -ENOMEM;
  }
  for (int j = 0; j < s->framebuff_cnt; j++) s->st21_ready_framebuff[j] = 0;
  s->st21_framebuff_idx = 0;
  s->st21_source_fd = -1;
  pthread_mutex_init(&s->st21_wake_mutex, NULL);
  pthread_cond_init(&s->st21_wake_cond, NULL);

  /* select rtp type for pcap file or tx_video_rtp_ring_size */
  if (strstr(s->st21_source_url, ".pcap")) {
    ops.type = ST20_TYPE_RTP_LEVEL;
    s->st21_pcap_input = true;
  } else if (ctx->tx_video_rtp_ring_size > 0) {
    ops.type = ST20_TYPE_RTP_LEVEL;
    s->st21_rtp_input = true;
  }
  if (ops.type == ST20_TYPE_RTP_LEVEL) {
    s->st21_rtp_input = true;
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
  s->handle_sch_idx = st20_tx_get_sch_idx(handle);
  unsigned int lcore;
  ret = st_app_video_get_lcore(ctx, s->handle_sch_idx, &lcore);
  if (ret >= 0) s->lcore = lcore;

  ret = app_tx_video_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_video_open_source fail %d\n", __func__, idx, ret);
    app_tx_video_uinit(s);
    return ret;
  }
  ret = app_tx_video_start_source(ctx, s);
  if (ret < 0) {
    err("%s(%d), app_tx_video_start_source fail %d\n", __func__, idx, ret);
    app_tx_video_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_video_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_video_session* s;

  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    s->idx = i;
    s->lcore = -1;
    ret = app_tx_video_init(ctx, ctx->json_ctx ? &ctx->json_ctx->tx_video[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_video_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_video_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_video_session* s;

  for (int i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    app_tx_video_stop_source(s);
  }

  return 0;
}

int st_app_tx_video_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_video_session* s;

  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    app_tx_video_uinit(s);
  }

  return 0;
}

int st_app_tx_video_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_tx_video_session* s;

  for (i = 0; i < ctx->tx_video_session_cnt; i++) {
    s = &ctx->tx_video_sessions[i];
    ret += app_tx_video_result(s);
  }

  return 0;
}
