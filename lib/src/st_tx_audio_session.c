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

static int tx_audio_session_alloc_frames(struct st_main_impl* impl,
                                         struct st_tx_audio_session_impl* s,
                                         struct st30_tx_ops* ops) {
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
                                     struct st30_tx_ops* ops,
                                     enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  int ret;
  struct st_rfc3550_audio_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st30_rfc3550_rtp_hdr* rtp = &hdr->rtp;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = st_sip_addr(impl, port);

  /* ether hdr */
  ret = st_dev_dst_ip_mac(impl, dip, &eth->d_addr, port);
  if (ret < 0) {
    err("%s(%d), st_dev_dst_ip_mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0],
        dip[1], dip[2], dip[3]);
    return ret;
  }

  ret = rte_eth_macaddr_get(mgr->port_id[port], &eth->s_addr);
  if (ret < 0) {
    err("%s(%d), rte_eth_macaddr_get fail %d for port %d\n", __func__, idx, ret, port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ipv4 hdr */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0; /* todo: from rte flow? s->fl[portId].tos */
  ipv4->fragment_offset = ST_IP_DONT_FRAGMENT_FLAG;
  ipv4->total_length = htons(s->pkt_len + ST_PKT_AUDIO_HDR_LEN);
  ipv4->next_proto_id = 17;
  memcpy(&ipv4->src_addr, sip, ST_IP_ADDR_LEN);
  memcpy(&ipv4->dst_addr, dip, ST_IP_ADDR_LEN);

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
                                        struct st_tx_audio_session_impl* s,
                                        struct st30_tx_ops* ops) {
  int idx = s->idx;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  int sampling = (ops->sampling == ST30_SAMPLING_48K) ? 48 : 96;

  double frame_time = (double)1000000000.0 * 1 / 1000; /* 1ms */
  pacing->frame_time = frame_time;
  pacing->frame_time_sampling = (double)(sampling * 1000) * 1 / 1000;
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

static int tx_audio_sessions_tasklet_start(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_audio_sessions_tasklet_stop(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;
  int sid;
  struct st_tx_audio_session_impl* s;

  for (sid = 0; sid < ST_MAX_TX_AUDIO_SESSIONS; sid++) {
    if (!mgr->active[sid]) continue;
    s = &mgr->sessions[sid];
    for (int i = 0; i < s->ops.num_port; i++) {
      info("%s(%d), session %d s_port %d, inflight count %d\n", __func__, idx, sid, i,
           s->inflight_cnt[i]);

      if (s->has_inflight[i]) {
        info("%s(%d), session %d, free inflight buf\n", __func__, idx, sid);
        rte_pktmbuf_free(s->inflight[i]);
      }
    }
  }

  return 0;
}

static int tx_audio_session_build_rtp_packet(struct st_main_impl* impl,
                                             struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt, int pkt_idx) {
  struct st30_rfc3550_rtp_hdr* rtp;
  struct st30_tx_ops* ops = &s->ops;
  uint16_t len = s->pkt_len + sizeof(struct st30_rfc3550_rtp_hdr);

  rtp = rte_pktmbuf_mtod(pkt, struct st30_rfc3550_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr->rtp, sizeof(*rtp));

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
  struct st_rfc3550_audio_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc3550_audio_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[s_port].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[s_port].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[s_port].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st30_ipv4_packet_id);
  s->st30_ipv4_packet_id++;

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->next = pkt_rtp;
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);
  pkt->pkt_len = pkt->data_len + pkt_rtp->pkt_len;
  pkt->nb_segs = pkt_rtp->nb_segs + 1;

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
  struct rte_mempool* mbuf_pool_p = st_get_mempool(impl, port_p);
  struct rte_mempool* mbuf_pool_r = NULL;

  if (s->ops.num_port > 1) {
    send_r = true;
    port_r = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_R);
    mbuf_pool_r = st_get_mempool(impl, port_r);
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

  pkt_rtp = rte_pktmbuf_alloc(mbuf_pool_p);
  if (!pkt_rtp) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  pkt = rte_pktmbuf_alloc(mbuf_pool_p);
  if (!pkt) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
    rte_pktmbuf_free(pkt_rtp);
    return -ENOMEM;
  }

  if (send_r) {
    pkt_r = rte_pktmbuf_alloc(mbuf_pool_r);
    if (!pkt_r) {
      err("%s(%d), rte_pktmbuf_alloc redundant fail\n", __func__, idx);
      rte_pktmbuf_free(pkt_rtp);
      rte_pktmbuf_free(pkt);
      return -ENOMEM;
    }
  }

  tx_audio_session_build_rtp_packet(impl, s, pkt_rtp, s->st30_pkt_idx);
  tx_audio_session_build_packet(impl, s, pkt, pkt_rtp, ST_SESSION_PORT_P);
  st_mbuf_set_idx(pkt, s->st30_pkt_idx);
  st_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    struct rte_mbuf* pkt_rtp1 = rte_pktmbuf_clone(pkt_rtp, mbuf_pool_r);
    tx_audio_session_build_packet(impl, s, pkt_r, pkt_rtp1, ST_SESSION_PORT_R);
    st_mbuf_set_idx(pkt_r, s->st30_pkt_idx);
    st_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
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
    s->st30_stat_frame_cnt++;
  }

  return 0;
}

