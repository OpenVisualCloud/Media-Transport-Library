/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_audio_app.h"

static int app_tx_audio_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st30_tx_frame_meta* meta) {
  struct st_app_tx_audio_session* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st30_wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, consumer_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, st30_get_sample_rate(s->sampling)));
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
  } else {
    /* not ready */
    dbg("%s(%d), idx %u err stat %d\n", __func__, s->idx, consumer_idx, framebuff->stat);
    ret = -EIO;
  }
  st_pthread_cond_signal(&s->st30_wake_cond);
  st_pthread_mutex_unlock(&s->st30_wake_mutex);
  return ret;
}

static int app_tx_audio_frame_done(void* priv, uint16_t frame_idx,
                                   struct st30_tx_frame_meta* meta) {
  struct st_app_tx_audio_session* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st30_wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    dbg("%s(%d), done frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, frame_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, st30_get_sample_rate(s->sampling)));
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
        frame_idx);
  }
  st_pthread_cond_signal(&s->st30_wake_cond);
  st_pthread_mutex_unlock(&s->st30_wake_mutex);

  s->st30_frame_done_cnt++;
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return ret;
}

static int app_tx_audio_rtp_done(void* priv) {
  struct st_app_tx_audio_session* s = priv;
  st_pthread_mutex_lock(&s->st30_wake_mutex);
  st_pthread_cond_signal(&s->st30_wake_cond);
  st_pthread_mutex_unlock(&s->st30_wake_mutex);
  s->st30_packet_done_cnt++;
  return 0;
}

static void app_tx_audio_build_frame(struct st_app_tx_audio_session* s, void* frame,
                                     size_t frame_size) {
  uint8_t* src = s->st30_frame_cursor;
  uint8_t* dst = frame;

  if (s->st30_frame_cursor + frame_size > s->st30_source_end) {
    /* reset to the start */
    s->st30_frame_cursor = s->st30_source_begin;
    mtl_memcpy(dst, s->st30_frame_cursor, s->st30_frame_size);
    s->st30_frame_cursor += s->st30_frame_size;
  } else {
    mtl_memcpy(dst, src, s->st30_frame_size);
    s->st30_frame_cursor += s->st30_frame_size;
  }
}

