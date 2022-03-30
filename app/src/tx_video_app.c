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

static int app_tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                                   bool* second_field) {
  struct st_app_tx_video_session* s = priv;
  int i;
  pthread_mutex_lock(&s->st20_wake_mutex);
  for (i = 0; i < s->framebuff_cnt; i++) {
    if (s->st20_ready_framebuff[i].used) {
      s->st20_framebuff_idx = i;
      s->st20_ready_framebuff[i].used = false;
      break;
    }
  }
  pthread_cond_signal(&s->st20_wake_cond);
  pthread_mutex_unlock(&s->st20_wake_mutex);
  if (i == s->framebuff_cnt) return -EIO;
  *next_frame_idx = s->st20_framebuff_idx;
  *second_field = s->st20_ready_framebuff[i].second_field;

  dbg("%s(%d), next framebuffer index %d\n", __func__, s->idx, *next_frame_idx);
  return 0;
}

static int app_tx_video_frame_done(void* priv, uint16_t frame_idx) {
  struct st_app_tx_video_session* s = priv;

  pthread_mutex_lock(&s->st20_wake_mutex);
  s->st20_free_framebuff[frame_idx] = 1;
  pthread_cond_signal(&s->st20_wake_cond);
  pthread_mutex_unlock(&s->st20_wake_mutex);
  s->st20_frame_done_cnt++;
  if (!s->stat_frame_frist_tx_time)
    s->stat_frame_frist_tx_time = st_app_get_monotonic_time();
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return 0;
}

static int app_tx_frame_lines_ready(void* priv, uint16_t frame_idx,
                                    uint16_t* lines_ready) {
  struct st_app_tx_video_session* s = priv;
  pthread_mutex_lock(&s->st20_wake_mutex);
  *lines_ready = s->st20_ready_framebuff[frame_idx].lines_ready;
  pthread_mutex_unlock(&s->st20_wake_mutex);

  dbg("%s(%d), lines ready %d\n", __func__, s->idx, *lines_ready);
  return 0;
}

