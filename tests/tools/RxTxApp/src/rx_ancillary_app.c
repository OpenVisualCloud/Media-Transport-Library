/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_ancillary_app.h"

static void app_rx_anc_handle_rtp(struct st_app_rx_anc_session* s, void* usrptr) {
  struct st40_rfc8331_rtp_hdr* hdr = (struct st40_rfc8331_rtp_hdr*)usrptr;
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);

  int anc_count = hdr->first_hdr_chunk.anc_count;
  int idx, payload_len;
  dbg("%s(%d), anc_count %d\n", __func__, s->idx, anc_count);

  for (idx = 0; idx < anc_count; idx++) {
    st40_rfc8331_payload_hdr_bswap(payload_hdr);
    if (!st40_check_parity_bits(payload_hdr->second_hdr_chunk.did) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.sdid) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.data_count)) {
      if (!s->stat_pkt_invalid)
        err("%s(%d), anc RTP checkParityBits error\n", __func__, s->idx);
      s->stat_pkt_invalid++;
      return;
    }
    int udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;

    // verify checksum
    uint16_t checksum = 0;
    checksum = st40_get_udw(udw_size + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    payload_hdr->swapped_second_hdr_chunk = htonl(payload_hdr->swapped_second_hdr_chunk);
    if (checksum !=
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk)) {
      if (!s->stat_pkt_invalid)
        err("%s(%d), anc frame checksum error\n", __func__, s->idx);
      s->stat_pkt_invalid++;
      return;
    }
    // get payload
#ifdef DEBUG
    uint16_t data;
    for (int i = 0; i < udw_size; i++) {
      data = st40_get_udw(i + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
      if (!st40_check_parity_bits(data)) err("anc udw checkParityBits error\n");
      dbg("%c", data & 0xff);
    }
    dbg("\n");
#endif
    payload_len = st40_rfc8331_payload_bytes(udw_size);  // Full size of one ANC
    payload_hdr = (struct st40_rfc8331_payload_hdr*)((uint8_t*)payload_hdr + payload_len);
  }

  s->stat_frame_total_received++;
  if (!s->stat_frame_first_rx_time)
    s->stat_frame_first_rx_time = st_app_get_monotonic_time();
}

static bool app_rx_anc_fmd_ref_match(struct st_app_rx_anc_session* s, size_t off,
                                     uint8_t* item, uint16_t padded_size) {
  size_t size = ST_MIN(ST_APP_FMD_CHUNK_BYTES, s->st40_fmd_ref_size - off);
  if (((size + 3) & ~3) != padded_size) return false;
  if (memcmp(s->st40_fmd_ref + off, item, size)) return false;
  for (size_t i = size; i < padded_size; i++)
    if (item[i]) return false;
  return true;
}

static int64_t app_rx_anc_fmd_ref_after(struct st_app_rx_anc_session* s, size_t off) {
  off += ST_APP_FMD_CHUNK_BYTES;
  return off < s->st40_fmd_ref_size ? (int64_t)off : 0;
}

/* TX cuts the file at chunk boundaries: resync on one only when unsynced or after an rtp
 * seq gap, and only on an item that matches no other chunk */
static int app_rx_anc_fmd_ref_check(struct st_app_rx_anc_session* s, uint16_t seq,
                                    uint8_t* item, uint16_t padded_size) {
  int64_t off = s->st40_fmd_ref_next;
  bool in_seq = off >= 0 && seq == (uint16_t)(s->st40_fmd_last_seq + 1);

  s->st40_fmd_last_seq = seq;
  if (in_seq) {
    s->st40_fmd_ref_next = app_rx_anc_fmd_ref_after(s, off);
    return app_rx_anc_fmd_ref_match(s, off, item, padded_size) ? 0 : -EIO;
  }

  int64_t found = -1;
  s->st40_fmd_ref_next = -1;
  for (size_t i = 0; i < s->st40_fmd_ref_size; i += ST_APP_FMD_CHUNK_BYTES) {
    if (!app_rx_anc_fmd_ref_match(s, i, item, padded_size)) continue;
    if (found >= 0) return 0;
    found = (int64_t)i;
  }
  if (found < 0) return -EIO;
  s->st40_fmd_ref_next = app_rx_anc_fmd_ref_after(s, found);
  return 0;
}

