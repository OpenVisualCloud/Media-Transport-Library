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

#include "tx_audio_app.h"

static int app_tx_audio_session_next_frame(void* priv, uint16_t* next_frame_idx) {
  struct st_app_tx_audio_session* s = priv;
  uint16_t i;
  pthread_mutex_lock(&s->st30_wake_mutex);
  for (i = 0; i < s->framebuff_cnt; i++) {
    if (s->st30_ready_framebuff[i] == 1) {
      s->st30_framebuff_idx = i;
      s->st30_ready_framebuff[i] = 0;
      break;
    }
  }
  pthread_cond_signal(&s->st30_wake_cond);
  pthread_mutex_unlock(&s->st30_wake_mutex);
  if (i == s->framebuff_cnt) return -1;
  *next_frame_idx = s->st30_framebuff_idx;

  dbg("%s(%d), next framebuffer index %d\n", __func__, s->idx, *next_frame_idx);
  return 0;
}

static int app_tx_audio_session_frame_done(void* priv, uint16_t frame_idx) {
  struct st_app_tx_audio_session* s = priv;
  pthread_mutex_lock(&s->st30_wake_mutex);
  s->st30_free_framebuff[frame_idx] = 1;
  pthread_cond_signal(&s->st30_wake_cond);
  pthread_mutex_unlock(&s->st30_wake_mutex);
  s->st30_frame_done_cnt++;
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return 0;
}

static int app_tx_audio_session_rtp_done(void* priv) {
  struct st_app_tx_audio_session* s = priv;
  pthread_mutex_lock(&s->st30_wake_mutex);
  pthread_cond_signal(&s->st30_wake_cond);
  pthread_mutex_unlock(&s->st30_wake_mutex);
  s->st30_packet_done_cnt++;
  return 0;
}

static void* app_tx_audio_session_frame_thread(void* arg) {
  struct st_app_tx_audio_session* s = arg;
  uint16_t i;
  while (!s->st30_app_thread_stop) {
    pthread_mutex_lock(&s->st30_wake_mutex);
    // guarantee the sequence
    bool has_ready = false;
    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st30_ready_framebuff[i] == 1) {
        has_ready = true;
        break;
      }
    }
    if (has_ready) {
      if (!s->st30_app_thread_stop)
        pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
      pthread_mutex_unlock(&s->st30_wake_mutex);
      continue;
    }
    for (i = 0; i < s->framebuff_cnt; i++) {
      if (s->st30_free_framebuff[i] == 1) {
        s->st30_free_framebuff[i] = 0;
        break;
      }
    }

    if (i == s->framebuff_cnt) {
      if (!s->st30_app_thread_stop)
        pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
      pthread_mutex_unlock(&s->st30_wake_mutex);
      continue;
    }
    pthread_mutex_unlock(&s->st30_wake_mutex);
    uint8_t* src = s->st30_frame_cursor;
    uint8_t* dst = st30_tx_get_framebuffer(s->handle, i);
    if (s->st30_frame_cursor + s->st30_frame_size > s->st30_source_end) {
      int len = s->st30_source_end - s->st30_frame_cursor;
      len = len / s->pkt_len * s->pkt_len;
      if (len) st_memcpy(dst, s->st30_frame_cursor, len);
      /* wrap back in the end */
      st_memcpy(dst + len, s->st30_source_begin, s->st30_frame_size - len);
      s->st30_frame_cursor = s->st30_source_begin + s->st30_frame_size - len;
    } else {
      st_memcpy(dst, src, s->st30_frame_size);
      s->st30_frame_cursor += s->st30_frame_size;
    }
    pthread_mutex_lock(&s->st30_wake_mutex);
    s->st30_ready_framebuff[i] = 1;
    pthread_mutex_unlock(&s->st30_wake_mutex);
  }

  return NULL;
}

