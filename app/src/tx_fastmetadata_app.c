/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_fastmetadata_app.h"

#define ST_PKT_ST41_PAYLOAD_MAX_BYTES (1460 - sizeof(struct st41_rtp_hdr) - 8)

static int app_tx_fmd_next_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st41_tx_frame_meta* meta) {
  struct st_app_tx_fmd_session* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st41_wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u, epoch %" PRIu64 ", tai %" PRIu64 "\n", __func__,
        s->idx, consumer_idx, meta->epoch,
        st10_get_tai(meta->tfmt, meta->timestamp, ST10_VIDEO_SAMPLING_RATE_90K));
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
  st_pthread_cond_signal(&s->st41_wake_cond);
  st_pthread_mutex_unlock(&s->st41_wake_mutex);
  return ret;
}

static int app_tx_fmd_frame_done(void* priv, uint16_t frame_idx,
                                 struct st41_tx_frame_meta* meta) {
  struct st_app_tx_fmd_session* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];
  MTL_MAY_UNUSED(meta);

  st_pthread_mutex_lock(&s->st41_wake_mutex);
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
  st_pthread_cond_signal(&s->st41_wake_cond);
  st_pthread_mutex_unlock(&s->st41_wake_mutex);

  s->st41_frame_done_cnt++;
  dbg("%s(%d), framebuffer index %d\n", __func__, s->idx, frame_idx);
  return ret;
}

static int app_tx_fmd_rtp_done(void* priv) {
  struct st_app_tx_fmd_session* s = priv;
  st_pthread_mutex_lock(&s->st41_wake_mutex);
  st_pthread_cond_signal(&s->st41_wake_cond);
  st_pthread_mutex_unlock(&s->st41_wake_mutex);
  s->st41_packet_done_cnt++;
  return 0;
}

static void app_tx_fmd_build_frame(struct st_app_tx_fmd_session* s,
                                   struct st41_frame* dst) {
  uint16_t data_item_length_bytes = s->st41_source_end - s->st41_frame_cursor > ST_PKT_ST41_PAYLOAD_MAX_BYTES
                          ? ST_PKT_ST41_PAYLOAD_MAX_BYTES
                          : s->st41_source_end - s->st41_frame_cursor;
  dst->data_item_length_bytes = data_item_length_bytes;
  dst->data = s->st41_frame_cursor;
  s->st41_frame_cursor += data_item_length_bytes;
  if (s->st41_frame_cursor == s->st41_source_end)
    s->st41_frame_cursor = s->st41_source_begin;
}