static void app_rx_anc_handle_fmd(struct st_app_rx_anc_session* s, void* usrptr,
                                  uint16_t len) {
  struct st40_fmd_rtp_hdr hdr;
  uint8_t* item = (uint8_t*)usrptr + sizeof(hdr);
  const char* fail = NULL;

  if (len == sizeof(hdr.base)) {
    memcpy(&hdr.base, usrptr, sizeof(hdr.base));
    if (hdr.base.version != 2)
      fail = "RTP version";
    else if (hdr.base.marker)
      fail = "marker bit";
    else if (hdr.base.csrc_count || hdr.base.extension || hdr.base.padding)
      fail = "truncated RTP header";
  } else if (len < sizeof(hdr)) {
    fail = "short packet";
  } else if (len > ST_APP_FMD_MAX_RTP_BYTES) {
    fail = "UDP size limit";
  } else {
    memcpy(&hdr, usrptr, sizeof(hdr));
    hdr.swapped_fmd_hdr_chunk = ntohl(hdr.swapped_fmd_hdr_chunk);
    uint16_t padded_size = hdr.fmd_hdr_chunk.data_item_length * 4;
    if (hdr.base.version != 2)
      fail = "RTP version";
    else if (hdr.base.marker)
      fail = "marker bit";
    else if (!padded_size)
      fail = "zero data item length";
    else if (len != sizeof(hdr) + padded_size)
      fail = "data item length";
    else if (s->st40_fmd_dit >= 0 && hdr.fmd_hdr_chunk.data_item_type != s->st40_fmd_dit)
      fail = "data item type";
    else if (s->st40_fmd_k_bit >= 0 &&
             hdr.fmd_hdr_chunk.data_item_k_bit != s->st40_fmd_k_bit)
      fail = "K-bit";
    else if (s->st40_fmd_ref && app_rx_anc_fmd_ref_check(s, ntohs(hdr.base.seq_number),
                                                         item, padded_size) < 0)
      fail = "reference file compare";
  }
  if (fail) {
    if (!s->stat_pkt_invalid)
      err("%s(%d), fast metadata %s error\n", __func__, s->idx, fail);
    s->stat_pkt_invalid++;
    return;
  }

  uint64_t now = st_app_get_monotonic_time();
  if (s->st40_fmd_last_rx_time)
    s->st40_fmd_max_gap_ns =
        ST_MAX(s->st40_fmd_max_gap_ns, now - s->st40_fmd_last_rx_time);
  s->st40_fmd_last_rx_time = now;
  if (len == sizeof(hdr.base)) {
    /* a zero-item packet keeps the stream alive but is no frame */
    uint16_t seq = ntohs(hdr.base.seq_number);
    if (seq != (uint16_t)(s->st40_fmd_last_seq + 1)) s->st40_fmd_ref_next = -1;
    s->st40_fmd_last_seq = seq;
    return;
  }
  s->stat_frame_total_received++;
  if (!s->stat_frame_first_rx_time) s->stat_frame_first_rx_time = now;
}

static void* app_rx_anc_read_thread(void* arg) {
  struct st_app_rx_anc_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40_app_thread_stop) {
    mbuf = st40_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      st_pthread_mutex_lock(&s->st40_wake_mutex);
      if (!s->st40_app_thread_stop)
        st_pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
      st_pthread_mutex_unlock(&s->st40_wake_mutex);
      continue;
    }
    /* parse the packet */
    if (s->st40_fmd)
      app_rx_anc_handle_fmd(s, usrptr, len);
    else
      app_rx_anc_handle_rtp(s, usrptr);
    st40_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_anc_rtp_ready(void* priv) {
  struct st_app_rx_anc_session* s = priv;

  st_pthread_mutex_lock(&s->st40_wake_mutex);
  st_pthread_cond_signal(&s->st40_wake_cond);
  st_pthread_mutex_unlock(&s->st40_wake_mutex);
  return 0;
}

