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

#include "st_tx_audio_session.h"

#include "st_dev.h"
#include "st_log.h"
#include "st_sch.h"
#include "st_util.h"

/* call tx_audio_session_put always if get successfully */
static inline struct st_tx_audio_session_impl* tx_audio_session_get(
    struct st_tx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_audio_session_put always if get successfully */
static inline struct st_tx_audio_session_impl* tx_audio_session_try_get(
    struct st_tx_audio_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_tx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_audio_session_put always if get successfully */
static inline bool tx_audio_session_get_empty(struct st_tx_audio_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_audio_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void tx_audio_session_put(struct st_tx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static int tx_audio_session_alloc_frames(struct st_main_impl* impl,
                                         struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t size = ops->framebuff_size * ops->framebuff_cnt;
  void* frame;

  if (s->st30_frames) {
    err("%s(%d), st30_frames already alloc\n", __func__, idx);
    return -EIO;
  }

  frame = st_rte_zmalloc_socket(size, soc_id);
  if (!frame) {
    err("%s(%d), rte_malloc %" PRIu64 " fail\n", __func__, idx, size);
    return -ENOMEM;
  }

  s->st30_frames = frame;

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_audio_session_free_frames(struct st_tx_audio_session_impl* s) {
  if (s->st30_frames) {
    st_rte_free(s->st30_frames);
    s->st30_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_audio_session_init_hdr(struct st_main_impl* impl,
                                     struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s,
                                     enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct st30_tx_ops* ops = &s->ops;
  int ret;
  struct st_rfc3550_audio_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st_rfc3550_rtp_hdr* rtp = &hdr->rtp;
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
  ipv4->total_length = htons(s->pkt_len + ST_PKT_AUDIO_HDR_LEN);
  ipv4->next_proto_id = 17;
  st_memcpy(&ipv4->src_addr, sip, ST_IP_ADDR_LEN);
  st_memcpy(&ipv4->dst_addr, dip, ST_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st30_src_port[s_port]);
  udp->dst_port = htons(s->st30_dst_port[s_port]);
  udp->dgram_len = htons(s->pkt_len + ST_PKT_AUDIO_HDR_LEN - sizeof(*ipv4));
  udp->dgram_cksum = 0;

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = ST_RVRTP_VERSION_2;
  rtp->marker = 0;
  rtp->payload_type = st_is_valid_payload_type(ops->payload_type)
                          ? ops->payload_type
                          : ST_RARTP_PAYLOAD_TYPE_PCM_AUDIO;
  rtp->ssrc = htonl(s->idx + 0x223450);

  info("%s(%d), succ, dst ip:port %d.%d.%d.%d:%d, port %d\n", __func__, idx, dip[0],
       dip[1], dip[2], dip[3], s->st30_dst_port[s_port], s_port);
  return 0;
}

static int tx_audio_session_init_pacing(struct st_main_impl* impl,
                                        struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  struct st30_tx_ops* ops = &s->ops;
  double frame_time = st30_get_packet_time(ops->ptime);

  pacing->frame_time = frame_time;
  pacing->frame_time_sampling = (double)(ops->sample_num * 1000) * 1 / 1000;
  pacing->trs = frame_time;

  /* always use ST_PORT_P for ptp now */
  pacing->cur_epochs = st_get_ptp_time(impl, ST_PORT_P) / frame_time;
  pacing->tsc_time_cursor = 0;

  info("%s[%02d], frame_time %f frame_time_sampling %f\n", __func__, idx,
       pacing->frame_time, pacing->frame_time_sampling);
  return 0;
}

static inline double tx_audio_pacing_time(struct st_tx_audio_session_pacing* pacing,
                                          uint64_t epochs) {
  return epochs * pacing->frame_time;
}

static inline uint32_t tx_audio_pacing_time_stamp(
    struct st_tx_audio_session_pacing* pacing, uint64_t epochs) {
  uint64_t tmstamp64 = epochs * pacing->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;

  return tmstamp32;
}

static int tx_audio_session_sync_pacing(struct st_main_impl* impl,
                                        struct st_tx_audio_session_impl* s, bool sync) {
  int idx = s->idx;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
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

  to_epoch_tr_offset = tx_audio_pacing_time(pacing, epochs) - ptp_time;
  if (to_epoch_tr_offset < 0) {
    /* current time run out of tr offset already, sync to next epochs */
    s->st30_epoch_mismatch++;
    epochs++;
    to_epoch_tr_offset = tx_audio_pacing_time(pacing, epochs) - ptp_time;
  }

  if (to_epoch_tr_offset < 0) {
    /* should never happen */
    err("%s(%d), error to_epoch_tr_offset %f, ptp_time %" PRIu64 ", epochs %" PRIu64
        " %" PRIu64 "\n",
        __func__, idx, to_epoch_tr_offset, ptp_time, epochs, pacing->cur_epochs);
    to_epoch_tr_offset = 0;
  }

  pacing->cur_epochs = epochs;
  pacing->cur_time_stamp = tx_audio_pacing_time_stamp(pacing, epochs);
  pacing->tsc_time_cursor = (double)st_get_tsc(impl) + to_epoch_tr_offset;

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, idx,
        pacing->tsc_time_cursor, st_get_tsc(impl));
    st_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tx_audio_session_init(struct st_main_impl* impl,
                                 struct st_tx_audio_sessions_mgr* mgr,
                                 struct st_tx_audio_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int tx_audio_sessions_tasklet_start(void* priv) { return 0; }

static int tx_audio_sessions_tasklet_stop(void* priv) { return 0; }

static int tx_audio_session_build_rtp_packet(struct st_main_impl* impl,
                                             struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt, int pkt_idx) {
  struct st_rfc3550_rtp_hdr* rtp;
  struct st30_tx_ops* ops = &s->ops;
  uint16_t len = s->pkt_len + sizeof(struct st_rfc3550_rtp_hdr);

  rtp = rte_pktmbuf_mtod(pkt, struct st_rfc3550_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr[ST_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->seq_number = htons(s->st30_seq_id);
  s->st30_seq_id++;
  rtp->tmstamp = htonl(s->pacing.cur_time_stamp);

  /* copy payload now */
  uint8_t* payload = (uint8_t*)&rtp[1];
  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  uint8_t* src = s->st30_frames + s->st30_frame_idx * ops->framebuff_size;
  rte_memcpy(payload, src + offset, s->pkt_len);

  pkt->data_len = len;
  pkt->pkt_len = len;

  return 0;
}

static int tx_audio_session_build_packet(struct st_main_impl* impl,
                                         struct st_tx_audio_session_impl* s,
                                         struct rte_mbuf* pkt, struct rte_mbuf* pkt_rtp,
                                         enum st_session_port s_port) {
  struct st_base_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st30_tx_ops* ops = &s->ops;

  hdr = rte_pktmbuf_mtod(pkt, struct st_base_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[s_port].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[s_port].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[s_port].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st30_ipv4_packet_id);
  /* update only for primary */
  if (s_port == ST_SESSION_PORT_P) {
    s->st30_ipv4_packet_id++;
    /* update rtp time for rtp path */
    if (ops->type == ST30_TYPE_RTP_LEVEL) {
      struct st_rfc3550_rtp_hdr* rtp =
          rte_pktmbuf_mtod(pkt_rtp, struct st_rfc3550_rtp_hdr*);
      if (rtp->tmstamp != s->st30_rtp_time_app) {
        /* start of a new epoch */
        s->st30_rtp_time_app = rtp->tmstamp;
        s->st30_rtp_time = s->pacing.cur_time_stamp;
      }
      /* update rtp time */
      rtp->tmstamp = htonl(s->st30_rtp_time);
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

static int tx_audio_session_tasklet_frame(struct st_main_impl* impl,
                                          struct st_tx_audio_sessions_mgr* mgr,
                                          struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st30_tx_ops* ops = &s->ops;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
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

  if (0 == s->st30_pkt_idx) {
    if (ST30_TX_STAT_WAIT_FRAME == s->st30_frame_stat) {
      uint16_t next_frame_idx;

      /* Query next frame buffer idx */
      ret = ops->get_next_frame(ops->priv, &next_frame_idx);
      if (ret < 0) { /* no frame ready from app */
        dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        return ret;
      }
      s->st30_frame_idx = next_frame_idx;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st30_frame_stat = ST30_TX_STAT_SENDING_PKTS;
    }
  }

  /* sync pacing */
  if (!pacing->tsc_time_cursor) tx_audio_session_sync_pacing(impl, s, false);

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
  struct rte_mbuf* pkt_rtp = NULL;
  struct rte_mbuf* pkt_r = NULL;

  pkt_rtp = rte_pktmbuf_alloc(chain_pool);
  if (!pkt_rtp) {
    err("%s(%d), pkt_rtp alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  pkt = rte_pktmbuf_alloc(hdr_pool_p);
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, idx);
    rte_pktmbuf_free(pkt_rtp);
    return -ENOMEM;
  }

  if (send_r) {
    pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
    if (!pkt_r) {
      err("%s(%d), rte_pktmbuf_alloc redundant fail\n", __func__, idx);
      rte_pktmbuf_free(pkt_rtp);
      rte_pktmbuf_free(pkt);
      return -ENOMEM;
    }
  }

  tx_audio_session_build_rtp_packet(impl, s, pkt_rtp, s->st30_pkt_idx);
  tx_audio_session_build_packet(impl, s, pkt, pkt_rtp, ST_SESSION_PORT_P);
  st_tx_mbuf_set_idx(pkt, s->st30_pkt_idx);
  st_tx_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    tx_audio_session_build_packet(impl, s, pkt_r, pkt_rtp, ST_SESSION_PORT_R);
    st_tx_mbuf_set_idx(pkt_r, s->st30_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
  }

  pacing->tsc_time_cursor += pacing->trs; /* pkt foward */
  s->st30_pkt_idx++;
  s->st30_stat_pkt_cnt++;

  pacing->tsc_time_cursor = 0;

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

  if (s->st30_pkt_idx >= s->st30_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st30_frame_idx);
    /* end of current frame */
    if (s->ops.notify_frame_done) ops->notify_frame_done(ops->priv, s->st30_frame_idx);
    s->st30_frame_stat = ST30_TX_STAT_WAIT_FRAME;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->st30_stat_frame_cnt);
  }

  return 0;
}

static int tx_audio_session_tasklet_rtp(struct st_main_impl* impl,
                                        struct st_tx_audio_sessions_mgr* mgr,
                                        struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
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

  /* sync pacing */
  if (!pacing->tsc_time_cursor) tx_audio_session_sync_pacing(impl, s, false);

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
    dbg("%s(%d), rtp pkts not ready\n", __func__, idx);
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

  tx_audio_session_build_packet(impl, s, pkt, pkt_rtp, ST_SESSION_PORT_P);
  st_tx_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    tx_audio_session_build_packet(impl, s, pkt_r, pkt_rtp, ST_SESSION_PORT_R);
    st_tx_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
  }
  s->st30_stat_pkt_cnt++;
  pacing->tsc_time_cursor = 0;

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

static int tx_audio_sessions_tasklet_handler(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_audio_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_audio_session_try_get(mgr, sidx);
    if (!s) continue;

    if (s->ops.type == ST30_TYPE_FRAME_LEVEL)
      tx_audio_session_tasklet_frame(impl, mgr, s);
    else
      tx_audio_session_tasklet_rtp(impl, mgr, s);

    tx_audio_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_audio_sessions_mgr_uinit_hw(struct st_main_impl* impl,
                                          struct st_tx_audio_sessions_mgr* mgr) {
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

static int tx_audio_sessions_mgr_init_hw(struct st_main_impl* impl,
                                         struct st_tx_audio_sessions_mgr* mgr) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx;
  int ret;
  uint16_t queue = 0;

  for (int i = 0; i < st_num_ports(impl); i++) {
    /* do we need quota for audio? */
    ret = st_dev_request_tx_queue(impl, i, &queue, 0);
    if (ret < 0) {
      tx_audio_sessions_mgr_uinit_hw(impl, mgr);
      return ret;
    }
    mgr->queue_id[i] = queue;
    mgr->queue_active[i] = true;
    mgr->port_id[i] = st_port_id(impl, i);

    snprintf(ring_name, 32, "TX-AUDIO-RING-M%d-P%d", mgr_idx, i);
    flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
    count = ST_TX_AUDIO_SESSIONS_RING_SIZE;
    ring = rte_ring_create(ring_name, count, st_socket_id(impl, i), flags);
    if (!ring) {
      err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, i);
      tx_audio_sessions_mgr_uinit_hw(impl, mgr);
      return -ENOMEM;
    }
    mgr->ring[i] = ring;
    info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, i, queue);
  }

  return 0;
}

static int tx_audio_session_flush_port(struct st_tx_audio_sessions_mgr* mgr,
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

/* wa to flush the audio transmitter tx queue */
static int tx_audio_session_flush(struct st_tx_audio_sessions_mgr* mgr,
                                  struct st_tx_audio_session_impl* s) {
  int mgr_idx = mgr->idx, s_idx = s->idx;

  for (int i = 0; i < ST_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];
    if (pool && rte_mempool_in_use_count(pool)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_audio_session_flush_port(mgr, st_port_logic2phy(s->port_maps, i));
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

int tx_audio_session_mempool_free(struct st_tx_audio_session_impl* s) {
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

static int tx_audio_session_mempool_init(struct st_main_impl* impl,
                                         struct st_tx_audio_sessions_mgr* mgr,
                                         struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum st_port port;
  unsigned int n;

  uint16_t hdr_room_size = sizeof(struct st_base_hdr);
  uint16_t chain_room_size = s->pkt_len + sizeof(struct st_rfc3550_rtp_hdr);

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
    n = st_if_nb_tx_desc(impl, port) + ST_TX_AUDIO_SESSIONS_RING_SIZE;
    if (s->mbuf_mempool_hdr[i]) {
      warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "TXAUDIOHDR-M%d-R%d-P%d", mgr->idx, idx, i);
      struct rte_mempool* mbuf_pool =
          st_mempool_create(impl, port, pool_name, n, ST_MBUF_CACHE_SIZE,
                            sizeof(struct st_muf_priv_data), hdr_room_size);
      if (!mbuf_pool) {
        tx_audio_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  n = st_if_nb_tx_desc(impl, port) + ST_TX_AUDIO_SESSIONS_RING_SIZE;
  if (ops->type == ST30_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
  if (s->mbuf_mempool_chain) {
    warn("%s(%d), use previous chain mempool\n", __func__, idx);
  } else {
    char pool_name[32];
    snprintf(pool_name, 32, "TXAUDIOCHAIN-M%d-R%d", mgr->idx, idx);
    struct rte_mempool* mbuf_pool = st_mempool_create(
        impl, port, pool_name, n, ST_MBUF_CACHE_SIZE, 0, chain_room_size);
    if (!mbuf_pool) {
      tx_audio_session_mempool_free(s);
      return -ENOMEM;
    }
    s->mbuf_mempool_chain = mbuf_pool;
  }

  return 0;
}

static int tx_audio_session_init_rtp(struct st_main_impl* impl,
                                     struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "TX-AUDIO-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    tx_audio_session_mempool_free(s);
    return -ENOMEM;
  }
  s->packet_ring = ring;

  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
  return 0;
}

static int tx_audio_session_uinit_sw(struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
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

  tx_audio_session_flush(mgr, s);
  tx_audio_session_mempool_free(s);

  tx_audio_session_free_frames(s);

  return 0;
}

static int tx_audio_session_init_sw(struct st_main_impl* impl,
                                    struct st_tx_audio_sessions_mgr* mgr,
                                    struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  int idx = s->idx, ret;

  /* free the pool if any in previous session */
  tx_audio_session_mempool_free(s);
  ret = tx_audio_session_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  if (ops->type == ST30_TYPE_RTP_LEVEL) {
    ret = tx_audio_session_init_rtp(impl, mgr, s);
  } else {
    ret = tx_audio_session_alloc_frames(impl, s);
  }
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int tx_audio_session_attach(struct st_main_impl* impl,
                                   struct st_tx_audio_sessions_mgr* mgr,
                                   struct st_tx_audio_session_impl* s,
                                   struct st30_tx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10100 + idx);
    s->st30_dst_port[i] = s->st30_src_port[i];
  }
  s->st30_ipv4_packet_id = 0;

  /* calculate pkts in line*/
  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr);
  s->pkt_len = ops->sample_size * ops->sample_num * ops->channel;
  s->st30_pkt_size = s->pkt_len + sizeof(struct st_rfc3550_audio_hdr);
  if (s->pkt_len > bytes_in_pkt) {
    err("%s(%d), invalid pkt_len %d\n", __func__, idx, s->pkt_len);
    return -EIO;
  }

  s->st30_total_pkts = ops->framebuff_size / s->pkt_len;
  if (ops->framebuff_size % s->pkt_len) {
    /* todo: add the support? */
    err("%s(%d), framebuff_size %d not multiple pkt_len %d\n", __func__, idx, s->pkt_len,
        ops->framebuff_size);
    return -EIO;
  }
  s->st30_pkt_idx = 0;
  s->st30_frame_stat = ST30_TX_STAT_WAIT_FRAME;
  s->st30_frame_idx = 0;
  s->st30_frame_size = ops->framebuff_size;
  rte_atomic32_set(&s->st30_stat_frame_cnt, 0);

  s->st30_rtp_time_app = 0xFFFFFFFF;
  s->st30_rtp_time = 0xFFFFFFFF;

  for (int i = 0; i < num_port; i++) {
    s->has_inflight[i] = false;
    s->inflight_cnt[i] = 0;
  }

  ret = tx_audio_session_init_pacing(impl, s);
  if (ret < 0) {
    err("%s(%d), tx_audio_session_init_pacing fail %d\n", __func__, idx, ret);
    return ret;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tx_audio_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_init_hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  ret = tx_audio_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), init sw fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static void tx_audio_session_stat(struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  int frame_cnt = rte_atomic32_read(&s->st30_stat_frame_cnt);

  rte_atomic32_set(&s->st30_stat_frame_cnt, 0);

  info("TX_AUDIO_SESSION(%d:%s): frame cnt %d, pkt cnt %d, inflight count %d: %d\n", idx,
       s->ops_name, frame_cnt, s->st30_stat_pkt_cnt, s->inflight_cnt[ST_PORT_P],
       s->inflight_cnt[ST_PORT_R]);
  s->st30_stat_pkt_cnt = 0;

  if (s->st30_epoch_mismatch) {
    info("TX_AUDIO_SESSION(%d): st30 epoch mismatch %d\n", idx, s->st30_epoch_mismatch);
    s->st30_epoch_mismatch = 0;
  }
}

static int tx_audio_session_detach(struct st_tx_audio_sessions_mgr* mgr,
                                   struct st_tx_audio_session_impl* s) {
  tx_audio_session_stat(s);
  tx_audio_session_uinit_sw(mgr, s);
  return 0;
}

static int tx_audio_sessions_mgr_detach(struct st_tx_audio_sessions_mgr* mgr,
                                        struct st_tx_audio_session_impl* s, int idx) {
  tx_audio_session_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  st_rte_free(s);
  return 0;
}

int st_tx_audio_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_tx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct st_sch_tasklet_ops ops;
  int ret, i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc3550_audio_hdr) != 54);

  mgr->parnet = impl;
  mgr->idx = idx;

  for (i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  ret = tx_audio_sessions_mgr_init_hw(impl, mgr);
  if (ret < 0) {
    err("%s(%d), tx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_audio_sessions_mgr";
  ops.start = tx_audio_sessions_tasklet_start;
  ops.stop = tx_audio_sessions_tasklet_stop;
  ops.handler = tx_audio_sessions_tasklet_handler;

  ret = st_sch_register_tasklet(sch, &ops);
  if (ret < 0) {
    tx_audio_sessions_mgr_uinit_hw(impl, mgr);
    err("%s(%d), st_sch_register_tasklet fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_tx_audio_sessions_mgr_uinit(struct st_tx_audio_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_audio_session_impl* s;

  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    s = tx_audio_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_audio_sessions_mgr_detach(mgr, s, i);
    tx_audio_session_put(mgr, i);
  }

  tx_audio_sessions_mgr_uinit_hw(impl, mgr);

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

struct st_tx_audio_session_impl* st_tx_audio_sessions_mgr_attach(
    struct st_tx_audio_sessions_mgr* mgr, struct st30_tx_ops* ops) {
  int midx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  int ret;
  struct st_tx_audio_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (!tx_audio_session_get_empty(mgr, i)) continue;

    s = st_rte_zmalloc_socket(sizeof(*s), st_socket_id(impl, ST_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      return NULL;
    }
    ret = tx_audio_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    ret = tx_audio_session_attach(mgr->parnet, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    tx_audio_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_tx_audio_sessions_mgr_detach(struct st_tx_audio_sessions_mgr* mgr,
                                    struct st_tx_audio_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_audio_sessions_mgr_detach(mgr, s, idx);

  tx_audio_session_put(mgr, idx);

  return 0;
}

int st_tx_audio_sessions_mgr_update(struct st_tx_audio_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

void st_tx_audio_sessions_stat(struct st_main_impl* impl) {
  struct st_tx_audio_sessions_mgr* mgr = &impl->tx_a_mgr;
  struct st_tx_audio_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_audio_session_get(mgr, j);
    if (!s) continue;
    tx_audio_session_stat(s);
    tx_audio_session_put(mgr, j);
  }
  if (mgr->st30_stat_pkts_burst) {
    info("TX_AUDIO_SESSION, pkts burst %d\n", mgr->st30_stat_pkts_burst);
    mgr->st30_stat_pkts_burst = 0;
  }
}
