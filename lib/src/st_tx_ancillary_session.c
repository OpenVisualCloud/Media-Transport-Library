/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_ancillary_session.h"

#include "st_dev.h"
#include "st_log.h"
#include "st_sch.h"
#include "st_util.h"

/* call tx_ancillary_session_put always if get successfully */
static inline struct st_tx_ancillary_session_impl* tx_ancillary_session_get(
    struct st_tx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_ancillary_session_put always if get successfully */
static inline struct st_tx_ancillary_session_impl* tx_ancillary_session_try_get(
    struct st_tx_ancillary_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_tx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline bool tx_ancillary_session_get_empty(
    struct st_tx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_ancillary_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void tx_ancillary_session_put(struct st_tx_ancillary_sessions_mgr* mgr,
                                            int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static int tx_ancillary_session_alloc_frames(struct st_main_impl* impl,
                                             struct st_tx_ancillary_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  struct st40_tx_ops* ops = &s->ops;
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t size = sizeof(struct st40_frame) * ops->framebuff_cnt;
  void* frame;

  if (s->st40_frames) {
    err("%s(%d), st40_frames already alloc\n", __func__, idx);
    return -EIO;
  }

  frame = st_rte_zmalloc_socket(size, soc_id);
  if (!frame) {
    err("%s(%d), rte_malloc %" PRIu64 " fail\n", __func__, idx, size);
    return -ENOMEM;
  }

  s->st40_frames = frame;

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_ancillary_session_free_frames(struct st_tx_ancillary_session_impl* s) {
  if (s->st40_frames) {
    st_rte_free(s->st40_frames);
    s->st40_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_ancillary_session_init_hdr(struct st_main_impl* impl,
                                         struct st_tx_ancillary_sessions_mgr* mgr,
                                         struct st_tx_ancillary_session_impl* s,
                                         enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st40_tx_ops* ops = &s->ops;
  int ret;
  struct st_rfc8331_anc_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st40_rfc8331_rtp_hdr* rtp = &hdr->rtp;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = st_sip_addr(impl, port);

  /* ether hdr */
  ret = st_dev_dst_ip_mac(impl, dip, st_eth_d_addr(eth), port);
  if (ret < 0) {
    err("%s(%d), st_dev_dst_ip_mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0],
        dip[1], dip[2], dip[3]);
    return ret;
  }

  ret = rte_eth_macaddr_get(mgr->port_id[port], st_eth_s_addr(eth));
  if (ret < 0) {
    err("%s(%d), rte_eth_macaddr_get fail %d for port %d\n", __func__, idx, ret, port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ipv4 hdr */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0;
  ipv4->fragment_offset = ST_IP_DONT_FRAGMENT_FLAG;
  ipv4->next_proto_id = 17;
  st_memcpy(&ipv4->src_addr, sip, ST_IP_ADDR_LEN);
  st_memcpy(&ipv4->dst_addr, dip, ST_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st40_src_port[s_port]);
  udp->dst_port = htons(s->st40_dst_port[s_port]);
  udp->dgram_cksum = 0;

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = ST_RVRTP_VERSION_2;
  rtp->base.marker = 0;
  rtp->base.payload_type = st_is_valid_payload_type(ops->payload_type)
                               ? ops->payload_type
                               : ST_RANCRTP_PAYLOAD_TYPE_ANCILLARY;
  rtp->base.ssrc = htonl(s->idx + 0x323450);
  s->st40_seq_id = 0;
  s->st40_ext_seq_id = 0;

  info("%s(%d), succ, dst ip:port %d.%d.%d.%d:%d, s_port %d\n", __func__, idx, dip[0],
       dip[1], dip[2], dip[3], s->st40_dst_port[s_port], s_port);
  return 0;
}

static int tx_ancillary_session_init_pacing(struct st_main_impl* impl,
                                            struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  double frame_time = (double)1000000000.0 * s->fps_tm.den / s->fps_tm.mul;

  pacing->frame_time = frame_time;
  pacing->frame_time_sampling =
      (double)(s->fps_tm.sampling_clock_rate) * s->fps_tm.den / s->fps_tm.mul;
  /* always use ST_PORT_P for ptp now */
  pacing->cur_epochs = st_get_ptp_time(impl, ST_PORT_P) / frame_time;
  pacing->tsc_time_cursor = 0;

  info("%s[%02d], frame_time %f frame_time_sampling %f\n", __func__, idx,
       pacing->frame_time, pacing->frame_time_sampling);
  return 0;
}

static inline double tx_ancillary_pacing_time(
    struct st_tx_ancillary_session_pacing* pacing, uint64_t epochs) {
  return epochs * pacing->frame_time;
}

static inline uint32_t tx_ancillary_pacing_time_stamp(
    struct st_tx_ancillary_session_pacing* pacing, uint64_t epochs) {
  uint64_t tmstamp64 = epochs * pacing->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;

  return tmstamp32;
}

static int tx_ancillary_session_sync_pacing(struct st_main_impl* impl,
                                            struct st_tx_ancillary_session_impl* s,
                                            bool sync) {
  int idx = s->idx;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  double frame_time = pacing->frame_time;
  /* always use ST_PORT_P for ptp now */
  uint64_t ptp_time = st_get_ptp_time(impl, ST_PORT_P);
  uint64_t epochs = ptp_time / frame_time;
  double to_epoch_tr_offset;

  dbg("%s(%d), epochs %" PRIu64 " %" PRIu64 "\n", __func__, idx, epochs,
      pacing->cur_epochs);
  if (epochs == pacing->cur_epochs) {
    /* likely most previous frame can enqueue within previous timing */
    epochs++;
  }

  to_epoch_tr_offset = tx_ancillary_pacing_time(pacing, epochs) - ptp_time;
  if (to_epoch_tr_offset < 0) {
    /* current time run out of tr offset already, sync to next epochs */
    s->st40_epoch_mismatch++;
    epochs++;
    to_epoch_tr_offset = tx_ancillary_pacing_time(pacing, epochs) - ptp_time;
  }

  if (to_epoch_tr_offset < 0) {
    /* should never happen */
    err("%s(%d), error to_epoch_tr_offset %f, ptp_time %" PRIu64 ", epochs %" PRIu64
        " %" PRIu64 "\n",
        __func__, idx, to_epoch_tr_offset, ptp_time, epochs, pacing->cur_epochs);
    to_epoch_tr_offset = 0;
  }

  pacing->cur_epochs = epochs;
  pacing->cur_time_stamp = tx_ancillary_pacing_time_stamp(pacing, epochs);
  pacing->tsc_time_cursor = (double)st_get_tsc(impl) + to_epoch_tr_offset;
  dbg("%s(%d), epochs %lu time_stamp %u time_cursor %f to_epoch_tr_offset %f\n", __func__,
      idx, pacing->cur_epochs, pacing->cur_time_stamp, pacing->tsc_time_cursor,
      to_epoch_tr_offset);

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, idx,
        pacing->tsc_time_cursor, st_get_tsc(impl));
    st_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tx_ancillary_session_init(struct st_main_impl* impl,
                                     struct st_tx_ancillary_sessions_mgr* mgr,
                                     struct st_tx_ancillary_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int tx_ancillary_sessions_tasklet_start(void* priv) { return 0; }

static int tx_ancillary_sessions_tasklet_stop(void* priv) { return 0; }

static int tx_ancillary_session_build_rtp_packet(struct st_tx_ancillary_session_impl* s,
                                                 struct rte_mbuf* pkt, int anc_idx) {
  struct st40_rfc8331_rtp_hdr* rtp;

  rtp = rte_pktmbuf_mtod(pkt, struct st40_rfc8331_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr[ST_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->base.seq_number = htons(s->st40_seq_id);
  rtp->seq_number_ext = htons(s->st40_ext_seq_id);
  if (s->st40_seq_id == 0xFFFF) s->st40_ext_seq_id++;
  s->st40_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.cur_time_stamp);

  /* Set place for payload just behind rtp header */
  uint8_t* payload = (uint8_t*)&rtp[1];
  struct st40_frame* src =
      (struct st40_frame*)(s->st40_frames +
                           s->st40_frame_idx * sizeof(struct st40_frame));
  int anc_count = src->meta_num;
  int total_udw = 0;
  int idx = 0;
  for (idx = anc_idx; idx < anc_count; idx++) {
    uint16_t udw_size = src->meta[idx].udw_size;
    total_udw += udw_size;
    if (total_udw * 10 / 8 > 1200) break;
    struct st40_rfc8331_payload_hdr* pktBuff =
        (struct st40_rfc8331_payload_hdr*)(payload);
    pktBuff->first_hdr_chunk.c = src->meta[idx].c;
    pktBuff->first_hdr_chunk.line_number = src->meta[idx].line_number;
    pktBuff->first_hdr_chunk.horizontal_offset = src->meta[idx].hori_offset;
    pktBuff->first_hdr_chunk.s = src->meta[idx].s;
    pktBuff->first_hdr_chunk.stream_num = src->meta[idx].stream_num;
    pktBuff->second_hdr_chunk.did = st40_add_parity_bits(src->meta[idx].did);
    pktBuff->second_hdr_chunk.sdid = st40_add_parity_bits(src->meta[idx].sdid);
    pktBuff->second_hdr_chunk.data_count = st40_add_parity_bits(udw_size);

    pktBuff->swaped_first_hdr_chunk = htonl(pktBuff->swaped_first_hdr_chunk);
    pktBuff->swaped_second_hdr_chunk = htonl(pktBuff->swaped_second_hdr_chunk);
    int i = 0;
    int offset = src->meta[idx].udw_offset;
    for (; i < udw_size; i++) {
      st40_set_udw(i + 3, st40_add_parity_bits(src->data[offset++]),
                   (uint8_t*)&pktBuff->second_hdr_chunk);
    }
    uint16_t checksum = 0;
    checksum = st40_calc_checksum(3 + udw_size, (uint8_t*)&pktBuff->second_hdr_chunk);
    st40_set_udw(i + 3, checksum, (uint8_t*)&pktBuff->second_hdr_chunk);

    uint16_t total_size =
        ((3 + udw_size + 1) * 10) / 8;  // Calculate size of the
                                        // 10-bit words: DID, SDID, DATA_COUNT
                                        // + size of buffer with data + checksum
    total_size = (4 - total_size % 4) + total_size;  // Calculate word align to the 32-bit
                                                     // word of ANC data packet
    uint16_t size_to_send =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size;  // Full size of one ANC
    payload = payload + size_to_send;
  }
  int payload_size = payload - (uint8_t*)&rtp[1];
  pkt->data_len = payload_size + sizeof(struct st40_rfc8331_rtp_hdr);
  pkt->pkt_len = pkt->data_len;
  rtp->length = htons(payload_size);
  rtp->anc_count = idx - anc_idx;
  rtp->f = 0b00;
  if (idx == anc_count) rtp->base.marker = 1;
  return idx;
}

static int tx_ancillary_session_build_packet(struct st_main_impl* impl,
                                             struct st_tx_ancillary_session_impl* s,
                                             struct rte_mbuf* pkt,
                                             struct rte_mbuf* pkt_rtp,
                                             enum st_session_port s_port) {
  struct st_base_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st40_tx_ops* ops = &s->ops;

  hdr = rte_pktmbuf_mtod(pkt, struct st_base_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[s_port].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[s_port].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[s_port].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st40_ipv4_packet_id);
  /* update only for primary */
  if (s_port == ST_SESSION_PORT_P) {
    s->st40_ipv4_packet_id++;
    /* update rtp time for rtp path */
    if (ops->type == ST40_TYPE_RTP_LEVEL) {
      struct st40_rfc8331_rtp_hdr* rtp =
          rte_pktmbuf_mtod(pkt_rtp, struct st40_rfc8331_rtp_hdr*);
      if (rtp->base.tmstamp != s->st40_rtp_time) {
        /* start of a new frame */
        s->st40_pkt_idx = 0;
        rte_atomic32_inc(&s->st40_stat_frame_cnt);
        s->st40_rtp_time = rtp->base.tmstamp;
        tx_ancillary_session_sync_pacing(impl, s, false);
      }
      rtp->base.tmstamp = htonl(s->pacing.cur_time_stamp);
    }
  }

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);
  pkt->pkt_len = pkt->data_len;
  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_rtp);
  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  /* rtp packet used twice for redundant path */
  if (s_port == ST_SESSION_PORT_R) rte_mbuf_refcnt_update(pkt_rtp, 1);

  return 0;
}

static int tx_ancillary_session_tasklet_frame(struct st_main_impl* impl,
                                              struct st_tx_ancillary_sessions_mgr* mgr,
                                              struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  struct st40_tx_ops* ops = &s->ops;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  enum st_port port_p = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  enum st_port port_r = ST_PORT_MAX;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;

  if (s->ops.num_port > 1) {
    send_r = true;
    port_r = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_R);
    hdr_pool_r = s->mbuf_mempool_hdr[ST_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[ST_SESSION_PORT_P]) {
    ret = rte_ring_mp_enqueue(mgr->ring[port_p], (void*)s->inflight[ST_SESSION_PORT_P]);
    if (ret == 0) s->has_inflight[ST_SESSION_PORT_P] = false;
    return 0;
  }

  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    ret = rte_ring_mp_enqueue(mgr->ring[port_r], (void*)s->inflight[ST_SESSION_PORT_R]);
    if (ret == 0) s->has_inflight[ST_SESSION_PORT_R] = false;
    return 0;
  }

  if (ST40_TX_STAT_WAIT_FRAME == s->st40_frame_stat) {
    uint16_t next_frame_idx;
    int total_udw = 0;

    /* Query next frame buffer idx */
    ret = ops->get_next_frame(ops->priv, &next_frame_idx);
    if (ret < 0) { /* no frame ready from app */
      dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
      return ret;
    }
    s->st40_frame_idx = next_frame_idx;
    dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
    s->st40_frame_stat = ST40_TX_STAT_SENDING_PKTS;
    struct st40_frame* src =
        (struct st40_frame*)(s->st40_frames +
                             s->st40_frame_idx * sizeof(struct st40_frame));
    for (int i = 0; i < src->meta_num; i++) total_udw += src->meta[i].udw_size;
    s->st40_pkt_idx = 0;
    s->st40_total_pkts =
        (total_udw * 10 / 8) / 1200 + 1; /* len > 1200, will split to another packet */
  }

  /* sync pacing */
  if (!pacing->tsc_time_cursor) tx_ancillary_session_sync_pacing(impl, s, false);

  uint64_t cur_tsc = st_get_tsc(impl);
  uint64_t target_tsc = pacing->tsc_time_cursor;
  if (cur_tsc < target_tsc) {
    uint64_t delta = target_tsc - cur_tsc;
    // dbg("%s(%d), cur_tsc %"PRIu64" target_tsc %"PRIu64"\n", __func__, idx, cur_tsc,
    // target_tsc);
    if (likely(delta < NS_PER_S)) {
      return 0;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;
  struct rte_mbuf* pkt_rtp = NULL;
  int anc_idx = 0;
  for (unsigned int i = 0; i < s->st40_total_pkts; i++) {
    pkt_rtp = rte_pktmbuf_alloc(chain_pool);
    if (!pkt_rtp) {
      err("%s(%d), pkt_rtp alloc fail\n", __func__, idx);
      return -ENOMEM;
    }
    pkt = rte_pktmbuf_alloc(hdr_pool_p);
    if (!pkt) {
      err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt_rtp);
      return -ENOMEM;
    }
    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        return -ENOMEM;
      }
    }
    anc_idx = tx_ancillary_session_build_rtp_packet(s, pkt_rtp, anc_idx);
    tx_ancillary_session_build_packet(impl, s, pkt, pkt_rtp, ST_SESSION_PORT_P);
    st_tx_mbuf_set_idx(pkt, s->st40_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);
    if (send_r) {
      tx_ancillary_session_build_packet(impl, s, pkt_r, pkt_rtp, ST_SESSION_PORT_R);
      st_tx_mbuf_set_idx(pkt_r, s->st40_pkt_idx);
      st_tx_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
    }

    s->st40_pkt_idx++;
    s->st40_stat_pkt_cnt++;
    if (rte_ring_mp_enqueue(mgr->ring[port_p], (void*)pkt) != 0) {
      s->inflight[ST_SESSION_PORT_P] = pkt;
      s->has_inflight[ST_SESSION_PORT_P] = true;
      s->inflight_cnt[ST_SESSION_PORT_P]++;
    }
    if (send_r && rte_ring_mp_enqueue(mgr->ring[port_r], (void*)pkt_r) != 0) {
      s->inflight[ST_SESSION_PORT_R] = pkt_r;
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
    }
  }
  pacing->tsc_time_cursor = 0;

  if (s->st40_pkt_idx >= s->st40_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st40_frame_idx);
    /* end of current frame */
    if (s->ops.notify_frame_done) ops->notify_frame_done(ops->priv, s->st40_frame_idx);
    s->st40_frame_stat = ST40_TX_STAT_WAIT_FRAME;
    s->st40_pkt_idx = 0;
    rte_atomic32_inc(&s->st40_stat_frame_cnt);
  }

  return 0;
}

static int tx_ancillary_session_tasklet_rtp(struct st_main_impl* impl,
                                            struct st_tx_ancillary_sessions_mgr* mgr,
                                            struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  bool send_r = false;
  enum st_port port_p = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  enum st_port port_r = ST_PORT_MAX;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;

  if (s->ops.num_port > 1) {
    send_r = true;
    port_r = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_R);
    hdr_pool_r = s->mbuf_mempool_hdr[ST_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[ST_SESSION_PORT_P]) {
    ret = rte_ring_mp_enqueue(mgr->ring[port_p], (void*)s->inflight[ST_SESSION_PORT_P]);
    if (ret == 0) s->has_inflight[ST_SESSION_PORT_P] = false;
    return 0;
  }

  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    ret = rte_ring_mp_enqueue(mgr->ring[port_r], (void*)s->inflight[ST_SESSION_PORT_R]);
    if (ret == 0) s->has_inflight[ST_SESSION_PORT_R] = false;
    return 0;
  }

  uint64_t cur_tsc = st_get_tsc(impl);
  uint64_t target_tsc = pacing->tsc_time_cursor;
  if (cur_tsc < target_tsc) {
    uint64_t delta = target_tsc - cur_tsc;
    // dbg("%s(%d), cur_tsc %"PRIu64" target_tsc %"PRIu64"\n", __func__, idx, cur_tsc,
    // target_tsc);
    if (likely(delta < NS_PER_S)) {
      return 0;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;
  struct rte_mbuf* pkt_rtp = NULL;

  if (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt_rtp) != 0) {
    dbg("%s(%d), rtp pkts not ready %d\n", __func__, idx, ret);
    return -EBUSY;
  }

  s->ops.notify_rtp_done(s->ops.priv);

  pkt = rte_pktmbuf_alloc(hdr_pool_p);
  if (!pkt) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
    rte_pktmbuf_free(pkt_rtp);
    return -ENOMEM;
  }
  if (send_r) {
    pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
    if (!pkt_r) {
      err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt);
      rte_pktmbuf_free(pkt_rtp);
      return -ENOMEM;
    }
  }

  tx_ancillary_session_build_packet(impl, s, pkt, pkt_rtp, ST_SESSION_PORT_P);
  st_tx_mbuf_set_idx(pkt, s->st40_pkt_idx);
  st_tx_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    tx_ancillary_session_build_packet(impl, s, pkt_r, pkt_rtp, ST_SESSION_PORT_R);
    st_tx_mbuf_set_idx(pkt_r, s->st40_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
  }
  s->st40_pkt_idx++;
  s->st40_stat_pkt_cnt++;

  if (rte_ring_mp_enqueue(mgr->ring[port_p], (void*)pkt) != 0) {
    s->inflight[ST_SESSION_PORT_P] = pkt;
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
  }
  if (send_r && rte_ring_mp_enqueue(mgr->ring[port_r], (void*)pkt_r) != 0) {
    s->inflight[ST_SESSION_PORT_R] = pkt_r;
    s->has_inflight[ST_SESSION_PORT_R] = true;
    s->inflight_cnt[ST_SESSION_PORT_R]++;
  }
  return 0;
}

static int tx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_tx_ancillary_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_ancillary_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_ancillary_session_try_get(mgr, sidx);
    if (!s) continue;

    if (s->ops.type == ST40_TYPE_FRAME_LEVEL)
      tx_ancillary_session_tasklet_frame(impl, mgr, s);
    else
      tx_ancillary_session_tasklet_rtp(impl, mgr, s);

    tx_ancillary_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_ancillary_sessions_mgr_uinit_hw(struct st_main_impl* impl,
                                              struct st_tx_ancillary_sessions_mgr* mgr) {
  for (int i = 0; i < st_num_ports(impl); i++) {
    if (mgr->ring[i]) {
      rte_ring_free(mgr->ring[i]);
      mgr->ring[i] = NULL;
    }
    if (mgr->queue_active[i]) {
      st_dev_free_tx_queue(impl, i, mgr->queue_id[i]);
      mgr->queue_active[i] = false;
    }
  }

  dbg("%s(%d), succ\n", __func__, mgr->idx);
  return 0;
}

static int tx_ancillary_sessions_mgr_init_hw(struct st_main_impl* impl,
                                             struct st_tx_ancillary_sessions_mgr* mgr) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx;
  int ret;
  uint16_t queue = 0;

  for (int i = 0; i < st_num_ports(impl); i++) {
    /* do we need quota for anc? */
    ret = st_dev_request_tx_queue(impl, i, &queue, 0);
    if (ret < 0) {
      tx_ancillary_sessions_mgr_uinit_hw(impl, mgr);
      return ret;
    }
    mgr->queue_id[i] = queue;
    mgr->queue_active[i] = true;
    mgr->port_id[i] = st_port_id(impl, i);

    snprintf(ring_name, 32, "TX-ANC-RING-M%d-P%d", mgr_idx, i);
    flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
    count = ST_TX_ANC_SESSIONS_RING_SIZE;
    ring = rte_ring_create(ring_name, count, st_socket_id(impl, i), flags);
    if (!ring) {
      err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, i);
      tx_ancillary_sessions_mgr_uinit_hw(impl, mgr);
      return -ENOMEM;
    }
    mgr->ring[i] = ring;
    info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, i, queue);
  }

  return 0;
}

static int tx_ancillary_session_flush_port(struct st_tx_ancillary_sessions_mgr* mgr,
                                           enum st_port port) {
  struct st_main_impl* impl = mgr->parnet;
  struct st_interface* inf = st_if(impl, port);
  int ret;
  int burst_pkts = inf->nb_tx_desc;
  struct rte_mbuf* pad = inf->pad;

  for (int i = 0; i < burst_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    do {
      ret = rte_ring_mp_enqueue(mgr->ring[port], (void*)pad);
    } while (ret != 0);
  }

  return 0;
}

/* wa to flush the ancillary transmitter tx queue */
static int tx_ancillary_session_flush(struct st_tx_ancillary_sessions_mgr* mgr,
                                      struct st_tx_ancillary_session_impl* s) {
  int mgr_idx = mgr->idx, s_idx = s->idx;

  for (int i = 0; i < ST_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];
    if (pool && rte_mempool_in_use_count(pool)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_ancillary_session_flush_port(mgr, st_port_logic2phy(s->port_maps, i));
      info("%s(%d,%d), flush port %d end\n", __func__, mgr_idx, s_idx, i);

      int retry = 100; /* max 1000ms */
      while (retry > 0) {
        retry--;
        if (!rte_mempool_in_use_count(pool)) break;
        st_sleep_ms(10);
      }
      info("%s(%d,%d), check in_use retry %d\n", __func__, mgr_idx, s_idx, retry);
    }
  }

  return 0;
}

