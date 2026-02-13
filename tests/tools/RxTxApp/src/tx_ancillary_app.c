/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_ancillary_app.h"

static int app_tx_anc_next_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st40_tx_frame_meta* meta) {
  struct st_app_tx_anc_session* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  st_pthread_mutex_lock(&s->st40_wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, consumer_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, ST10_VIDEO_SAMPLING_RATE_90K));
    /* populate user timestamp if enabled */
    if (s->user_time) {
      double frame_time = s->expect_fps ? (NS_PER_S / s->expect_fps) : 0;
      bool restart_base_time = !s->local_tai_base_time;
      meta->timestamp = st_app_user_time(s->ctx, s->user_time, s->frame_num, frame_time,
                                         restart_base_time);
      meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
      s->local_tai_base_time = s->user_time->base_tai_time;
      s->frame_num++;
    }
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
  st_pthread_cond_signal(&s->st40_wake_cond);
  st_pthread_mutex_unlock(&s->st40_wake_mutex);
  return ret;
}

static int app_tx_anc_frame_done(void* priv, uint16_t frame_idx,
                                 struct st40_tx_frame_meta* meta) {
  struct st_app_tx_anc_session* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st40_wake_mutex);
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
  st_pthread_cond_signal(&s->st40_wake_cond);
  st_pthread_mutex_unlock(&s->st40_wake_mutex);

  s->st40_frame_done_cnt++;
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return ret;
}

static int app_tx_anc_rtp_done(void* priv) {
  struct st_app_tx_anc_session* s = priv;
  st_pthread_mutex_lock(&s->st40_wake_mutex);
  st_pthread_cond_signal(&s->st40_wake_cond);
  st_pthread_mutex_unlock(&s->st40_wake_mutex);
  s->st40_packet_done_cnt++;
  return 0;
}

static void app_tx_anc_build_frame(struct st_app_tx_anc_session* s,
                                   struct st40_frame* dst) {
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
}

