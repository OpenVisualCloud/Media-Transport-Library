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

#include "tx_ancillary_app.h"

static int app_tx_anc_session_next_frame(void* priv, uint16_t* next_frame_idx) {
  struct st_app_tx_anc_session* s = priv;
  uint16_t i;
  pthread_mutex_lock(&s->st40_wake_mutex);
  for (i = 0; i < s->framebuff_cnt; i++) {
    if (s->st40_ready_framebuff[i] == 1) {
      s->st40_framebuff_idx = i;
      s->st40_ready_framebuff[i] = 0;
      break;
    }
  }
  pthread_cond_signal(&s->st40_wake_cond);
  pthread_mutex_unlock(&s->st40_wake_mutex);
  if (i == s->framebuff_cnt) return -1;
  *next_frame_idx = s->st40_framebuff_idx;

  dbg("%s(%d), next framebuffer index %d\n", __func__, s->idx, *next_frame_idx);
  return 0;
}

static int app_tx_anc_session_frame_done(void* priv, uint16_t frame_idx) {
  struct st_app_tx_anc_session* s = priv;

  pthread_mutex_lock(&s->st40_wake_mutex);
  s->st40_free_framebuff[frame_idx] = 1;
  pthread_cond_signal(&s->st40_wake_cond);
  pthread_mutex_unlock(&s->st40_wake_mutex);
  s->st40_frame_done_cnt++;
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return 0;
}

static int app_tx_anc_session_rtp_done(void* priv) {
  struct st_app_tx_anc_session* s = priv;
  pthread_mutex_lock(&s->st40_wake_mutex);
  pthread_cond_signal(&s->st40_wake_cond);
  pthread_mutex_unlock(&s->st40_wake_mutex);
  s->st40_packet_done_cnt++;
  return 0;
}

static void* app_tx_anc_session_frame_thread(void* arg) {
  struct st_app_tx_anc_session* s = arg;
  uint16_t i;
  while (!s->st40_app_thread_stop) {
    // guarantee the sequence
    pthread_mutex_lock(&s->st40_wake_mutex);
    bool has_ready = false;
    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st40_ready_framebuff[i] == 1) {
        has_ready = true;
        break;
      }
    }
    if (has_ready) {
      if (!s->st40_app_thread_stop)
        pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
      pthread_mutex_unlock(&s->st40_wake_mutex);
      continue;
    }

    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st40_free_framebuff[i] == 1) {
        s->st40_free_framebuff[i] = 0;
        break;
      }
    }
    if (i == s->framebuff_cnt) {
      if (!s->st40_app_thread_stop)
        pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
      pthread_mutex_unlock(&s->st40_wake_mutex);
      continue;
    }
    pthread_mutex_unlock(&s->st40_wake_mutex);
    struct st40_frame* dst = (struct st40_frame*)st40_tx_get_framebuffer(s->handle, i);
    if (!dst) {
      err("did not get the buffer, continue");
      continue;
    }
    uint16_t udw_size = s->st40_source_end - s->st40_frame_cursor > 255
                            ? 255
                            : s->st40_source_end - s->st40_frame_cursor;
    dst->meta[0].c = 0;
    dst->meta[0].line_number = 10;
    dst->meta[0].hori_offset = 0;
    dst->meta[0].s = 0;
    dst->meta[0].stream_num = 0;
    dst->meta[0].did = 0x43;
    dst->meta[0].sdid = 0x02;
    dst->meta[0].udw_size = udw_size;
    dst->meta[0].udw_offset = 0;
    dst->data = s->st40_frame_cursor;
    dst->data_size = udw_size;
    dst->meta_num = 1;
    s->st40_frame_cursor += udw_size;
    if (s->st40_frame_cursor == s->st40_source_end)
      s->st40_frame_cursor = s->st40_source_begin;
    pthread_mutex_lock(&s->st40_wake_mutex);
    s->st40_ready_framebuff[i] = 1;
    pthread_mutex_unlock(&s->st40_wake_mutex);
  }

  return NULL;
}