static void* app_tx_fmd_frame_thread(void* arg) {
  struct st_app_tx_fmd_session* s = arg;
  int idx = s->idx;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st41_app_thread_stop) {
    st_pthread_mutex_lock(&s->st41_wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->st41_app_thread_stop)
        st_pthread_cond_wait(&s->st41_wake_cond, &s->st41_wake_mutex);
      st_pthread_mutex_unlock(&s->st41_wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->st41_wake_mutex);

    struct st41_frame* frame_addr = st41_tx_get_framebuffer(s->handle, producer_idx);
    app_tx_fmd_build_frame(s, frame_addr);

    st_pthread_mutex_lock(&s->st41_wake_mutex);
    framebuff->size = sizeof(*frame_addr);
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->st41_wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void* app_tx_fmd_pcap_thread(void* arg) {
  struct st_app_tx_fmd_session* s = arg;
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
  while (!s->st41_app_thread_stop) {
    /* get available buffer*/
    mbuf = st41_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&s->st41_wake_mutex);
      /* try again */
      mbuf = st41_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st41_wake_mutex);
      } else {
        if (!s->st41_app_thread_stop)
          st_pthread_cond_wait(&s->st41_wake_cond, &s->st41_wake_mutex);
        st_pthread_mutex_unlock(&s->st41_wake_mutex);
        continue;
      }
    }
    udp_data_len = 0;
    packet = (uint8_t*)pcap_next(s->st41_pcap, &hdr);
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
      pcap_close(s->st41_pcap);
      /* open capture file for offline processing */
      s->st41_pcap = pcap_open_offline(s->st41_source_url, err_buf);
      if (s->st41_pcap == NULL) {
        err("pcap_open_offline %s() failed: %s\n:", s->st41_source_url, err_buf);
        return NULL;
      }
    }

    st41_tx_put_mbuf(s->handle, mbuf, udp_data_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_tx_fmd_build_rtp(struct st_app_tx_fmd_session* s, void* usrptr,
                                 uint16_t* mbuf_len) {
  /* generate one fmd rtp for test purpose */
  struct st41_rtp_hdr* hdr = (struct st41_rtp_hdr*)usrptr;
  uint8_t* payload_hdr = (uint8_t*)(&hdr[1]);
  uint16_t data_item_length_bytes = s->st41_source_end - s->st41_frame_cursor > (MTL_PKT_MAX_RTP_BYTES-16) 
                          ? (MTL_PKT_MAX_RTP_BYTES-16)
                          : s->st41_source_end - s->st41_frame_cursor;
  uint16_t data_item_length;
  data_item_length = (data_item_length_bytes + 3) / 4; /* expressed in number of 4-byte words */
  hdr->base.marker = 1;
  hdr->base.payload_type = ST_APP_PAYLOAD_TYPE_FASTMETADATA;
  hdr->base.version = 2;
  hdr->base.extension = 0;
  hdr->base.padding = 0;
  hdr->base.csrc_count = 0;
  hdr->base.tmstamp = s->st41_rtp_tmstamp;
  hdr->base.ssrc = htonl(0x88888888 + s->idx);
  /* update rtp seq*/
  hdr->base.seq_number = htons((uint16_t)s->st41_seq_id);
  s->st41_seq_id++;
  s->st41_rtp_tmstamp++;

  for (int i = 0; i < data_item_length_bytes; i++) {
    payload_hdr[i] = s->st41_frame_cursor[i];
  }
  /* filling with 0's the remianing bytes of last 4-byte word */
  for (int i = data_item_length_bytes; i < data_item_length * 4; i++) {
    payload_hdr[i] = 0;
  }

  *mbuf_len = sizeof(struct st41_rtp_hdr) + data_item_length * 4;
  hdr->st41_hdr_chunk.data_item_length = data_item_length;
  hdr->st41_hdr_chunk.data_item_type = s->st41_dit;
  hdr->st41_hdr_chunk.data_item_k_bit = s->st41_k_bit;
  hdr->swaped_st41_hdr_chunk = htonl(hdr->swaped_st41_hdr_chunk);
  
  s->st41_frame_cursor += data_item_length_bytes;
  if (s->st41_frame_cursor == s->st41_source_end)
    s->st41_frame_cursor = s->st41_source_begin;
}

static void* app_tx_fmd_rtp_thread(void* arg) {
  struct st_app_tx_fmd_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st41_app_thread_stop) {
    /* get available buffer*/
    mbuf = st41_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      st_pthread_mutex_lock(&s->st41_wake_mutex);
      /* try again */
      mbuf = st41_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        st_pthread_mutex_unlock(&s->st41_wake_mutex);
      } else {
        if (!s->st41_app_thread_stop)
          st_pthread_cond_wait(&s->st41_wake_cond, &s->st41_wake_mutex);
        st_pthread_mutex_unlock(&s->st41_wake_mutex);
        continue;
      }
    }

    /* build the rtp pkt */
    app_tx_fmd_build_rtp(s, usrptr, &mbuf_len);

    st41_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_fmd_open_source(struct st_app_tx_fmd_session* s) {
  if (!s->st41_pcap_input) {
    struct stat i;

    s->st41_source_fd = st_open(s->st41_source_url, O_RDONLY);
    if (s->st41_source_fd >= 0) {
      if (fstat(s->st41_source_fd, &i) < 0) {
        err("%s, fstat %s fail\n", __func__, s->st41_source_url);
        close(s->st41_source_fd);
        s->st41_source_fd = -1;
        return -EIO;
      }

      uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, s->st41_source_fd, 0);

      if (MAP_FAILED != m) {
        s->st41_source_begin = m;
        s->st41_frame_cursor = m;
        s->st41_source_end = m + i.st_size;
      } else {
        err("%s, mmap fail '%s'\n", __func__, s->st41_source_url);
        close(s->st41_source_fd);
        s->st41_source_fd = -1;
        return -EIO;
      }
    } else {
      err("%s, open fail '%s'\n", __func__, s->st41_source_url);
      return -EIO;
    }
  } else {
    char err_buf[PCAP_ERRBUF_SIZE];

    /* open capture file for offline processing */
    s->st41_pcap = pcap_open_offline(s->st41_source_url, err_buf);
    if (!s->st41_pcap) {
      err("pcap_open_offline %s() failed: %s\n:", s->st41_source_url, err_buf);
      return -EIO;
    }
  }

  return 0;
}