static void* app_tx_audio_frame_thread(void* arg) {
  struct st_app_tx_audio_session* s = arg;
  int idx = s->idx;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st30_app_thread_stop) {
    st_pthread_mutex_lock(&s->st30_wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->st30_app_thread_stop)
        st_pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
      st_pthread_mutex_unlock(&s->st30_wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->st30_wake_mutex);

    void* frame_addr = st30_tx_get_framebuffer(s->handle, producer_idx);
    app_tx_audio_build_frame(s, frame_addr, s->st30_frame_size);

    st_pthread_mutex_lock(&s->st30_wake_mutex);
    framebuff->size = s->st30_frame_size;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->st30_wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void* app_tx_audio_pcap_thread(void* arg) {
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
      st_pthread_mutex_lock(&s->st30_wake_mutex);
      /* try again */
      mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st30_wake_mutex);
      } else {
        if (!s->st30_app_thread_stop)
          st_pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
        st_pthread_mutex_unlock(&s->st30_wake_mutex);
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
          udp_data_len = ntohs(udp_hdr->len) - sizeof(struct udphdr);
          mtl_memcpy(usrptr,
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

static void app_tx_audio_build_rtp(struct st_app_tx_audio_session* s, void* usrptr,
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
  rtp->payload_type = ST_APP_PAYLOAD_TYPE_AUDIO;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->st30_rtp_tmstamp;
  s->st30_rtp_tmstamp++;
  rtp->seq_number = htons(s->st30_seq_id);
  s->st30_seq_id++;

  if (s->st30_frame_cursor + s->pkt_len > s->st30_source_end) {
    mtl_memcpy(payload, s->st30_source_begin, s->pkt_len);
    s->st30_frame_cursor = s->st30_source_begin + s->pkt_len;
  } else {
    mtl_memcpy(payload, s->st30_frame_cursor, s->pkt_len);
    s->st30_frame_cursor += s->pkt_len;
  }
  *mbuf_len = sizeof(struct st_rfc3550_rtp_hdr) + s->pkt_len;
}

static void* app_tx_audio_rtp_thread(void* arg) {
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
      st_pthread_mutex_lock(&s->st30_wake_mutex);
      /* try again */
      mbuf = st30_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st30_wake_mutex);
      } else {
        if (!s->st30_app_thread_stop)
          st_pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
        st_pthread_mutex_unlock(&s->st30_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_audio_build_rtp(s, usrptr, &mbuf_len);

    st30_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_audio_open_source(struct st_app_tx_audio_session* s) {
  if (!s->st30_pcap_input) {
    struct stat i;

    s->st30_source_fd = st_open(s->st30_source_url, O_RDONLY);
    if (s->st30_source_fd >= 0) {
      if (fstat(s->st30_source_fd, &i) < 0) {
        err("%s, fstat %s fail\n", __func__, s->st30_source_url);
        close(s->st30_source_fd);
        s->st30_source_fd = -1;
        return -EIO;
      }

      uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, s->st30_source_fd, 0);

      if (MAP_FAILED != m) {
        s->st30_source_begin = m;
        s->st30_frame_cursor = m;
        s->st30_source_end = m + i.st_size;
      } else {
        err("%s, mmap fail '%s'\n", __func__, s->st30_source_url);
        close(s->st30_source_fd);
        s->st30_source_fd = -1;
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

static int app_tx_audio_close_source(struct st_app_tx_audio_session* s) {
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

static int app_tx_audio_start_source(struct st_app_tx_audio_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  s->st30_app_thread_stop = false;
  if (s->st30_pcap_input)
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_pcap_thread, (void*)s);
  else if (s->st30_rtp_input)
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_rtp_thread, (void*)s);
  else
    ret = pthread_create(&s->st30_app_thread, NULL, app_tx_audio_frame_thread, (void*)s);

  if (ret < 0) {
    err("%s(%d), thread create fail err = %d\n", __func__, idx, ret);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_audio_%d", idx);
  mtl_thread_setname(s->st30_app_thread, thread_name);

  return 0;
}

static void app_tx_audio_stop_source(struct st_app_tx_audio_session* s) {
  if (s->st30_source_fd >= 0) {
    s->st30_app_thread_stop = true;
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st30_wake_mutex);
    st_pthread_cond_signal(&s->st30_wake_cond);
    st_pthread_mutex_unlock(&s->st30_wake_mutex);
    if (s->st30_app_thread) (void)pthread_join(s->st30_app_thread, NULL);
  }
}

static int app_tx_audio_uinit(struct st_app_tx_audio_session* s) {
  int ret;

  app_tx_audio_stop_source(s);

  if (s->handle) {
    ret = st30_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_tx_free fail %d\n", __func__, s->idx, ret);
    s->handle = NULL;
  }

  app_tx_audio_close_source(s);
  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }
  st_pthread_mutex_destroy(&s->st30_wake_mutex);
  st_pthread_cond_destroy(&s->st30_wake_cond);

  return 0;
}

static int app_tx_audio_init(struct st_app_context* ctx, st_json_audio_session_t* audio,
                             struct st_app_tx_audio_session* s) {
  int idx = s->idx, ret;
  struct st30_tx_ops ops;
  char name[32];
  st30_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->framebuff_cnt = 2;
  s->st30_seq_id = 1;

  s->framebuffs =
      (struct st_tx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) {
    return -ENOMEM;
  }
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].stat = ST_TX_FRAME_FREE;
    s->framebuffs[j].lines_ready = 0;
  }

  s->st30_source_fd = -1;
  st_pthread_mutex_init(&s->st30_wake_mutex, NULL);
  st_pthread_cond_init(&s->st30_wake_cond, NULL);

  snprintf(name, 32, "app_tx_audio%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = audio ? audio->base.num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P],
         audio ? st_json_ip(ctx, &audio->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(
      ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      audio ? audio->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = audio ? audio->base.udp_port : (10100 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST30_TX_FLAG_USER_P_MAC;
  }
  if (ctx->tx_audio_build_pacing) ops.flags |= ST30_TX_FLAG_BUILD_PACING;
  ops.pacing_way = ctx->tx_audio_pacing_way;
  if (ctx->tx_audio_fifo_size) ops.fifo_size = ctx->tx_audio_fifo_size;
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R],
           audio ? st_json_ip(ctx, &audio->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(
        ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        audio ? audio->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = audio ? audio->base.udp_port : (10100 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST30_TX_FLAG_USER_R_MAC;
    }
  }
  ops.get_next_frame = app_tx_audio_next_frame;
  ops.notify_frame_done = app_tx_audio_frame_done;
  ops.notify_rtp_done = app_tx_audio_rtp_done;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.fmt = audio ? audio->info.audio_format : ST30_FMT_PCM16;
  ops.channel = audio ? audio->info.audio_channel : 2;
  ops.sampling = audio ? audio->info.audio_sampling : ST30_SAMPLING_48K;
  ops.ptime = audio ? audio->info.audio_ptime : ST30_PTIME_1MS;
  s->sampling = ops.sampling;
  s->pkt_len = st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);
  if (s->pkt_len < 0) {
    err("%s(%d), st30_get_packet_size fail\n", __func__, idx);
    app_tx_audio_uinit(s);
    return -EIO;
  }
  int pkt_per_frame = 1;

  double pkt_time = st30_get_packet_time(ops.ptime);
  /* when ptime <= 1ms, set frame time to 1ms */
  if (pkt_time < NS_PER_MS) {
    pkt_per_frame = NS_PER_MS / pkt_time;
  }

  s->st30_frame_size = pkt_per_frame * s->pkt_len;
  ops.framebuff_size = s->st30_frame_size;
  ops.payload_type = audio ? audio->base.payload_type : ST_APP_PAYLOAD_TYPE_AUDIO;

  s->st30_pcap_input = false;
  ops.type = audio ? audio->info.type : ST30_TYPE_FRAME_LEVEL;
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
  if (audio && audio->enable_rtcp) ops.flags |= ST30_TX_FLAG_ENABLE_RTCP;
  ops.rl_accuracy_ns = ctx->tx_audio_rl_accuracy_us * 1000;

  handle = st30_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30_tx_create fail\n", __func__, idx);
    app_tx_audio_uinit(s);
    return -EIO;
  }

  s->handle = handle;
  snprintf(s->st30_source_url, sizeof(s->st30_source_url), "%s",
           audio ? audio->info.audio_url : ctx->tx_audio_url);

  ret = app_tx_audio_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_audio_session_open_source fail\n", __func__, idx);
    app_tx_audio_uinit(s);
    return ret;
  }

  ret = app_tx_audio_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_audio_session_start_source fail %d\n", __func__, idx, ret);
    app_tx_audio_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_audio_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_audio_session* s;
  if (!ctx->tx_audio_sessions) return 0;
  for (int i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    app_tx_audio_stop_source(s);
  }

  return 0;
}

int st_app_tx_audio_sessions_init(struct st_app_context* ctx) {
  int ret;
  struct st_app_tx_audio_session* s;
  ctx->tx_audio_sessions = (struct st_app_tx_audio_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_audio_session) * ctx->tx_audio_session_cnt);
  if (!ctx->tx_audio_sessions) return -ENOMEM;

  for (int i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    s->idx = i;
    ret = app_tx_audio_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_audio_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_audio_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_audio_sessions_uinit(struct st_app_context* ctx) {
  struct st_app_tx_audio_session* s;

  if (!ctx->tx_audio_sessions) return 0;

  for (int i = 0; i < ctx->tx_audio_session_cnt; i++) {
    s = &ctx->tx_audio_sessions[i];
    app_tx_audio_uinit(s);
  }
  st_app_free(ctx->tx_audio_sessions);

  return 0;
}