static int tx_audio_session_build_single_rtp(struct st_main_impl* impl,
                                             struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt,
                                             struct rte_mbuf* pkt_chain) {
  struct st_rfc3550_audio_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st30_rfc3550_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc3550_audio_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = rte_pktmbuf_mtod(pkt_chain, struct st30_rfc3550_rtp_hdr*);

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[ST_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[ST_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[ST_SESSION_PORT_P].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st30_ipv4_packet_id);
  s->st30_ipv4_packet_id++;

  if (rtp->tmstamp != s->st30_rtp_time) {
    /* start of a new epoch*/
    s->st30_rtp_time = rtp->tmstamp;
    tx_audio_session_sync_pacing(impl, s, false);
  }
  /* update rtp time*/
  rtp->tmstamp = htonl(s->pacing.cur_time_stamp);

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);
  pkt->pkt_len = pkt->data_len;
  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);
  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  return 0;
}

static int tx_audio_session_build_redundant_rtp(struct st_tx_audio_session_impl* s,
                                                struct rte_mbuf* pkt_r,
                                                struct rte_mbuf* pkt_base,
                                                struct rte_mbuf* pkt_chain) {
  struct rte_ipv4_hdr *ipv4, *ipv4_base;
  struct st_rfc3550_audio_hdr *hdr, *hdr_base;
  hdr = rte_pktmbuf_mtod(pkt_r, struct st_rfc3550_audio_hdr*);
  ipv4 = &hdr->ipv4;
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_rfc3550_audio_hdr*);
  ipv4_base = &hdr_base->ipv4;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[ST_SESSION_PORT_R].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[ST_SESSION_PORT_R].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(&hdr->udp, &s->hdr[ST_SESSION_PORT_R].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = ipv4_base->packet_id;

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  pkt_r->next = pkt_chain;
  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);

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
  struct rte_mempool* mbuf_pool_p = st_get_mempool(impl, port_p);
  struct rte_mempool* mbuf_pool_r = NULL;

  if (s->ops.num_port > 1) {
    send_r = true;
    port_r = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_R);
    mbuf_pool_r = st_get_mempool(impl, port_r);
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
  struct rte_mbuf* pkt_chain = NULL;

  if (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt_chain) != 0) {
    dbg("%s(%d), rtp pkts not ready\n", __func__, idx);
    return -EBUSY;
  }

  s->ops.notify_rtp_done(s->ops.priv);

  pkt = rte_pktmbuf_alloc(mbuf_pool_p);
  if (!pkt) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
    rte_pktmbuf_free(pkt_chain);
    return -ENOMEM;
  }
  if (send_r) {
    pkt_r = rte_pktmbuf_alloc(mbuf_pool_r);
    if (!pkt_r) {
      err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt);
      rte_pktmbuf_free(pkt_chain);
      return -ENOMEM;
    }
  }

  tx_audio_session_build_single_rtp(impl, s, pkt, pkt_chain);
  st_mbuf_set_time_stamp(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    tx_audio_session_build_redundant_rtp(s, pkt_r, pkt, pkt_chain);
    st_mbuf_set_time_stamp(pkt_r, pacing->tsc_time_cursor);
  }
  s->st30_stat_pkt_cnt++;

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
  int i;

  for (i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (mgr->active[i]) {
      if (mgr->sessions[i].ops.type == ST30_TYPE_FRAME_LEVEL)
        tx_audio_session_tasklet_frame(impl, mgr, &mgr->sessions[i]);
      else
        tx_audio_session_tasklet_rtp(impl, mgr, &mgr->sessions[i]);
    }
  }

  return 0;
}

