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

#include "rx_ancillary_app.h"

static void app_rx_anc_handle_rtp(struct st_app_rx_anc_session* s, void* usrptr) {
  struct st40_rfc8331_rtp_hdr* hdr = (struct st40_rfc8331_rtp_hdr*)usrptr;
  struct st40_rfc8331_payload_hdr* payload_hdr =
      (struct st40_rfc8331_payload_hdr*)(&hdr[1]);

  int anc_count = hdr->anc_count;
  int idx, total_size, payload_len;
  for (idx = 0; idx < anc_count; idx++) {
    payload_hdr->swaped_first_hdr_chunk = ntohl(payload_hdr->swaped_first_hdr_chunk);
    payload_hdr->swaped_second_hdr_chunk = ntohl(payload_hdr->swaped_second_hdr_chunk);
    if (!st40_check_parity_bits(payload_hdr->second_hdr_chunk.did) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.sdid) ||
        !st40_check_parity_bits(payload_hdr->second_hdr_chunk.data_count)) {
      err("anc RTP checkParityBits error\n");
      return;
    }
    int udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;

    // verify checksum
    uint16_t checksum = 0;
    checksum = st40_get_udw(udw_size + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    payload_hdr->swaped_second_hdr_chunk = htonl(payload_hdr->swaped_second_hdr_chunk);
    if (checksum !=
        st40_calc_checksum(3 + udw_size, (uint8_t*)&payload_hdr->second_hdr_chunk)) {
      err("anc frame checksum error\n");
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
    total_size = ((3 + udw_size + 1) * 10) / 8;  // Calculate size of the
                                                 // 10-bit words: DID, SDID, DATA_COUNT
                                                 // + size of buffer with data + checksum
    total_size = (4 - total_size % 4) + total_size;  // Calculate word align to the 32-bit
                                                     // word of ANC data packet
    payload_len =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size;  // Full size of one ANC
    payload_hdr = (struct st40_rfc8331_payload_hdr*)((uint8_t*)payload_hdr + payload_len);
  }
}

static void* app_rx_anc_session_read_thread(void* arg) {
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
      pthread_mutex_lock(&s->st40_wake_mutex);
      if (!s->st40_app_thread_stop)
        pthread_cond_wait(&s->st40_wake_cond, &s->st40_wake_mutex);
      pthread_mutex_unlock(&s->st40_wake_mutex);
      continue;
    }
    /* parse the packet */
    app_rx_anc_handle_rtp(s, usrptr);
    st40_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_anc_session_rtp_ready(void* priv) {
  struct st_app_rx_anc_session* s = priv;

  pthread_mutex_lock(&s->st40_wake_mutex);
  pthread_cond_signal(&s->st40_wake_cond);
  pthread_mutex_unlock(&s->st40_wake_mutex);
  return 0;
}

static int app_rx_anc_session_uinit(struct st_app_rx_anc_session* s) {
  int ret, idx = s->idx;
  s->st40_app_thread_stop = true;
  if (s->st40_app_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&s->st40_wake_mutex);
    pthread_cond_signal(&s->st40_wake_cond);
    pthread_mutex_unlock(&s->st40_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st40_app_thread, NULL);
  }
  if (s->handle) {
    ret = st40_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  pthread_mutex_destroy(&s->st40_wake_mutex);
  pthread_cond_destroy(&s->st40_wake_cond);

  return 0;
}

static int app_rx_anc_session_init(struct st_app_context* ctx,
                                   st_json_rx_ancillary_session_t* anc,
                                   struct st_app_rx_anc_session* s) {
  int idx = s->idx, ret;
  struct st40_rx_ops ops;
  char name[32];
  st40_rx_handle handle;

  snprintf(name, 32, "app_rx_anc%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = anc ? anc->num_inf : ctx->para.num_ports;
  memcpy(ops.sip_addr[ST_PORT_P], anc ? anc->ip[ST_PORT_P] : ctx->rx_sip_addr[ST_PORT_P],
         ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          anc ? anc->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = anc ? anc->udp_port : (10200 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.sip_addr[ST_PORT_R],
           anc ? anc->ip[ST_PORT_R] : ctx->rx_sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            anc ? anc->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = anc ? anc->udp_port : (10200 + s->idx);
  }
  ops.rtp_ring_size = 1024;
  ops.notify_rtp_ready = app_rx_anc_session_rtp_ready;
  pthread_mutex_init(&s->st40_wake_mutex, NULL);
  pthread_cond_init(&s->st40_wake_cond, NULL);

  handle = st40_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40_rx_create fail\n", __func__, idx);
    return -EIO;
  }
  s->handle = handle;

  ret = pthread_create(&s->st40_app_thread, NULL, app_rx_anc_session_read_thread, s);
  if (ret < 0) {
    err("%s, st40_app_thread create fail %d\n", __func__, ret);
    return -EIO;
  }

  return 0;
}

int st_app_rx_anc_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_anc_session* s;

  for (i = 0; i < ctx->rx_anc_session_cnt; i++) {
    s = &ctx->rx_anc_sessions[i];
    s->idx = i;

    ret =
        app_rx_anc_session_init(ctx, ctx->json_ctx ? &ctx->json_ctx->rx_anc[i] : NULL, s);
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

  for (i = 0; i < ctx->rx_anc_session_cnt; i++) {
    s = &ctx->rx_anc_sessions[i];
    app_rx_anc_session_uinit(s);
  }
  return 0;
}