static int app_tx_video_rtp_done(void* priv) {
  struct st_app_tx_video_session* s = priv;

  pthread_mutex_lock(&s->st20_wake_mutex);
  pthread_cond_signal(&s->st20_wake_cond);
  pthread_mutex_unlock(&s->st20_wake_mutex);
  s->st20_packet_done_cnt++;
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
  while (!s->st20_app_thread_stop) {
    pthread_mutex_lock(&s->st20_wake_mutex);
    // guarantee the sequence
    bool has_ready = false;
    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st20_ready_framebuff[i].used) {
        has_ready = true;
        break;
      }
    }
    if (has_ready) {
      if (!s->st20_app_thread_stop)
        pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
      pthread_mutex_unlock(&s->st20_wake_mutex);
      continue;
    }

    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st20_free_framebuff[i] == 1) {
        s->st20_free_framebuff[i] = 0;
        break;
      }
    }
    if (i == s->framebuff_cnt) {
      if (!s->st20_app_thread_stop)
        pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
      pthread_mutex_unlock(&s->st20_wake_mutex);
      continue;
    }
    if (s->slice) {
      s->st20_ready_framebuff[i].used = true;
      s->st20_ready_framebuff[i].lines_ready = 0;
    }
    pthread_mutex_unlock(&s->st20_wake_mutex);
    uint8_t* src = s->st20_frame_cursor;
    uint8_t* dst = st20_tx_get_framebuffer(s->handle, i);
    int framesize = s->interlaced ? s->st20_frame_size * 2 : s->st20_frame_size;
    if (s->st20_frame_cursor + framesize > s->st20_source_end) {
      s->st20_frame_cursor = s->st20_source_begin;
      src = s->st20_frame_cursor;
    }
    if (!s->interlaced && !s->slice) {
      st_memcpy(dst, src, s->st20_frame_size);
      /* point to next frame */
      s->st20_frame_cursor += s->st20_frame_size;
    } else if (s->slice) {
      int stride = s->width / s->st20_pg.coverage * s->st20_pg.size;
      for (int slice_idx = 0; slice_idx * s->lines_per_slice < s->height; slice_idx++) {
        int offset = slice_idx * s->lines_per_slice * stride;
        st_memcpy(dst + offset, src + offset, s->lines_per_slice * stride);
        pthread_mutex_lock(&s->st20_wake_mutex);
        s->st20_ready_framebuff[i].lines_ready += s->lines_per_slice;
        pthread_mutex_unlock(&s->st20_wake_mutex);
      }
      s->st20_frame_cursor += s->st20_frame_size;
    } else {
      s->st20_ready_framebuff[i].second_field = s->second_field;
      s->second_field = !s->second_field;
      int stride = s->width / s->st20_pg.coverage * s->st20_pg.size;
      int offset = s->st20_ready_framebuff[i].second_field ? stride : 0;
      for (int i = 0; i < s->height / 2; i++) {
        st_memcpy(dst + i * stride, s->st20_frame_cursor + i * stride * 2 + offset,
                  stride);
      }
      if (s->st20_ready_framebuff[i].second_field) s->st20_frame_cursor += framesize;
    }
    if (!s->slice) {
      pthread_mutex_lock(&s->st20_wake_mutex);
      s->st20_ready_framebuff[i].used = true;
      pthread_mutex_unlock(&s->st20_wake_mutex);
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
  if (s->lcore != -1) {
    st_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st20_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st20_wake_mutex);
      } else {
        if (!s->st20_app_thread_stop)
          pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
        pthread_mutex_unlock(&s->st20_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st20_pcap, &hdr);
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
      pcap_close(s->st20_pcap);
      /* open capture file for offline processing */
      s->st20_pcap = pcap_open_offline(s->st20_source_url, err_buf);
      if (s->st20_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st20_source_url, err_buf);
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
  s->st20_bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_pkt_idx = 0;
  s->st20_seq_id = 1;
  int height = ops->height;
  if (ops->interlaced) height = height >> 1;

  if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line for rtp */
    size_t bytes_in_pkt = ST_PKT_MAX_RTP_BYTES - sizeof(*rtp);
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
        ST_PKT_MAX_RTP_BYTES - sizeof(*rtp) - sizeof(struct st20_rfc4175_extra_rtp_hdr);
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
    ops->rtp_pkt_size = ST_PKT_MAX_RTP_BYTES;
  else
    ops->rtp_pkt_size = s->st20_pkt_data_len + sizeof(*rtp);

  memset(rtp, 0, sizeof(*rtp));
  rtp->base.version = 2;
  rtp->base.payload_type = 112;
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
  st_memcpy(rtp, &s->st20_rtp_base, sizeof(*rtp));
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
    st_memcpy(payload, frame + offset, data_len);
  else {
    if (s->st20_second_field)
      st_memcpy(payload,
                frame + ((2 * row_number + 1) * s->width + row_offset) /
                            s->st20_pg.coverage * s->st20_pg.size,
                data_len);
    else
      st_memcpy(payload,
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
    if (!s->stat_frame_frist_tx_time)
      s->stat_frame_frist_tx_time = st_app_get_monotonic_time();
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
  if (s->lcore != -1) {
    st_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20_app_thread_stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st20_wake_mutex);
      /* try again */
      mbuf = st20_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st20_wake_mutex);
      } else {
        if (!s->st20_app_thread_stop)
          pthread_cond_wait(&s->st20_wake_cond, &s->st20_wake_mutex);
        pthread_mutex_unlock(&s->st20_wake_mutex);
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
  if (!s->st20_pcap_input) {
    int fd;
    struct stat i;

    fd = open(s->st20_source_url, O_RDONLY);
    if (fd < 0) {
      err("%s, open fail '%s'\n", __func__, s->st20_source_url);
      return -EIO;
    }

    fstat(fd, &i);
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

    s->st20_source_begin = st_hp_malloc(s->st, i.st_size, ST_PORT_P);
    if (!s->st20_source_begin) {
      warn("%s, source malloc on hugepage fail\n", __func__);
      s->st20_source_begin = m;
      s->st20_frame_cursor = m;
      s->st20_source_end = m + i.st_size;
      s->st20_source_fd = fd;
    } else {
      s->st20_frame_cursor = s->st20_source_begin;
      st_memcpy(s->st20_source_begin, m, i.st_size);
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

static int app_tx_video_start_source(struct st_app_context* ctx,
                                     struct st_app_tx_video_session* s) {
  int ret = -EINVAL;

  if (s->st20_pcap_input)
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_pcap_thread, s);
  else if (s->st20_rtp_input)
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_rtp_thread, s);
  else
    ret = pthread_create(&s->st20_app_thread, NULL, app_tx_video_frame_thread, s);
  if (ret < 0) {
    err("%s, st20_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st20_app_thread_stop = false;

  return 0;
}

static void app_tx_video_stop_source(struct st_app_tx_video_session* s) {
  s->st20_app_thread_stop = true;
  /* wake up the thread */
  pthread_mutex_lock(&s->st20_wake_mutex);
  pthread_cond_signal(&s->st20_wake_cond);
  pthread_mutex_unlock(&s->st20_wake_mutex);
  if (s->st20_app_thread) {
    pthread_join(s->st20_app_thread, NULL);
    s->st20_app_thread = 0;
  }
}

static int app_tx_video_close_source(struct st_app_tx_video_session* s) {
  if (s->st20_source_fd < 0 && s->st20_source_begin) {
    st_hp_free(s->st, s->st20_source_begin);
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

  if (s->st20_ready_framebuff) {
    free(s->st20_ready_framebuff);
    s->st20_ready_framebuff = NULL;
  }
  if (s->st20_free_framebuff) {
    free(s->st20_free_framebuff);
    s->st20_free_framebuff = NULL;
  }
  pthread_mutex_destroy(&s->st20_wake_mutex);
  pthread_cond_destroy(&s->st20_wake_cond);

  return 0;
}

static int app_tx_video_result(struct st_app_tx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_frist_tx_time) / NS_PER_S;
  double framerate = s->st20_frame_done_cnt / time_sec;

  if (!s->st20_frame_done_cnt) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frames send\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->st20_frame_done_cnt);
  return 0;
}

static int app_tx_video_init(struct st_app_context* ctx,
                             st_json_tx_video_session_t* video,
                             struct st_app_tx_video_session* s) {
  int idx = s->idx, ret;
  struct st20_tx_ops ops;
  char name[32];
  st20_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

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
  ops.interlaced = video ? st_app_get_interlaced(video->video_format) : false;
  ops.get_next_frame = app_tx_video_next_frame;
  ops.notify_frame_done = app_tx_video_frame_done;
  ops.query_frame_lines_ready = app_tx_frame_lines_ready;
  ops.notify_rtp_done = app_tx_video_rtp_done;
  ops.framebuff_cnt = 2;
  ops.payload_type = 112;

  ret = st20_get_pgroup(ops.fmt, &s->st20_pg);
  if (ret < 0) return ret;
  s->st20_frame_size = ops.width * ops.height * s->st20_pg.size / s->st20_pg.coverage;
  if (ops.interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
  s->width = ops.width;
  s->height = ops.height;
  s->interlaced = ops.interlaced ? true : false;
  memcpy(s->st20_source_url, video ? video->video_url : ctx->tx_video_url,
         ST_APP_URL_MAX_LEN);
  s->st20_pcap_input = false;
  s->st20_rtp_input = false;
  s->st = ctx->st;
  s->single_line = (ops.packing == ST20_PACKING_GPM_SL);
  s->slice = (ops.type == ST20_TYPE_SLICE_LEVEL);
  s->expect_fps = st_frame_rate(ops.fps);

  s->framebuff_cnt = ops.framebuff_cnt;
  s->lines_per_slice = ops.height / 30;

  s->st20_free_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st20_free_framebuff) return -ENOMEM;
  for (int j = 0; j < s->framebuff_cnt; j++) s->st20_free_framebuff[j] = 1;
  s->st20_ready_framebuff = (struct st_app_frameinfo*)st_app_zmalloc(
      sizeof(struct st_app_frameinfo) * s->framebuff_cnt);
  if (!s->st20_ready_framebuff) {
    st_app_free(s->st20_free_framebuff);
    return -ENOMEM;
  }
  for (int j = 0; j < s->framebuff_cnt; j++) s->st20_ready_framebuff[j].used = false;
  s->st20_framebuff_idx = 0;
  s->st20_source_fd = -1;
  pthread_mutex_init(&s->st20_wake_mutex, NULL);
  pthread_cond_init(&s->st20_wake_cond, NULL);

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
  ctx->tx_video_sessions = (struct st_app_tx_video_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_video_session) * ctx->tx_video_session_cnt);
  if (!ctx->tx_video_sessions) return -ENOMEM;
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

  return 0;
}