static int app_tx_fmd_close_source(struct st_app_tx_fmd_session* s) {
  if (s->st41_source_fd >= 0) {
    munmap(s->st41_source_begin, s->st41_source_end - s->st41_source_begin);
    close(s->st41_source_fd);
    s->st41_source_fd = -1;
  }
  if (s->st41_pcap) {
    pcap_close(s->st41_pcap);
    s->st41_pcap = NULL;
  }

  return 0;
}

static int app_tx_fmd_start_source(struct st_app_tx_fmd_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  s->st41_app_thread_stop = false;
  if (s->st41_pcap_input)
    ret = pthread_create(&s->st41_app_thread, NULL, app_tx_fmd_pcap_thread, (void*)s);
  else if (s->st41_rtp_input)
    ret = pthread_create(&s->st41_app_thread, NULL, app_tx_fmd_rtp_thread, (void*)s);
  else
    ret = pthread_create(&s->st41_app_thread, NULL, app_tx_fmd_frame_thread, (void*)s);
  if (ret < 0) {
    err("%s(%d), thread create fail err = %d\n", __func__, idx, ret);
    return ret;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_fmd_%d", idx);
  mtl_thread_setname(s->st41_app_thread, thread_name);

  return 0;
}

static void app_tx_fmd_stop_source(struct st_app_tx_fmd_session* s) {
  if (s->st41_source_fd >= 0) {
    s->st41_app_thread_stop = true;
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st41_wake_mutex);
    st_pthread_cond_signal(&s->st41_wake_cond);
    st_pthread_mutex_unlock(&s->st41_wake_mutex);
    if (s->st41_app_thread) (void)pthread_join(s->st41_app_thread, NULL);
  }
}

int app_tx_fmd_uinit(struct st_app_tx_fmd_session* s) {
  int ret;

  app_tx_fmd_stop_source(s);

  if (s->handle) {
    ret = st41_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st_tx_fmd_session_free fail %d\n", __func__, s->idx, ret);
    s->handle = NULL;
  }

  app_tx_fmd_close_source(s);

  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }

  st_pthread_mutex_destroy(&s->st41_wake_mutex);
  st_pthread_cond_destroy(&s->st41_wake_cond);

  return 0;
}