static void* app_tx_anc_session_pcap_thread(void* arg) {
  struct st_app_tx_anc_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  struct pcap_pkthdr hdr;
  uint8_t* packet;
  struct ether_header* eth_hdr;
  struct ip* ip_hdr;
  struct udphdr* udp_hdr;
  uint16_t udp_data_len;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40_app_thread_stop) {
    /* get available buffer*/
    mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st40_wake_mutex);
      /* try again */
      mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st40_wake_mutex);
      } else {
        if (!s->st40_app_thread_stop)
          pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
        pthread_mutex_unlock(&s->st40_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st40_pcap, &hdr);
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
      pcap_close(s->st40_pcap);
      /* open capture file for offline processing */
      s->st40_pcap = pcap_open_offline(s->st40_source_url, err_buf);
      if (s->st40_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st40_source_url, err_buf);
        return NULL;
      }
    }

    st40_tx_put_mbuf(s->handle, mbuf, udp_data_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_tx_anc_build_rtp_packet(struct st_app_tx_anc_session* s, void* usrptr,
                                        uint16_t* mbuf_len) {
  /* generate one anc rtp for test purpose */
  struct st40_rfc8331_rtp_hdr* hdr = (struct st40_rfc8331_rtp_hdr*)usrptr;
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);
  uint16_t udw_size = s->st40_source_end - s->st40_frame_cursor > 255
                          ? 255
                          : s->st40_source_end - s->st40_frame_cursor;
  uint16_t check_sum, total_size, payload_len;
  hdr->base.marker = 1;
  hdr->anc_count = 1;
  hdr->base.payload_type = 113;
  hdr->base.version = 2;
  hdr->base.extension = 0;
  hdr->base.padding = 0;
  hdr->base.csrc_count = 0;
  hdr->f = 0b00;
  hdr->base.tmstamp = s->st40_rtp_tmstamp;
  hdr->base.ssrc = htonl(0x88888888 + s->idx);
  /* update rtp seq*/
  hdr->base.seq_number = htons((uint16_t)s->st40_seq_id);
  hdr->seq_number_ext = htons((uint16_t)(s->st40_seq_id >> 16));
  s->st40_seq_id++;
  s->st40_rtp_tmstamp++;
  payload_hdr->first_hdr_chunk.c = 0;
  payload_hdr->first_hdr_chunk.line_number = 10;
  payload_hdr->first_hdr_chunk.horizontal_offset = 0;
  payload_hdr->first_hdr_chunk.s = 0;
  payload_hdr->first_hdr_chunk.stream_num = 0;
  payload_hdr->second_hdr_chunk.did = st40_add_parity_bits(0x43);
  payload_hdr->second_hdr_chunk.sdid = st40_add_parity_bits(0x02);
  payload_hdr->second_hdr_chunk.data_count = st40_add_parity_bits(udw_size);
  payload_hdr->swaped_first_hdr_chunk = htonl(payload_hdr->swaped_first_hdr_chunk);
  payload_hdr->swaped_second_hdr_chunk = htonl(payload_hdr->swaped_second_hdr_chunk);
  for (int i = 0; i < udw_size; i++) {
    st40_set_udw(i + 3, st40_add_parity_bits(s->st40_frame_cursor[i]),
                 (uint8_t*)&payload_hdr->second_hdr_chunk);
  }
  check_sum = st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk);
  st40_set_udw(udw_size + 3, check_sum, (uint8_t*)&payload_hdr->second_hdr_chunk);
  total_size = ((3 + udw_size + 1) * 10) / 8;  // Calculate size of the
                                               // 10-bit words: DID, SDID, DATA_COUNT
                                               // + size of buffer with data + checksum
  total_size = (4 - total_size % 4) + total_size;  // Calculate word align to the 32-bit
                                                   // word of ANC data packet
  payload_len =
      sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size;  // Full size of one ANC
  *mbuf_len = payload_len + sizeof(struct st40_rfc8331_rtp_hdr);
  hdr->length = htons(payload_len);
  s->st40_frame_cursor += udw_size;
  if (s->st40_frame_cursor == s->st40_source_end)
    s->st40_frame_cursor = s->st40_source_begin;
}