int tx_ancillary_session_mempool_free(struct st_tx_ancillary_session_impl* s) {
  int ret;

  if (s->mbuf_mempool_chain) {
    ret = st_mempool_free(s->mbuf_mempool_chain);
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }

  for (int i = 0; i < ST_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i]) {
      ret = st_mempool_free(s->mbuf_mempool_hdr[i]);
      if (ret >= 0) s->mbuf_mempool_hdr[i] = NULL;
    }
  }

  return 0;
}

static int tx_ancillary_session_mempool_init(struct st_main_impl* impl,
                                             struct st_tx_ancillary_sessions_mgr* mgr,
                                             struct st_tx_ancillary_session_impl* s) {
  struct st40_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum st_port port;
  unsigned int n;

  uint16_t hdr_room_size = sizeof(struct st_base_hdr);

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
    n = st_if_nb_tx_desc(impl, port) + ST_TX_ANC_SESSIONS_RING_SIZE;
    if (s->mbuf_mempool_hdr[i]) {
      warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "TXANCHDR-M%d-R%d-P%d", mgr->idx, idx, i);
      struct rte_mempool* mbuf_pool =
          st_mempool_create(impl, port, pool_name, n, ST_MBUF_CACHE_SIZE,
                            sizeof(struct st_muf_priv_data), hdr_room_size);
      if (!mbuf_pool) {
        tx_ancillary_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  n = st_if_nb_tx_desc(impl, port) + ST_TX_ANC_SESSIONS_RING_SIZE;
  if (ops->type == ST40_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
  if (s->mbuf_mempool_chain) {
    warn("%s(%d), use previous chain mempool\n", __func__, idx);
  } else {
    char pool_name[32];
    snprintf(pool_name, 32, "TXANCCHAIN-M%d-R%d", mgr->idx, idx);
    struct rte_mempool* mbuf_pool = st_mempool_create(
        impl, port, pool_name, n, ST_MBUF_CACHE_SIZE, sizeof(struct st_muf_priv_data),
        ST_PKT_MAX_ETHER_BYTES - hdr_room_size);
    if (!mbuf_pool) {
      tx_ancillary_session_mempool_free(s);
      return -ENOMEM;
    }
    s->mbuf_mempool_chain = mbuf_pool;
  }

  return 0;
}

static int tx_ancillary_session_init_rtp(struct st_main_impl* impl,
                                         struct st_tx_ancillary_sessions_mgr* mgr,
                                         struct st_tx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "TX-ANC-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    tx_ancillary_session_mempool_free(s);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
  return 0;
}

static int tx_ancillary_session_uinit_sw(struct st_tx_ancillary_sessions_mgr* mgr,
                                         struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;

  for (int port = 0; port < num_port; port++) {
    if (s->has_inflight[port]) {
      info("%s(%d), free inflight buf for port %d\n", __func__, idx, port);
      rte_pktmbuf_free(s->inflight[port]);
      s->has_inflight[port] = false;
    }
  }

  if (s->packet_ring) {
    st_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tx_ancillary_session_flush(mgr, s);
  tx_ancillary_session_mempool_free(s);

  tx_ancillary_session_free_frames(s);

  return 0;
}

static int tx_ancillary_session_init_sw(struct st_main_impl* impl,
                                        struct st_tx_ancillary_sessions_mgr* mgr,
                                        struct st_tx_ancillary_session_impl* s) {
  struct st40_tx_ops* ops = &s->ops;
  int idx = s->idx, ret;

  /* free the pool if any in previous session */
  tx_ancillary_session_mempool_free(s);
  ret = tx_ancillary_session_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_ancillary_session_uinit_sw(mgr, s);
    return ret;
  }

  if (ops->type == ST40_TYPE_RTP_LEVEL) {
    ret = tx_ancillary_session_init_rtp(impl, mgr, s);
  } else {
    ret = tx_ancillary_session_alloc_frames(impl, s);
  }
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_ancillary_session_uinit_sw(mgr, s);
    return ret;
  }

  return 0;
}

static int tx_ancillary_session_attach(struct st_main_impl* impl,
                                       struct st_tx_ancillary_sessions_mgr* mgr,
                                       struct st_tx_ancillary_session_impl* s,
                                       struct st40_tx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st40_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10200 + idx);
    s->st40_dst_port[i] = s->st40_src_port[i];
  }
  s->st40_ipv4_packet_id = 0;

  s->st40_frame_stat = ST40_TX_STAT_WAIT_FRAME;
  s->st40_frame_idx = 0;
  rte_atomic32_set(&s->st40_stat_frame_cnt, 0);

  for (int i = 0; i < num_port; i++) {
    s->has_inflight[i] = false;
    s->inflight_cnt[i] = 0;
  }

  ret = st_get_fps_timing(ops->fps, &s->fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  ret = tx_ancillary_session_init_pacing(impl, s);
  if (ret < 0) {
    err("%s(%d), init pacing fail %d\n", __func__, idx, ret);
    return ret;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tx_ancillary_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), port(%d) init hdr fail %d\n", __func__, idx, i, ret);
      return ret;
    }
  }

  ret = tx_ancillary_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), init sw fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static void tx_ancillary_session_stat(struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  int frame_cnt = rte_atomic32_read(&s->st40_stat_frame_cnt);

  rte_atomic32_set(&s->st40_stat_frame_cnt, 0);

  info("TX_ANC_SESSION(%d:%s): frame cnt %d, pkt cnt %d\n", idx, s->ops_name, frame_cnt,
       s->st40_stat_pkt_cnt);
  s->st40_stat_pkt_cnt = 0;

  if (s->st40_epoch_mismatch) {
    info("TX_ANC_SESSION(%d): st40 epoch mismatch %d\n", idx, s->st40_epoch_mismatch);
    s->st40_epoch_mismatch = 0;
  }
}