static int app_tx_fmd_init(struct st_app_context* ctx, st_json_fastmetadata_session_t* fmd,
                           struct st_app_tx_fmd_session* s) {
  int idx = s->idx, ret;
  struct st41_tx_ops ops;
  char name[32];
  st41_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->framebuff_cnt = 2;
  s->st41_seq_id = 1;

  s->framebuffs =
      (struct st_tx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) {
    return -ENOMEM;
  }
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].stat = ST_TX_FRAME_FREE;
    s->framebuffs[j].lines_ready = 0;
  }

  s->st41_source_fd = -1;
  st_pthread_mutex_init(&s->st41_wake_mutex, NULL);
  st_pthread_cond_init(&s->st41_wake_cond, NULL);

  snprintf(name, 32, "app_tx_fastmetadata%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = fmd ? fmd->base.num_inf : ctx->para.num_ports;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P],
         fmd ? st_json_ip(ctx, &fmd->base, MTL_SESSION_PORT_P)
             : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           fmd ? fmd->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = fmd ? fmd->base.udp_port : (10200 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST41_TX_FLAG_USER_P_MAC;
  }
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R],
           fmd ? st_json_ip(ctx, &fmd->base, MTL_SESSION_PORT_R)
               : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             fmd ? fmd->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = fmd ? fmd->base.udp_port : (10200 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST41_TX_FLAG_USER_R_MAC;
    }
  }
  ops.get_next_frame = app_tx_fmd_next_frame;
  ops.notify_frame_done = app_tx_fmd_frame_done;
  ops.notify_rtp_done = app_tx_fmd_rtp_done;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.fps = fmd ? fmd->info.fmd_fps : ST_FPS_P59_94;
  ops.fmd_dit = fmd->info.fmd_dit;
  ops.fmd_k_bit = fmd->info.fmd_k_bit;
  s->st41_pcap_input = false;
  ops.type = fmd ? fmd->info.type : ST41_TYPE_FRAME_LEVEL;
  ops.interlaced = fmd ? fmd->info.interlaced : false;
  ops.payload_type = fmd ? fmd->base.payload_type : ST_APP_PAYLOAD_TYPE_FASTMETADATA;
  /* select rtp type for pcap file or tx_video_rtp_ring_size */
  if (strstr(s->st41_source_url, ".pcap")) {
    ops.type = ST41_TYPE_RTP_LEVEL;
    s->st41_pcap_input = true;
  } else if (ctx->tx_fmd_rtp_ring_size > 0) {
    ops.type = ST41_TYPE_RTP_LEVEL;
    s->st41_rtp_input = true;
  }
  if (ops.type == ST41_TYPE_RTP_LEVEL) {
    s->st41_rtp_input = true;
    if (ctx->tx_fmd_rtp_ring_size > 0)
      ops.rtp_ring_size = ctx->tx_fmd_rtp_ring_size;
    else
      ops.rtp_ring_size = 16;
  }
  if (fmd && fmd->enable_rtcp) ops.flags |= ST41_TX_FLAG_ENABLE_RTCP;
  if (ctx->tx_fmd_dedicate_queue) ops.flags |= ST41_TX_FLAG_DEDICATE_QUEUE;

  handle = st41_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st41_tx_create fail\n", __func__, idx);
    app_tx_fmd_uinit(s);
    return -EIO;
  }

  /* copying frame fields for RTP mode to function*/
  s->st41_dit = fmd->info.fmd_dit;
  s->st41_k_bit = fmd->info.fmd_k_bit;

  s->handle = handle;
  snprintf(s->st41_source_url, sizeof(s->st41_source_url), "%s",
           fmd ? fmd->info.fmd_url : ctx->tx_fmd_url);

  ret = app_tx_fmd_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_fmd_session_open_source fail\n", __func__, idx);
    app_tx_fmd_uinit(s);
    return ret;
  }

  ret = app_tx_fmd_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_fmd_session_start_source fail %d\n", __func__, idx, ret);
    app_tx_fmd_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_fmd_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_fmd_session* s;
  if (!ctx->tx_fmd_sessions) return 0;
  for (int i = 0; i < ctx->tx_fmd_session_cnt; i++) {
    s = &ctx->tx_fmd_sessions[i];
    app_tx_fmd_stop_source(s);
  }

  return 0;
}

int st_app_tx_fmd_sessions_init(struct st_app_context* ctx) {
  int ret;
  struct st_app_tx_fmd_session* s;
  ctx->tx_fmd_sessions = (struct st_app_tx_fmd_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_fmd_session) * ctx->tx_fmd_session_cnt);
  if (!ctx->tx_fmd_sessions) return -ENOMEM;

  for (int i = 0; i < ctx->tx_fmd_session_cnt; i++) {
    s = &ctx->tx_fmd_sessions[i];
    s->idx = i;
    ret = app_tx_fmd_init(ctx, ctx->json_ctx ? &ctx->json_ctx->tx_fmd_sessions[i] : NULL,
                          s);
    if (ret < 0) {
      err("%s(%d), app_tx_fmd_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_fmd_sessions_uinit(struct st_app_context* ctx) {
  struct st_app_tx_fmd_session* s;
  if (!ctx->tx_fmd_sessions) return 0;

  for (int i = 0; i < ctx->tx_fmd_session_cnt; i++) {
    s = &ctx->tx_fmd_sessions[i];
    app_tx_fmd_uinit(s);
  }
  st_app_free(ctx->tx_fmd_sessions);

  return 0;
}