static int tx_audio_session_uinit(struct st_main_impl* impl,
                                  struct st_tx_audio_session_impl* s) {
  tx_audio_session_free_frames(s);
  if (s->packet_ring) {
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
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
  uint16_t queue;

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
    count = 1024;
    ring = rte_ring_create(ring_name, count, st_socket_id(impl, i), flags);
    if (!ring) {
      err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, i);
      tx_audio_sessions_mgr_uinit_hw(impl, mgr);
      return -ENOMEM;
    }
    mgr->ring[i] = ring;
  }

  info("%s(%d), succ, queue %d\n", __func__, mgr_idx, queue);
  return 0;
}

static int tx_audio_session_init_packet_ring(struct st_main_impl* impl,
                                             struct st_tx_audio_sessions_mgr* mgr,
                                             struct st_tx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "TX-AUDIO-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
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

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10100 + idx);
    s->st30_dst_port[i] = s->st30_src_port[i];
  }
  s->st30_ipv4_packet_id = 0;

  /* calculate pkts in line*/
  size_t bytes_in_pkt = ST_PKT_MAX_UDP_BYTES - sizeof(struct st_rfc3550_audio_hdr);
  s->pkt_len = s->ops.sample_size;
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

  ret = tx_audio_session_init_pacing(impl, s, ops);
  if (ret < 0) {
    err("%s(%d), tx_audio_session_init_pacing fail %d\n", __func__, idx, ret);
    return ret;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tx_audio_session_init_hdr(impl, mgr, s, ops, i);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_init_hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  if (s->ops.type == ST30_TYPE_RTP_LEVEL) {
    ret = tx_audio_session_init_packet_ring(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_init_packet_ring fail %d\n", __func__, idx, ret);
      return -EIO;
    }
  }

  if (s->ops.type == ST30_TYPE_FRAME_LEVEL) {
    ret = tx_audio_session_alloc_frames(impl, s, ops);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_alloc_frames fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  info("%s(%d), succ\n", __func__, idx);
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
    ret = tx_audio_session_init(impl, mgr, &mgr->sessions[i], i);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_init fail %d for %d\n", __func__, idx, ret, i);
      return ret;
    }
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
  int idx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  int ret, i;

  for (i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    ret = tx_audio_session_uinit(impl, &mgr->sessions[i]);
    if (ret < 0) {
      err("%s(%d), tx_audio_session_uinit fail %d for %d\n", __func__, idx, ret, i);
    }
  }

  tx_audio_sessions_mgr_uinit_hw(impl, mgr);

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

struct st_tx_audio_session_impl* st_tx_audio_sessions_mgr_attach(
    struct st_tx_audio_sessions_mgr* mgr, struct st30_tx_ops* ops) {
  int midx = mgr->idx;
  int ret, i;
  struct st_tx_audio_session_impl* s;

  for (i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (!mgr->active[i]) {
      s = &mgr->sessions[i];
      ret = tx_audio_session_attach(mgr->parnet, mgr, s, ops);
      if (ret < 0) {
        err("%s(%d), tx_audio_session_attach fail on %d\n", __func__, midx, i);
        return NULL;
      }
      mgr->active[i] = true;
      return s;
    }
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

int st_tx_audio_sessions_mgr_detach(struct st_tx_audio_sessions_mgr* mgr,
                                    struct st_tx_audio_session_impl* s) {
  int midx = mgr->idx;
  int sidx = s->idx;

  if (s != &mgr->sessions[sidx]) {
    err("%s(%d,%d), mismatch session\n", __func__, midx, sidx);
    return -EIO;
  }

  if (s->packet_ring) {
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tx_audio_session_free_frames(s);

  mgr->active[sidx] = false;

  return 0;
}

void st_tx_audio_sessions_stat(struct st_main_impl* impl) {
  struct st_tx_audio_sessions_mgr* mgr = &impl->tx_a_mgr;
  struct st_tx_audio_session_impl* s;

  for (int j = 0; j < ST_MAX_TX_AUDIO_SESSIONS; j++) {
    if (mgr->active[j]) {
      s = &mgr->sessions[j];

      info("TX_AUDIO_SESSION(%d): frame cnt %d, pkt cnt %d\n", j, s->st30_stat_frame_cnt,
           s->st30_stat_pkt_cnt);
      s->st30_stat_frame_cnt = 0;
      s->st30_stat_pkt_cnt = 0;

      if (s->st30_epoch_mismatch) {
        info("TX_AUDIO_SESSION(%d): st30 epoch mismatch %d\n", j, s->st30_epoch_mismatch);
        s->st30_epoch_mismatch = 0;
      }
    }
    if (mgr->st30_stat_pkts_burst) {
      info("TX_AUDIO_SESSION, pkts burst %d\n", mgr->st30_stat_pkts_burst);
      mgr->st30_stat_pkts_burst = 0;
    }
  }
}