int tx_ancillary_session_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                struct st_tx_ancillary_session_impl* s) {
  tx_ancillary_session_stat(s);
  tx_ancillary_session_uinit_sw(mgr, s);
  return 0;
}

static int tx_ancillary_sessions_mgr_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                            struct st_tx_ancillary_session_impl* s,
                                            int idx) {
  tx_ancillary_session_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  st_rte_free(s);
  return 0;
}

int st_tx_ancillary_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                      struct st_tx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct st_sch_tasklet_ops ops;
  int ret, i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc8331_anc_hdr) != 62);

  mgr->parnet = impl;
  mgr->idx = idx;

  for (i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  ret = tx_ancillary_sessions_mgr_init_hw(impl, mgr);
  if (ret < 0) {
    err("%s(%d), tx_ancillary_sessions_mgr_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_ancillary_sessions_mgr";
  ops.start = tx_ancillary_sessions_tasklet_start;
  ops.stop = tx_ancillary_sessions_tasklet_stop;
  ops.handler = tx_ancillary_sessions_tasklet_handler;

  mgr->tasklet = st_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    tx_ancillary_sessions_mgr_uinit_hw(impl, mgr);
    err("%s(%d), st_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_tx_ancillary_sessions_mgr_uinit(struct st_tx_ancillary_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_ancillary_session_impl* s;

  if (mgr->tasklet) {
    st_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    s = tx_ancillary_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_ancillary_sessions_mgr_detach(mgr, s, i);
    tx_ancillary_session_put(mgr, i);
  }

  tx_ancillary_sessions_mgr_uinit_hw(impl, mgr);

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

struct st_tx_ancillary_session_impl* st_tx_ancillary_sessions_mgr_attach(
    struct st_tx_ancillary_sessions_mgr* mgr, struct st40_tx_ops* ops) {
  int midx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  int ret;
  struct st_tx_ancillary_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    if (!tx_ancillary_session_get_empty(mgr, i)) continue;

    s = st_rte_zmalloc_socket(sizeof(*s), st_socket_id(impl, ST_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      return NULL;
    }
    ret = tx_ancillary_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    ret = tx_ancillary_session_attach(mgr->parnet, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    tx_ancillary_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_tx_ancillary_sessions_mgr_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                        struct st_tx_ancillary_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_ancillary_sessions_mgr_detach(mgr, s, idx);

  tx_ancillary_session_put(mgr, idx);

  return 0;
}

int st_tx_ancillary_sessions_mgr_update(struct st_tx_ancillary_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_tx_ancillary_sessions_stat(struct st_main_impl* impl) {
  struct st_tx_ancillary_sessions_mgr* mgr = &impl->tx_anc_mgr;
  struct st_tx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_ancillary_session_get(mgr, j);
    if (!s) continue;
    tx_ancillary_session_stat(s);
    tx_ancillary_session_put(mgr, j);
  }
  if (mgr->st40_stat_pkts_burst) {
    info("TX_ANC_SESSION, pkts burst %d\n", mgr->st40_stat_pkts_burst);
    mgr->st40_stat_pkts_burst = 0;
  }
}