static void* app_tx_anc_frame_thread(void* arg) {
  struct st_app_tx_anc_session* s = arg;
  int idx = s->idx;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40_app_thread_stop) {
    st_pthread_mutex_lock(&s->st40_wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->st40_app_thread_stop)
        st_pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
      st_pthread_mutex_unlock(&s->st40_wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->st40_wake_mutex);

    struct st40_frame* frame_addr = st40_tx_get_framebuffer(s->handle, producer_idx);
    app_tx_anc_build_frame(s, frame_addr);

    st_pthread_mutex_lock(&s->st40_wake_mutex);
    framebuff->size = sizeof(*frame_addr);
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->st40_wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void* app_tx_anc_pcap_thread(void* arg) {
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
      st_pthread_mutex_lock(&s->st40_wake_mutex);
      /* try again */
      mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st40_wake_mutex);
      } else {
        if (!s->st40_app_thread_stop)
          st_pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
        st_pthread_mutex_unlock(&s->st40_wake_mutex);
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
          udp_data_len = ntohs(udp_hdr->len) - sizeof(struct udphdr);
          mtl_memcpy(usrptr,
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

static void app_tx_anc_build_rtp(struct st_app_tx_anc_session* s, void* usrptr,
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
  hdr->first_hdr_chunk.anc_count = 1;
  hdr->base.payload_type = s->st40_payload_type;
  hdr->base.version = 2;
  hdr->base.extension = 0;
  hdr->base.padding = 0;
  hdr->base.csrc_count = 0;
  hdr->first_hdr_chunk.f = 0b00;
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
  payload_hdr->swapped_first_hdr_chunk = htonl(payload_hdr->swapped_first_hdr_chunk);
  payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
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

static void* app_tx_anc_rtp_thread(void* arg) {
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
      st_pthread_mutex_lock(&s->st40_wake_mutex);
      /* try again */
      mbuf = st40_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st40_wake_mutex);
      } else {
        if (!s->st40_app_thread_stop)
          st_pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
        st_pthread_mutex_unlock(&s->st40_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_anc_build_rtp(s, usrptr, &mbuf_len);

    st40_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_anc_open_source(struct st_app_tx_anc_session* s) {
  if (!s->st40_pcap_input) {
    struct stat i;

    s->st40_source_fd = st_open(s->st40_source_url, O_RDONLY);
    if (s->st40_source_fd >= 0) {
      if (fstat(s->st40_source_fd, &i) < 0) {
        err("%s, fstat %s fail\n", __func__, s->st40_source_url);
        close(s->st40_source_fd);
        s->st40_source_fd = -1;
        return -EIO;
      }

      uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, s->st40_source_fd, 0);

      if (MAP_FAILED != m) {
        s->st40_source_begin = m;
        s->st40_frame_cursor = m;
        s->st40_source_end = m + i.st_size;
      } else {
        err("%s, mmap fail '%s'\n", __func__, s->st40_source_url);
        close(s->st40_source_fd);
        s->st40_source_fd = -1;
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

static int app_tx_anc_close_source(struct st_app_tx_anc_session* s) {
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

static int app_tx_anc_start_source(struct st_app_tx_anc_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  s->st40_app_thread_stop = false;
  if (s->st40_pcap_input)
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_pcap_thread, (void*)s);
  else if (s->st40_rtp_input)
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_rtp_thread, (void*)s);
  else
    ret = pthread_create(&s->st40_app_thread, NULL, app_tx_anc_frame_thread, (void*)s);
  if (ret < 0) {
    err("%s(%d), thread create fail err = %d\n", __func__, idx, ret);
    return ret;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_anc_%d", idx);
  mtl_thread_setname(s->st40_app_thread, thread_name);

  return 0;
}

static void app_tx_anc_stop_source(struct st_app_tx_anc_session* s) {
  if (s->st40_source_fd >= 0) {
    s->st40_app_thread_stop = true;
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st40_wake_mutex);
    st_pthread_cond_signal(&s->st40_wake_cond);
    st_pthread_mutex_unlock(&s->st40_wake_mutex);
    if (s->st40_app_thread) (void)pthread_join(s->st40_app_thread, NULL);
  }
}

int app_tx_anc_uinit(struct st_app_tx_anc_session* s) {
  int ret;

  app_tx_anc_stop_source(s);

  if (s->handle) {
    ret = st40_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st_tx_anc_session_free fail %d\n", __func__, s->idx, ret);
    s->handle = NULL;
  }

  app_tx_anc_close_source(s);

  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }

  st_pthread_mutex_destroy(&s->st40_wake_mutex);
  st_pthread_cond_destroy(&s->st40_wake_cond);

  return 0;
}

static int app_tx_anc_init(struct st_app_context* ctx, st_json_ancillary_session_t* anc,
                           struct st_app_tx_anc_session* s) {
  int idx = s->idx, ret;
  struct st40_tx_ops ops;
  char name[32];
  st40_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->framebuff_cnt = 2;
  s->st40_seq_id = 1;

  s->framebuffs =
      (struct st_tx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) {
    return -ENOMEM;
  }
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].stat = ST_TX_FRAME_FREE;
    s->framebuffs[j].lines_ready = 0;
  }

  s->st40_source_fd = -1;
  st_pthread_mutex_init(&s->st40_wake_mutex, NULL);
  st_pthread_cond_init(&s->st40_wake_cond, NULL);

  snprintf(name, 32, "app_tx_ancillary%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = anc ? anc->base.num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P],
         anc ? st_json_ip(ctx, &anc->base, MTL_SESSION_PORT_P)
             : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           anc ? anc->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = anc ? anc->base.udp_port : (10200 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST40_TX_FLAG_USER_P_MAC;
  }
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R],
           anc ? st_json_ip(ctx, &anc->base, MTL_SESSION_PORT_R)
               : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             anc ? anc->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = anc ? anc->base.udp_port : (10200 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST40_TX_FLAG_USER_R_MAC;
    }
  }
  ops.get_next_frame = app_tx_anc_next_frame;
  ops.notify_frame_done = app_tx_anc_frame_done;
  ops.notify_rtp_done = app_tx_anc_rtp_done;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.fps = anc ? anc->info.anc_fps : ST_FPS_P59_94;
  s->st40_pcap_input = false;
  ops.type = anc ? anc->info.type : ST40_TYPE_FRAME_LEVEL;
  ops.interlaced = anc ? anc->info.interlaced : false;
  ops.payload_type = anc ? anc->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  s->st40_payload_type = ops.payload_type;
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
  if (anc && (anc->user_timestamp || anc->user_pacing)) {
    if (anc->user_pacing) ops.flags |= ST40_TX_FLAG_USER_PACING;
    if (anc->user_timestamp) ops.flags |= ST40_TX_FLAG_USER_TIMESTAMP;
    /* use global user time */
    s->ctx = ctx;
    s->user_time = &ctx->user_time;
    s->frame_num = 0;
    s->local_tai_base_time = 0;
    s->expect_fps = st_frame_rate(ops.fps);
  }
  if (anc && anc->exact_user_pacing) ops.flags |= ST40_TX_FLAG_EXACT_USER_PACING;
  if (anc && anc->enable_rtcp) ops.flags |= ST40_TX_FLAG_ENABLE_RTCP;
  if (ctx->tx_anc_dedicate_queue) ops.flags |= ST40_TX_FLAG_DEDICATE_QUEUE;

  handle = st40_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40_tx_create fail\n", __func__, idx);
    app_tx_anc_uinit(s);
    return -EIO;
  }

  s->handle = handle;
  snprintf(s->st40_source_url, sizeof(s->st40_source_url), "%s",
           anc ? anc->info.anc_url : ctx->tx_anc_url);

  ret = app_tx_anc_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_anc_session_open_source fail\n", __func__, idx);
    app_tx_anc_uinit(s);
    return ret;
  }

  ret = app_tx_anc_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_anc_session_start_source fail %d\n", __func__, idx, ret);
    app_tx_anc_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_anc_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_anc_session* s;
  if (!ctx->tx_anc_sessions) return 0;
  for (int i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    app_tx_anc_stop_source(s);
  }

  return 0;
}

int st_app_tx_anc_sessions_init(struct st_app_context* ctx) {
  int ret;
  struct st_app_tx_anc_session* s;
  ctx->tx_anc_sessions = (struct st_app_tx_anc_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_anc_session) * ctx->tx_anc_session_cnt);
  if (!ctx->tx_anc_sessions) return -ENOMEM;

  for (int i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    s->idx = i;
    ret = app_tx_anc_init(ctx, ctx->json_ctx ? &ctx->json_ctx->tx_anc_sessions[i] : NULL,
                          s);
    if (ret < 0) {
      err("%s(%d), app_tx_anc_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_anc_sessions_uinit(struct st_app_context* ctx) {
  struct st_app_tx_anc_session* s;
  if (!ctx->tx_anc_sessions) return 0;

  for (int i = 0; i < ctx->tx_anc_session_cnt; i++) {
    s = &ctx->tx_anc_sessions[i];
    app_tx_anc_uinit(s);
  }
  st_app_free(ctx->tx_anc_sessions);

  return 0;
}