static void app_rx_anc_stop(struct st_app_rx_anc_session* s) {
  int idx = s->idx;

  s->st40_app_thread_stop = true;
  if (s->st40_app_thread) {
    st_pthread_mutex_lock(&s->st40_wake_mutex);
    st_pthread_cond_signal(&s->st40_wake_cond);
    st_pthread_mutex_unlock(&s->st40_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st40_app_thread, NULL);
    s->st40_app_thread = 0;
  }
}

static int app_rx_anc_fmd_open_ref(struct st_app_rx_anc_session* s, const char* url) {
  struct stat i;
  int fd = st_open(url, O_RDONLY);
  if (fd < 0) {
    err("%s(%d), open '%s' fail\n", __func__, s->idx, url);
    return -EIO;
  }
  if (fstat(fd, &i) < 0) {
    err("%s(%d), fstat '%s' fail\n", __func__, s->idx, url);
    close(fd);
    return -EIO;
  }
  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap '%s' fail\n", __func__, s->idx, url);
    return -EIO;
  }
  s->st40_fmd_ref = m;
  s->st40_fmd_ref_size = i.st_size;
  s->st40_fmd_ref_next = -1;
  return 0;
}

static int app_rx_anc_uinit(struct st_app_rx_anc_session* s) {
  int ret, idx = s->idx;
  app_rx_anc_stop(s);
  if (s->handle) {
    ret = st40_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  st_pthread_mutex_destroy(&s->st40_wake_mutex);
  st_pthread_cond_destroy(&s->st40_wake_cond);
  if (s->st40_fmd_ref) {
    munmap(s->st40_fmd_ref, s->st40_fmd_ref_size);
    s->st40_fmd_ref = NULL;
  }

  return 0;
}

static int app_rx_anc_init(struct st_app_context* ctx, st_json_ancillary_session_t* anc,
                           struct st_app_rx_anc_session* s) {
  int idx = s->idx, ret;
  struct st40_rx_ops ops;
  char name[32];
  st40_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_anc%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = anc ? anc->base.num_inf : ctx->para.num_ports;
  memcpy(
      ops.ip_addr[MTL_SESSION_PORT_P],
      anc ? st_json_ip(ctx, &anc->base, MTL_SESSION_PORT_P) : ctx->rx_ip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_P],
         anc ? anc->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           anc ? anc->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = anc ? anc->base.udp_port : (10200 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.ip_addr[MTL_SESSION_PORT_R],
           anc ? st_json_ip(ctx, &anc->base, MTL_SESSION_PORT_R)
               : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_R],
           anc ? anc->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             anc ? anc->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = anc ? anc->base.udp_port : (10200 + s->idx);
  }
  ops.type = ST40_TYPE_RTP_LEVEL;
  ops.rtp_ring_size = 1024;
  ops.payload_type = anc ? anc->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  ops.interlaced = anc ? anc->info.interlaced : false;
  ops.notify_rtp_ready = app_rx_anc_rtp_ready;
  if (anc && anc->enable_rtcp) ops.flags |= ST40_RX_FLAG_ENABLE_RTCP;
  st_pthread_mutex_init(&s->st40_wake_mutex, NULL);
  st_pthread_cond_init(&s->st40_wake_cond, NULL);
  s->st40_fmd = anc && anc->info.fast_metadata;
  if (s->st40_fmd) {
    ops.flags |= ST40_RX_FLAG_FAST_METADATA;
    s->st40_fmd_dit = anc->info.fmd_dit;
    s->st40_fmd_k_bit = anc->info.fmd_k_bit;
    if (!anc->info.anc_url[0])
      info(
          "%s(%d), no ancillary_url, the payload is not verified, only DIT, K-bit and "
          "rate\n",
          __func__, idx);
    else if (app_rx_anc_fmd_open_ref(s, anc->info.anc_url) < 0)
      return -EIO;
  }

  handle = st40_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40_rx_create fail\n", __func__, idx);
    return -EIO;
  }
  s->handle = handle;

  ret = pthread_create(&s->st40_app_thread, NULL, app_rx_anc_read_thread, s);
  if (ret < 0) {
    err("%s, st40_app_thread create fail %d\n", __func__, ret);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_anc_%d", idx);
  mtl_thread_setname(s->st40_app_thread, thread_name);

  return 0;
}