static void* app_tx_audio_session_pcap_thread(void* arg) {
  struct st_app_tx_audio_session* s = arg;
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
  while (!s->st30_app_thread_stop) {
    /* get available buffer*/
    mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st30_wake_mutex);
      /* try again */
      mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st30_wake_mutex);
      } else {
        if (!s->st30_app_thread_stop)
          pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
        pthread_mutex_unlock(&s->st30_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st30_pcap, &hdr);
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
      pcap_close(s->st30_pcap);
      /* open capture file for offline processing */
      s->st30_pcap = pcap_open_offline(s->st30_source_url, err_buf);
      if (s->st30_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st30_source_url, err_buf);
        return NULL;
      }
    }

    st30_tx_put_mbuf(s->handle, mbuf, udp_data_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_tx_audio_build_rtp_packet(struct st_app_tx_audio_session* s, void* usrptr,
                                          uint16_t* mbuf_len) {
  /* generate one anc rtp for test purpose */
  struct st_rfc3550_rtp_hdr* rtp = (struct st_rfc3550_rtp_hdr*)usrptr;
  uint8_t* payload = (uint8_t*)&rtp[1];
  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = 111;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->st30_rtp_tmstamp;
  s->st30_rtp_tmstamp++;
  rtp->seq_number = htons(s->st30_seq_id);
  s->st30_seq_id++;

  if (s->st30_frame_cursor + s->pkt_len > s->st30_source_end) {
    st_memcpy(payload, s->st30_source_begin, s->pkt_len);
    s->st30_frame_cursor = s->st30_source_begin + s->pkt_len;
  } else {
    st_memcpy(payload, s->st30_frame_cursor, s->pkt_len);
    s->st30_frame_cursor += s->pkt_len;
  }
  *mbuf_len = sizeof(struct st_rfc3550_rtp_hdr) + s->pkt_len;
}

static void* app_tx_audio_session_rtp_thread(void* arg) {
  struct st_app_tx_audio_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st30_app_thread_stop) {
    /* get available buffer*/
    mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st30_wake_mutex);
      /* try again */
      mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st30_wake_mutex);
      } else {
        if (!s->st30_app_thread_stop)
          pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
        pthread_mutex_unlock(&s->st30_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_audio_build_rtp_packet(s, usrptr, &mbuf_len);

    st30_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_audio_session_open_source(struct st_app_tx_audio_session* s) {
  if (!s->st30_pcap_input) {
    struct stat i;

    s->st30_source_fd = open(s->st30_source_url, O_RDONLY);
    if (s->st30_source_fd >= 0) {
      fstat(s->st30_source_fd, &i);

      uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, s->st30_source_fd, 0);

      if (MAP_FAILED != m) {
        s->st30_source_begin = m;
        s->st30_frame_cursor = m;
        s->st30_source_end = m + i.st_size;
      } else {
        err("%s, mmap fail '%s'\n", __func__, s->st30_source_url);
        return -EIO;
      }
    } else {
      err("%s, open fail '%s'\n", __func__, s->st30_source_url);
      return -EIO;
    }
  } else {
    char err_buf[PCAP_ERRBUF_SIZE];

    /* open capture file for offline processing */
    s->st30_pcap = pcap_open_offline(s->st30_source_url, err_buf);
    if (!s->st30_pcap) {
      err("pcap_open_offline %s() failed: %s\n:", s->st30_source_url, err_buf);
      return -EIO;
    }
  }

  return 0;
}

static int app_tx_audio_session_close_source(struct st_app_tx_audio_session* s) {
  if (s->st30_source_fd >= 0) {
    munmap(s->st30_source_begin, s->st30_source_end - s->st30_source_begin);
    close(s->st30_source_fd);
    s->st30_source_fd = -1;
  }
  if (s->st30_pcap) {
    pcap_close(s->st30_pcap);
    s->st30_pcap = NULL;
  }

  return 0;
}

static int app_tx_audio_session_start_source(struct st_app_tx_audio_session* s) {
  int ret = -EINVAL;

  if (s->st30_pcap_input)
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_session_pcap_thread,
                         (void*)s);
  else if (s->st30_rtp_input)
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_session_rtp_thread,
                         (void*)s);
  else
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_session_frame_thread,
                         (void*)s);

  if (ret != 0) {
    err("%s, st30_app_thread create fail err = %d", __func__, ret);
    return -EIO;
  }
  s->st30_app_thread_stop = false;
  return 0;
}

static void app_tx_audio_session_stop_source(struct st_app_tx_audio_session* s) {
  if (s->st30_source_fd >= 0) {
    s->st30_app_thread_stop = true;
    /* wake up the thread */
    pthread_mutex_lock(&s->st30_wake_mutex);
    pthread_cond_signal(&s->st30_wake_cond);
    pthread_mutex_unlock(&s->st30_wake_mutex);
    if (s->st30_app_thread) (void)pthread_join(s->st30_app_thread, NULL);
  }
}