static void* app_tx_anc_session_rtp_thread(void* arg) {
  struct st_app_tx_anc_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40_app_thread_stop) {
    /* get available buffer*/
    mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st40_wake_mutex);
      /* try again */
      mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st40_wake_mutex);
      } else {
        if (!s->st40_app_thread_stop)
          pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
        pthread_mutex_unlock(&s->st40_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_anc_build_rtp_packet(s, usrptr, &mbuf_len);

    st40_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_anc_session_open_source(struct st_app_tx_anc_session* s) {
  if (!s->st40_pcap_input) {
    struct stat i;

    s->st40_source_fd = open(s->st40_source_url, O_RDONLY);
    if (s->st40_source_fd >= 0) {
      fstat(s->st40_source_fd, &i);

      uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, s->st40_source_fd, 0);

      if (MAP_FAILED != m) {
        s->st40_source_begin = m;
        s->st40_frame_cursor = m;
        s->st40_source_end = m + i.st_size;
      } else {
        err("%s, mmap fail '%s'\n", __func__, s->st40_source_url);
        return -EIO;
      }
    } else {
      err("%s, open fail '%s'\n", __func__, s->st40_source_url);
      return -EIO;
    }
  } else {
    char err_buf[PCAP_ERRBUF_SIZE];

    /* open capture file for offline processing */
    s->st40_pcap = pcap_open_offline(s->st40_source_url, err_buf);
    if (!s->st40_pcap) {
      err("pcap_open_offline %s() failed: %s\n:", s->st40_source_url, err_buf);
      return -EIO;
    }
  }

  return 0;
}

static int app_tx_anc_session_close_source(struct st_app_tx_anc_session* s) {
  if (s->st40_source_fd >= 0) {
    munmap(s->st40_source_begin, s->st40_source_end - s->st40_source_begin);
    close(s->st40_source_fd);
    s->st40_source_fd = -1;
  }
  if (s->st40_pcap) {
    pcap_close(s->st40_pcap);
    s->st40_pcap = NULL;
  }

  return 0;
}

static int app_tx_anc_session_start_source(struct st_app_tx_anc_session* s) {
  int ret = -EINVAL;

  if (s->st40_pcap_input)
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_session_pcap_thread,
                         (void*)s);
  else if (s->st40_rtp_input)
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_session_rtp_thread,
                         (void*)s);
  else
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_session_frame_thread,
                         (void*)s);
  if (ret < 0) {
    err("%s, st21_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st40_app_thread_stop = false;

  return 0;
}

static void app_tx_anc_session_stop_source(struct st_app_tx_anc_session* s) {
  if (s->st40_source_fd >= 0) {
    s->st40_app_thread_stop = true;
    /* wake up the thread */
    pthread_mutex_lock(&s->st40_wake_mutex);
    pthread_cond_signal(&s->st40_wake_cond);
    pthread_mutex_unlock(&s->st40_wake_mutex);
    if (s->st40_app_thread) (void)pthread_join(s->st40_app_thread, NULL);
  }
}

static int app_tx_anc_session_init(struct st_app_context* ctx,
                                   st_json_tx_ancillary_session_t* anc,
                                   struct st_app_tx_anc_session* s) {
  int idx = s->idx, ret;
  struct st40_tx_ops ops;
  char name[32];
  st40_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->framebuff_cnt = 2;
  s->st40_framebuff_idx = 0;
  s->st40_seq_id = 1;
  s->st40_free_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st40_free_framebuff) return -ENOMEM;
  for (int j = 0; j < s->framebuff_cnt; j++) s->st40_free_framebuff[j] = 1;
  s->st40_ready_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st40_ready_framebuff) {
    st_app_free(s->st40_free_framebuff);
    return -ENOMEM;
  }
  for (int j = 0; j < s->framebuff_cnt; j++) s->st40_ready_framebuff[j] = 0;
  s->st40_source_fd = -1;
  pthread_mutex_init(&s->st40_wake_mutex, NULL);
  pthread_cond_init(&s->st40_wake_cond, NULL);

  snprintf(name, 32, "app_tx_ancillary%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = anc ? anc->num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[ST_PORT_P], anc ? anc->dip[ST_PORT_P] : ctx->tx_dip_addr[ST_PORT_P],
         ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          anc ? anc->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = anc ? anc->udp_port : (10200 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[ST_PORT_R],
           anc ? anc->dip[ST_PORT_R] : ctx->tx_dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            anc ? anc->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = anc ? anc->udp_port : (10200 + s->idx);
  }
  ops.get_next_frame = app_tx_anc_session_next_frame;
  ops.notify_frame_done = app_tx_anc_session_frame_done;
  ops.notify_rtp_done = app_tx_anc_session_rtp_done;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.fps = anc ? anc->anc_fps : ST_FPS_P59_94;
  s->st40_pcap_input = false;
  ops.type = anc ? anc->type : ST40_TYPE_FRAME_LEVEL;
  ops.payload_type = 113;
  /* select rtp type for pcap file or tx_video_rtp_ring_size */
  if (strstr(s->st40_source_url, ".pcap")) {
    ops.type = ST40_TYPE_RTP_LEVEL;
    s->st40_pcap_input = true;
  } else if (ctx->tx_anc_rtp_ring_size > 0) {
    ops.type = ST40_TYPE_RTP_LEVEL;
    s->st40_rtp_input = true;
  }
  if (ops.type == ST40_TYPE_RTP_LEVEL) {
    s->st40_rtp_input = true;
    if (ctx->tx_anc_rtp_ring_size > 0)
      ops.rtp_ring_size = ctx->tx_anc_rtp_ring_size;
    else
      ops.rtp_ring_size = 16;
  }

  handle = st40_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30_tx_create fail\n", __func__, idx);
    return -EIO;
  }

  s->handle = handle;
  strncpy(s->st40_source_url, anc ? anc->anc_url : ctx->tx_anc_url,
          sizeof(s->st40_source_url));

  ret = app_tx_anc_session_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_audio_session_open_source fail\n", __func__, idx);
    return ret;
  }

  ret = app_tx_anc_session_start_source(s);
  if (ret < 0) {
    app_tx_anc_session_close_source(s);
    err("%s(%d), app_tx_audio_session_start_source fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

int st_app_tx_anc_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_anc_session* s;
  if (!ctx->tx_anc_sessions) return 0;
  for (int i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    app_tx_anc_session_stop_source(s);
  }

  return 0;
}

int st_app_tx_anc_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_anc_session* s;
  ctx->tx_anc_sessions = (struct st_app_tx_anc_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_anc_session) * ctx->tx_anc_session_cnt);
  if (!ctx->tx_anc_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    s->idx = i;
    ret =
        app_tx_anc_session_init(ctx, ctx->json_ctx ? &ctx->json_ctx->tx_anc[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_anc_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_anc_sessions_uinit(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_anc_session* s;
  if (!ctx->tx_anc_sessions) return 0;
  for (i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    if (s->handle) {
      ret = st40_tx_free(s->handle);
      if (ret < 0) err("%s(%d), st_tx_anc_session_free fail %d\n", __func__, i, ret);
      s->handle = NULL;
    }

    app_tx_anc_session_close_source(s);
    if (s->st40_ready_framebuff) {
      free(s->st40_ready_framebuff);
      s->st40_ready_framebuff = NULL;
    }
    if (s->st40_free_framebuff) {
      free(s->st40_free_framebuff);
      s->st40_free_framebuff = NULL;
    }
    pthread_mutex_destroy(&s->st40_wake_mutex);
    pthread_cond_destroy(&s->st40_wake_cond);
  }
  st_app_free(ctx->tx_anc_sessions);

  return 0;
}