static bool app_rx_anc_fps_check(double framerate) {
  double expect;

  for (enum st_fps fps = 0; fps < ST_FPS_MAX; fps++) {
    expect = st_frame_rate(fps);
    if (ST_APP_EXPECT_NEAR(framerate, expect, expect * 0.05)) return true;
  }

  return false;
}

static int app_rx_anc_result(struct st_app_rx_anc_session* s) {
  int idx = s->idx;
  app_rx_anc_stop(s);
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
  double framerate = s->stat_frame_total_received / time_sec;

  if (!s->stat_frame_total_received) {
    /* A fully corrupt stream never reaches the result line below, so the reject
     * count has to be reported here or it is lost. */
    if (s->stat_pkt_invalid)
      err("%s(%d), no valid packet, %d rejected as invalid\n", __func__, idx,
          s->stat_pkt_invalid);
    return -EINVAL;
  }

  char gap[32] = "";
  if (s->st40_fmd) {
    if (s->st40_fmd_last_rx_time)
      s->st40_fmd_max_gap_ns =
          ST_MAX(s->st40_fmd_max_gap_ns, cur_time_ns - s->st40_fmd_last_rx_time);
    (void)snprintf(gap, sizeof(gap), ", max gap %.3f ms",
                   (double)s->st40_fmd_max_gap_ns / NS_PER_MS);
  }
  bool ok = app_rx_anc_fps_check(framerate) && !s->stat_pkt_invalid &&
            s->st40_fmd_max_gap_ns <= ST_APP_FMD_MAX_GAP_NS;
  notce("%s(%d), %s, fps %f, %d frame received, %d invalid pkt%s\n", __func__, idx,
        ok ? "OK" : "FAILED", framerate, s->stat_frame_total_received,
        s->stat_pkt_invalid, gap);
  return ok ? 0 : -EIO;
}

int st_app_rx_anc_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_anc_session* s;
  ctx->rx_anc_sessions = (struct st_app_rx_anc_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_anc_session) * ctx->rx_anc_session_cnt);
  if (!ctx->rx_anc_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_anc_session_cnt; i++) {
    s = &ctx->rx_anc_sessions[i];
    s->idx = i;

    ret = app_rx_anc_init(ctx, ctx->json_ctx ? &ctx->json_ctx->rx_anc_sessions[i] : NULL,
                          s);
    if (ret < 0) {
      err("%s(%d), app_rx_anc_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_anc_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_anc_session* s;
  if (!ctx->rx_anc_sessions) return 0;
  for (i = 0; i < ctx->rx_anc_session_cnt; i++) {
    s = &ctx->rx_anc_sessions[i];
    app_rx_anc_uinit(s);
  }
  st_app_free(ctx->rx_anc_sessions);
  return 0;
}

int st_app_rx_anc_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_anc_session* s;
  if (!ctx->rx_anc_sessions) return 0;

  for (i = 0; i < ctx->rx_anc_session_cnt; i++) {
    s = &ctx->rx_anc_sessions[i];
    ret += app_rx_anc_result(s);
  }

  return ret;
}