static int app_tx_audio_session_init(struct st_app_context* ctx,
                                     st_json_tx_audio_session_t* audio,
                                     struct st_app_tx_audio_session* s) {
  int idx = s->idx, ret, j;
  struct st30_tx_ops ops;
  char name[32];
  st30_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->framebuff_cnt = 2;
  s->st30_seq_id = 1;
  s->st30_framebuff_idx = 0;
  s->st30_free_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st30_free_framebuff) return -ENOMEM;
  for (j = 0; j < s->framebuff_cnt; j++) s->st30_free_framebuff[j] = 1;
  s->st30_ready_framebuff = (int*)st_app_zmalloc(sizeof(int) * s->framebuff_cnt);
  if (!s->st30_ready_framebuff) {
    st_app_free(s->st30_free_framebuff);
    return -ENOMEM;
  }
  for (j = 0; j < s->framebuff_cnt; j++) s->st30_ready_framebuff[j] = 0;
  s->st30_source_fd = -1;
  pthread_mutex_init(&s->st30_wake_mutex, NULL);
  pthread_cond_init(&s->st30_wake_cond, NULL);

  snprintf(name, 32, "app_tx_audio%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = audio ? audio->num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[ST_PORT_P],
         audio ? audio->dip[ST_PORT_P] : ctx->tx_dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          audio ? audio->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P],
          ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = audio ? audio->udp_port : (10100 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[ST_PORT_R],
           audio ? audio->dip[ST_PORT_R] : ctx->tx_dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            audio ? audio->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R],
            ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = audio ? audio->udp_port : (10100 + s->idx);
  }
  ops.get_next_frame = app_tx_audio_session_next_frame;
  ops.notify_frame_done = app_tx_audio_session_frame_done;
  ops.notify_rtp_done = app_tx_audio_session_rtp_done;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.fmt = audio ? audio->audio_format : ST30_FMT_PCM16;
  ops.channel = audio ? audio->audio_channel : 2;
  ops.sampling = audio ? audio->audio_sampling : ST30_SAMPLING_48K;
  ops.sample_size = st30_get_sample_size(ops.fmt, ops.channel, ops.sampling);
  int frametime = audio ? audio->audio_frametime_ms : 1; /* frame time: ms */
  s->st30_frame_size = frametime * ops.sample_size;
  s->pkt_len = ops.sample_size;
  ops.framebuff_size = s->st30_frame_size;
  ops.payload_type = 111;

  s->st30_pcap_input = false;
  ops.type = audio ? audio->type : ST30_TYPE_FRAME_LEVEL;
  /* select rtp type for pcap file or tx_video_rtp_ring_size */
  if (strstr(s->st30_source_url, ".pcap")) {
    ops.type = ST30_TYPE_RTP_LEVEL;
    s->st30_pcap_input = true;
  } else if (ctx->tx_audio_rtp_ring_size > 0) {
    ops.type = ST30_TYPE_RTP_LEVEL;
    s->st30_rtp_input = true;
  }
  if (ops.type == ST30_TYPE_RTP_LEVEL) {
    s->st30_rtp_input = true;
    if (ctx->tx_audio_rtp_ring_size > 0)
      ops.rtp_ring_size = ctx->tx_audio_rtp_ring_size;
    else
      ops.rtp_ring_size = 16;
  }

  handle = st30_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30_tx_create fail\n", __func__, idx);
    return -EIO;
  }

  s->handle = handle;
  strncpy(s->st30_source_url, audio ? audio->audio_url : ctx->tx_audio_url,
          sizeof(s->st30_source_url));

  ret = app_tx_audio_session_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_audio_session_open_source fail\n", __func__, idx);
    return ret;
  }

  ret = app_tx_audio_session_start_source(s);
  if (ret < 0) {
    app_tx_audio_session_close_source(s);
    err("%s(%d), app_tx_audio_session_start_source fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

int st_app_tx_audio_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_audio_session* s;
  if (!ctx->tx_audio_sessions) return 0;
  for (int i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    app_tx_audio_session_stop_source(s);
  }

  return 0;
}

int st_app_tx_audio_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_audio_session* s;
  ctx->tx_audio_sessions = (struct st_app_tx_audio_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_audio_session) * ctx->tx_audio_session_cnt);
  if (!ctx->tx_audio_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    s->idx = i;
    ret = app_tx_audio_session_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_audio[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_audio_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_audio_sessions_uinit(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_audio_session* s;
  if (!ctx->tx_audio_sessions) return 0;
  for (i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    if (s->handle) {
      ret = st30_tx_free(s->handle);
      if (ret < 0) err("%s(%d), st30_tx_free fail %d\n", __func__, i, ret);
      s->handle = NULL;
    }

    app_tx_audio_session_close_source(s);
    if (s->st30_ready_framebuff) {
      free(s->st30_ready_framebuff);
      s->st30_ready_framebuff = NULL;
    }
    if (s->st30_free_framebuff) {
      free(s->st30_free_framebuff);
      s->st30_free_framebuff = NULL;
    }
    pthread_mutex_destroy(&s->st30_wake_mutex);
    pthread_cond_destroy(&s->st30_wake_cond);
  }
  st_app_free(ctx->tx_audio_sessions);

  return 0;
}