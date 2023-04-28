/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_audio_session.h"

#include "../mt_log.h"
#include "st_audio_transmitter.h"
#include "st_err.h"

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

static int tx_audio_session_free_frames(struct st_tx_audio_session_impl* s) {
  if (s->st30_frames) {
    struct st_frame_trans* frame;

    /* dec ref for current frame */
    frame = &s->st30_frames[s->st30_frame_idx];
    if (rte_atomic32_read(&frame->refcnt)) rte_atomic32_dec(&frame->refcnt);

    for (int i = 0; i < s->st30_frames_cnt; i++) {
      frame = &s->st30_frames[i];
      st_frame_trans_uinit(frame);
    }

    mt_rte_free(s->st30_frames);
    s->st30_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_audio_session_alloc_frames(struct mtl_main_impl* impl,
                                         struct st_tx_audio_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  int soc_id = mt_socket_id(impl, port);
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  if (s->st30_frames) {
    err("%s(%d), st30_frames already alloc\n", __func__, idx);
    return -EIO;
  }

  s->st30_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st30_frames) * s->st30_frames_cnt, soc_id);
  if (!s->st30_frames) {
    err("%s(%d), st30_frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame_info = &s->st30_frames[i];
    rte_atomic32_set(&frame_info->refcnt, 0);
    frame_info->idx = i;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame_info = &s->st30_frames[i];

    void* frame = mt_rte_zmalloc_socket(s->st30_frame_size, soc_id);
    if (!frame) {
      err("%s(%d), rte_malloc %u fail at %d\n", __func__, idx, s->st30_frame_size, i);
      tx_audio_session_free_frames(s);
      return -ENOMEM;
    }
    frame_info->iova = rte_mem_virt2iova(frame);
    frame_info->addr = frame;
    frame_info->flags = ST_FT_FLAG_RTE_MALLOC;
  }

  dbg("%s(%d), succ with %u frames\n", __func__, idx, s->st30_frames_cnt);
  return 0;
}

static int tx_audio_session_init_hdr(struct mtl_main_impl* impl,
                                     struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s,
                                     enum mtl_session_port s_port) {
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct st30_tx_ops* ops = &s->ops;
  int ret;
  struct st_rfc3550_audio_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st_rfc3550_rtp_hdr* rtp = &hdr->rtp;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = mt_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);

  /* ether hdr */
  if ((s_port == MTL_SESSION_PORT_P) && (ops->flags & ST30_TX_FLAG_USER_P_MAC)) {
    rte_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == MTL_SESSION_PORT_R) && (ops->flags & ST30_TX_FLAG_USER_R_MAC)) {
    rte_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_R_TX_MAC\n", __func__);
  } else {
    ret = mt_dev_dst_ip_mac(impl, dip, d_addr, port, MT_DEV_TIMEOUT_INFINITE);
    if (ret < 0) {
      err("%s(%d), get mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0], dip[1],
          dip[2], dip[3]);
      return ret;
    }
  }

  ret = rte_eth_macaddr_get(mgr->port_id[port], mt_eth_s_addr(eth));
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
  ipv4->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ipv4->total_length = htons(s->pkt_len + ST_PKT_AUDIO_HDR_LEN);
  ipv4->next_proto_id = IPPROTO_UDP;
  mtl_memcpy(&ipv4->src_addr, sip, MTL_IP_ADDR_LEN);
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

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

  info("%s(%d,%d), ip %u.%u.%u.%u port %u:%u\n", __func__, idx, s_port, dip[0], dip[1],
       dip[2], dip[3], s->st30_src_port[s_port], s->st30_dst_port[s_port]);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5]);
  return 0;
}

static int tx_audio_session_init_pacing(struct mtl_main_impl* impl,
                                        struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  struct st30_tx_ops* ops = &s->ops;
  double frame_time = st30_get_packet_time(ops->ptime);

  pacing->frame_time = frame_time;
  pacing->frame_time_sampling = (double)(ops->sample_num * 1000) * 1 / 1000;
  pacing->trs = frame_time;

  /* always use MTL_PORT_P for ptp now */
  pacing->cur_epochs = mt_get_ptp_time(impl, MTL_PORT_P) / frame_time;
  pacing->tsc_time_cursor = 0;
  pacing->max_onward_epochs = (double)(NS_PER_S * 1) / frame_time;     /* 1s */
  pacing->max_late_epochs = (double)(NS_PER_S * 1) / frame_time / 100; /* 10ms */
  dbg("%s[%02d], max_onward_epochs %u max_late_epochs %u\n", __func__, idx,
      pacing->max_onward_epochs, pacing->max_late_epochs);

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

static uint64_t tx_audio_pacing_required_tai(struct st_tx_audio_session_impl* s,
                                             enum st10_timestamp_fmt tfmt,
                                             uint64_t timestamp) {
  uint64_t required_tai = 0;

  if (!(s->ops.flags & ST30_TX_FLAG_USER_PACING)) return 0;
  if (!timestamp) return 0;

  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    if (timestamp > 0xFFFFFFFF) {
      err("%s(%d), invalid timestamp %" PRIu64 "\n", __func__, s->idx, timestamp);
    }
    required_tai =
        st10_media_clk_to_ns((uint32_t)timestamp, st30_get_sample_rate(s->ops.sampling));
  } else {
    required_tai = timestamp;
  }

  return required_tai;
}

static int tx_audio_session_sync_pacing(struct mtl_main_impl* impl,
                                        struct st_tx_audio_session_impl* s, bool sync,
                                        uint64_t required_tai) {
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  double frame_time = pacing->frame_time;
  /* always use MTL_PORT_P for ptp now */
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t next_epochs = pacing->cur_epochs + 1;
  uint64_t epochs;
  double to_epoch;

  if (required_tai) {
    uint64_t ptp_epochs = ptp_time / frame_time;
    epochs = required_tai / frame_time;
    dbg("%s(%d), required tai %" PRIu64 " ptp_epochs %" PRIu64 " epochs %" PRIu64 "\n",
        __func__, s->idx, required_tai, ptp_epochs, epochs);
    if (epochs < ptp_epochs) s->stat_error_user_timestamp++;
  } else {
    epochs = ptp_time / frame_time;

    dbg("%s(%d), epochs %" PRIu64 " %" PRIu64 "\n", __func__, s->idx, epochs,
        pacing->cur_epochs);
    if (epochs <= pacing->cur_epochs) {
      uint64_t diff = pacing->cur_epochs - epochs;
      if (diff < pacing->max_onward_epochs) {
        /* point to next epoch since if it in the range of onward */
        epochs = next_epochs;
      }
    } else if (epochs > next_epochs) {
      uint64_t diff = epochs - next_epochs;
      if (diff < pacing->max_late_epochs) {
        /* point to next epoch since if it in the range of late */
        epochs = next_epochs;
        s->stat_epoch_late++;
      }
    }
  }

  to_epoch = tx_audio_pacing_time(pacing, epochs) - ptp_time;
  if (to_epoch < 0) {
    /* time bigger than the assigned epoch time */
    s->stat_epoch_mismatch++;
    to_epoch = 0; /* send asap */
  }

  if (epochs > next_epochs) s->stat_epoch_drop += (epochs - next_epochs);
  if (epochs < next_epochs) s->stat_epoch_onward += (next_epochs - epochs);

  pacing->cur_epochs = epochs;
  pacing->pacing_time_stamp = tx_audio_pacing_time_stamp(pacing, epochs);
  pacing->rtp_time_stamp = pacing->pacing_time_stamp;
  pacing->tsc_time_cursor = (double)mt_get_tsc(impl) + to_epoch;

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, s->idx,
        pacing->tsc_time_cursor, mt_get_tsc(impl));
    mt_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tx_audio_session_init(struct mtl_main_impl* impl,
                                 struct st_tx_audio_sessions_mgr* mgr,
                                 struct st_tx_audio_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static int tx_audio_sessions_tasklet_start(void* priv) { return 0; }

static int tx_audio_sessions_tasklet_stop(void* priv) { return 0; }

static int tx_audio_session_update_redundant(struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;

  /* update the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[MTL_SESSION_PORT_R].eth, sizeof(hdr->eth));
  ipv4->src_addr = s->hdr[MTL_SESSION_PORT_R].ipv4.src_addr;
  ipv4->dst_addr = s->hdr[MTL_SESSION_PORT_R].ipv4.dst_addr;
  udp->src_port = s->hdr[MTL_SESSION_PORT_R].udp.src_port;
  udp->dst_port = s->hdr[MTL_SESSION_PORT_R].udp.dst_port;

  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tx_audio_session_build_packet(struct st_tx_audio_session_impl* s,
                                         struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st_rfc3550_rtp_hdr* rtp;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = (struct st_rfc3550_rtp_hdr*)&udp[1];

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st30_ipv4_packet_id);
  s->st30_ipv4_packet_id++;

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);

  /* build rtp and payload */
  uint16_t len = s->pkt_len + sizeof(struct st_rfc3550_rtp_hdr);

  rte_memcpy(rtp, &s->hdr[MTL_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->seq_number = htons(s->st30_seq_id);
  s->st30_seq_id++;
  rtp->tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* copy payload now */
  uint8_t* payload = (uint8_t*)&rtp[1];
  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  struct st_frame_trans* frame_info = &s->st30_frames[s->st30_frame_idx];
  uint8_t* src = frame_info->addr;
  rte_memcpy(payload, src + offset, s->pkt_len);

  pkt->data_len += len;
  pkt->pkt_len = pkt->data_len;

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tx_audio_session_build_rtp_packet(struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt, int pkt_idx) {
  struct st_rfc3550_rtp_hdr* rtp;
  uint16_t len = s->pkt_len + sizeof(struct st_rfc3550_rtp_hdr);

  rtp = rte_pktmbuf_mtod(pkt, struct st_rfc3550_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr[MTL_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->seq_number = htons(s->st30_seq_id);
  s->st30_seq_id++;
  rtp->tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* copy payload now */
  uint8_t* payload = (uint8_t*)&rtp[1];
  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  struct st_frame_trans* frame_info = &s->st30_frames[s->st30_frame_idx];
  uint8_t* src = frame_info->addr;
  rte_memcpy(payload, src + offset, s->pkt_len);

  pkt->data_len = len;
  pkt->pkt_len = len;

  return 0;
}

static int tx_audio_session_build_packet_chain(struct st_tx_audio_session_impl* s,
                                               struct rte_mbuf* pkt,
                                               struct rte_mbuf* pkt_rtp,
                                               enum mtl_session_port s_port) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st30_tx_ops* ops = &s->ops;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[s_port].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[s_port].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[s_port].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st30_ipv4_packet_id);
  /* update only for primary */
  if (s_port == MTL_SESSION_PORT_P) {
    s->st30_ipv4_packet_id++;
    /* update rtp time for rtp path */
    if (ops->type == ST30_TYPE_RTP_LEVEL) {
      struct st_rfc3550_rtp_hdr* rtp =
          rte_pktmbuf_mtod(pkt_rtp, struct st_rfc3550_rtp_hdr*);
      if (rtp->tmstamp != s->st30_rtp_time_app) {
        /* start of a new epoch */
        s->st30_rtp_time_app = rtp->tmstamp;
        if (s->ops.flags & ST30_TX_FLAG_USER_TIMESTAMP) {
          s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
        }
        s->st30_rtp_time = s->pacing.rtp_time_stamp;
        rte_atomic32_inc(&s->st30_stat_frame_cnt);
      }
      /* update rtp time */
      rtp->tmstamp = htonl(s->st30_rtp_time);
    }
  }

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);
  pkt->pkt_len = pkt->data_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_rtp);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  if (!s->eth_ipv4_cksum_offload[s_port]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  /* rtp packet used twice for redundant path */
  if (s_port == MTL_SESSION_PORT_R) rte_mbuf_refcnt_update(pkt_rtp, 1);

  return 0;
}

static int tx_audio_session_tasklet_frame(struct mtl_main_impl* impl,
                                          struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st30_tx_ops* ops = &s->ops;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = s->trans_ring[MTL_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MT_TASKLET_ALL_DONE;
  }
  if (rte_ring_count(ring_p) > s->trans_ring_thresh) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MT_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->trans_ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[MTL_SESSION_PORT_P]) {
    ret = rte_ring_mp_enqueue(ring_p, (void*)s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->has_inflight[MTL_SESSION_PORT_P] = false;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->has_inflight[MTL_SESSION_PORT_R]) {
    ret = rte_ring_mp_enqueue(ring_r, (void*)s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->has_inflight[MTL_SESSION_PORT_R] = false;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st30_pkt_idx) {
    if (ST30_TX_STAT_WAIT_FRAME == s->st30_frame_stat) {
      uint16_t next_frame_idx;
      struct st30_tx_frame_meta meta;
      memset(&meta, 0, sizeof(meta));
      meta.fmt = ops->fmt;
      meta.channel = ops->channel;
      meta.ptime = ops->ptime;
      meta.sampling = ops->sampling;

      if (s->check_frame_done_time) {
        uint64_t frame_end_time = mt_get_tsc(impl);
        if (frame_end_time > pacing->tsc_time_cursor) {
          s->stat_exceed_frame_time++;
          dbg("%s(%d), frame %d build time out %" PRIu64 " us\n", __func__, idx,
              s->st30_frame_idx, (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
        }
        s->check_frame_done_time = false;
      }

      /* Query next frame buffer idx */
      ret = ops->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (ret < 0) { /* no frame ready from app */
        dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        s->stat_build_ret_code = -STI_FRAME_APP_GET_FRAME_BUSY;
        return MT_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st30_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
            refcnt);
        s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
        return MT_TASKLET_ALL_DONE;
      }
      rte_atomic32_inc(&frame->refcnt);
      frame->ta_meta = meta;
      s->st30_frame_idx = next_frame_idx;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st30_frame_stat = ST30_TX_STAT_SENDING_PKTS;
    }
  }

  if (s->calculate_time_cursor) {
    struct st_frame_trans* frame = &s->st30_frames[s->st30_frame_idx];
    /* user timestamp control if any */
    uint64_t required_tai =
        tx_audio_pacing_required_tai(s, frame->ta_meta.tfmt, frame->ta_meta.timestamp);
    tx_audio_session_sync_pacing(impl, s, false, required_tai);
    if (ops->flags & ST30_TX_FLAG_USER_TIMESTAMP &&
        (frame->ta_meta.tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK)) {
      pacing->rtp_time_stamp = (uint32_t)frame->ta_meta.timestamp;
    }
    frame->ta_meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    frame->ta_meta.timestamp = pacing->rtp_time_stamp;
    s->calculate_time_cursor = false; /* clear */
  }

  if (s->pacing_in_build) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    uint64_t target_tsc = pacing->tsc_time_cursor;
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      // dbg("%s(%d), cur_tsc %"PRIu64" target_tsc %"PRIu64"\n", __func__, idx, cur_tsc,
      // target_tsc);
      if (likely(delta < NS_PER_S)) {
        s->stat_build_ret_code = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
  }

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;

  pkt = rte_pktmbuf_alloc(hdr_pool_p);
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, idx);
    s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
    return MT_TASKLET_ALL_DONE;
  }

  if (!s->tx_no_chain) {
    struct rte_mbuf* pkt_rtp = rte_pktmbuf_alloc(chain_pool);
    if (!pkt_rtp) {
      err("%s(%d), pkt_rtp alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
    tx_audio_session_build_rtp_packet(s, pkt_rtp, s->st30_pkt_idx);
    tx_audio_session_build_packet_chain(s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc redundant fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        return MT_TASKLET_ALL_DONE;
      }
      tx_audio_session_build_packet_chain(s, pkt_r, pkt_rtp, MTL_SESSION_PORT_R);
    }
  } else {
    tx_audio_session_build_packet(s, pkt);
    if (send_r) {
      pkt_r = rte_pktmbuf_copy(pkt, hdr_pool_r, 0, UINT32_MAX);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_copy redundant fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        return MT_TASKLET_ALL_DONE;
      }
      tx_audio_session_update_redundant(s, pkt_r);
    }
  }

  st_tx_mbuf_set_idx(pkt, s->st30_pkt_idx);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  if (send_r) {
    st_tx_mbuf_set_idx(pkt_r, s->st30_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
  }

  s->st30_pkt_idx++;
  s->st30_stat_pkt_cnt++;
  pacing->tsc_time_cursor += pacing->frame_time;
  /* sync pacing for pkt, even in one frame */
  s->calculate_time_cursor = true;

  bool done = false;
  if (rte_ring_mp_enqueue(ring_p, (void*)pkt) != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->has_inflight[MTL_SESSION_PORT_P] = true;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
  }
  if (send_r && rte_ring_mp_enqueue(ring_r, (void*)pkt_r) != 0) {
    s->inflight[MTL_SESSION_PORT_R] = pkt_r;
    s->has_inflight[MTL_SESSION_PORT_R] = true;
    s->inflight_cnt[MTL_SESSION_PORT_R]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
  }

  if (s->st30_pkt_idx >= s->st30_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st30_frame_idx);
    struct st_frame_trans* frame = &s->st30_frames[s->st30_frame_idx];
    /* end of current frame */
    if (s->ops.notify_frame_done)
      ops->notify_frame_done(ops->priv, s->st30_frame_idx, &frame->ta_meta);
    rte_atomic32_dec(&frame->refcnt);
    s->st30_frame_stat = ST30_TX_STAT_WAIT_FRAME;
    s->check_frame_done_time = true;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->st30_stat_frame_cnt);
  }

  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static int tx_audio_session_tasklet_rtp(struct mtl_main_impl* impl,
                                        struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_ring* ring_p = s->trans_ring[MTL_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_RTP_RING_FULL;
    return MT_TASKLET_ALL_DONE;
  }
  if (rte_ring_count(ring_p) > s->trans_ring_thresh) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MT_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->trans_ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[MTL_SESSION_PORT_P]) {
    ret = rte_ring_mp_enqueue(ring_p, (void*)s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->has_inflight[MTL_SESSION_PORT_P] = false;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->has_inflight[MTL_SESSION_PORT_R]) {
    ret = rte_ring_mp_enqueue(ring_r, (void*)s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->has_inflight[MTL_SESSION_PORT_R] = false;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  /* sync pacing */
  if (!pacing->tsc_time_cursor) tx_audio_session_sync_pacing(impl, s, false, 0);

  if (s->pacing_in_build) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    uint64_t target_tsc = pacing->tsc_time_cursor;
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      // dbg("%s(%d), cur_tsc %"PRIu64" target_tsc %"PRIu64"\n", __func__, idx, cur_tsc,
      // target_tsc);
      if (likely(delta < NS_PER_S)) {
        s->stat_build_ret_code = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                                : MT_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
  }

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;
  struct rte_mbuf* pkt_rtp = NULL;

  if (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt_rtp) != 0) {
    dbg("%s(%d), rtp pkts not ready\n", __func__, idx);
    s->stat_build_ret_code = -STI_RTP_APP_DEQUEUE_FAIL;
    return MT_TASKLET_ALL_DONE;
  }
  s->ops.notify_rtp_done(s->ops.priv);

  pkt = rte_pktmbuf_alloc(hdr_pool_p);
  if (!pkt) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
    rte_pktmbuf_free(pkt_rtp);
    s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
    return MT_TASKLET_ALL_DONE;
  }

  if (send_r) {
    pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
    if (!pkt_r) {
      err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt);
      rte_pktmbuf_free(pkt_rtp);
      s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
  }

  tx_audio_session_build_packet_chain(s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);

  if (send_r) {
    tx_audio_session_build_packet_chain(s, pkt_r, pkt_rtp, MTL_SESSION_PORT_R);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
  }
  s->st30_stat_pkt_cnt++;
  pacing->tsc_time_cursor = 0;

  bool done = true;
  if (rte_ring_mp_enqueue(ring_p, (void*)pkt) != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->has_inflight[MTL_SESSION_PORT_P] = true;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
  }
  if (send_r && rte_ring_mp_enqueue(ring_r, (void*)pkt_r) != 0) {
    s->inflight[MTL_SESSION_PORT_R] = pkt_r;
    s->has_inflight[MTL_SESSION_PORT_R] = true;
    s->inflight_cnt[MTL_SESSION_PORT_R]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
  }
  return done ? MT_TASKLET_ALL_DONE : MT_TASKLET_HAS_PENDING;
}

static int tx_audio_session_tasklet_transmit(struct mtl_main_impl* impl,
                                             struct st_tx_audio_sessions_mgr* mgr,
                                             struct st_tx_audio_session_impl* s,
                                             int s_port) {
  int idx = s->idx, ret;
  struct rte_mbuf* pkt;
  enum mtl_port t_port = mt_port_logic2phy(s->port_maps, s_port);
  struct rte_ring* trs_ring = mgr->ring[t_port];
  uint64_t cur_tsc;
  uint64_t target_tsc;

  /* check if any pending pkt */
  pkt = s->trans_ring_inflight[s_port];
  if (pkt) {
    cur_tsc = mt_get_tsc(impl);
    target_tsc = st_tx_mbuf_get_tsc(pkt);
    if (cur_tsc < target_tsc) {
      s->stat_transmit_ret_code = -STI_TSCTRS_INFLIGHT_TSC_NOT_REACH;
      return MT_TASKLET_ALL_DONE;
    }
    ret = rte_ring_sp_enqueue(trs_ring, pkt);
    if (ret < 0) {
      s->stat_transmit_ret_code = -STI_TSCTRS_INFLIGHT_ENQUEUE_FAIL;
      return MT_TASKLET_ALL_DONE;
    }
    s->trans_ring_inflight[s_port] = NULL;
  }

  /* try to dequeue pkt */
  ret = rte_ring_sc_dequeue(s->trans_ring[s_port], (void**)&pkt);
  if (ret < 0) {
    s->stat_transmit_ret_code = -STI_TSCTRS_PKT_DEQUEUE_FAIL;
    return MT_TASKLET_ALL_DONE; /* no pkt */
  }

  cur_tsc = mt_get_tsc(impl);
  target_tsc = st_tx_mbuf_get_tsc(pkt);
  if (cur_tsc < target_tsc) {
    uint64_t delta = target_tsc - cur_tsc;
    // dbg("%s(%d), cur_tsc %"PRIu64" target_tsc %"PRIu64"\n", __func__, idx, cur_tsc,
    // target_tsc);
    if (likely(delta < NS_PER_S)) {
      s->stat_transmit_ret_code = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
      s->trans_ring_inflight[s_port] = pkt;
      return delta < mt_sch_schedule_ns(impl) ? MT_TASKLET_HAS_PENDING
                                              : MT_TASKLET_ALL_DONE;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  ret = rte_ring_sp_enqueue(trs_ring, pkt);
  if (ret < 0) { /* save to inflight */
    s->stat_transmit_ret_code = -STI_TSCTRS_PKT_ENQUEUE_FAIL;
    s->trans_ring_inflight[s_port] = pkt;
    return MT_TASKLET_ALL_DONE;
  }

  return 0;
}

static int tx_audio_sessions_tasklet_handler(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_audio_session_impl* s;
  int pending = MT_TASKLET_ALL_DONE;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_audio_session_try_get(mgr, sidx);
    if (!s) continue;

    s->stat_build_ret_code = 0;
    if (s->ops.type == ST30_TYPE_FRAME_LEVEL)
      pending += tx_audio_session_tasklet_frame(impl, s);
    else
      pending += tx_audio_session_tasklet_rtp(impl, s);
    for (int port = 0; port < s->ops.num_port; port++) {
      pending += tx_audio_session_tasklet_transmit(impl, mgr, s, port);
    }
    tx_audio_session_put(mgr, sidx);
  }

  return pending;
}

static int tx_audio_sessions_mgr_uinit_hw(struct mtl_main_impl* impl,
                                          struct st_tx_audio_sessions_mgr* mgr) {
  for (int i = 0; i < mt_num_ports(impl); i++) {
    if (mgr->ring[i]) {
      rte_ring_free(mgr->ring[i]);
      mgr->ring[i] = NULL;
    }
    if (mgr->queue[i]) {
      mt_dev_put_tx_queue(impl, mgr->queue[i]);
      mgr->queue[i] = NULL;
    }
  }

  dbg("%s(%d), succ\n", __func__, mgr->idx);
  return 0;
}

static int tx_audio_sessions_mgr_init_hw(struct mtl_main_impl* impl,
                                         struct st_tx_audio_sessions_mgr* mgr) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx;

  for (int i = 0; i < mt_num_ports(impl); i++) {
    mgr->port_id[i] = mt_port_id(impl, i);
    /* do we need quota for audio? */
    mgr->queue[i] = mt_dev_get_tx_queue(impl, i, 0);
    if (!mgr->queue[i]) {
      tx_audio_sessions_mgr_uinit_hw(impl, mgr);
      return -EIO;
    }

    snprintf(ring_name, 32, "TX-AUDIO-RING-M%d-P%d", mgr_idx, i);
    flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
    count = ST_TX_AUDIO_SESSIONS_RING_SIZE;
    ring = rte_ring_create(ring_name, count, mt_socket_id(impl, i), flags);
    if (!ring) {
      err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, i);
      tx_audio_sessions_mgr_uinit_hw(impl, mgr);
      return -ENOMEM;
    }
    mgr->ring[i] = ring;
    info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, i,
         mt_dev_tx_queue_id(mgr->queue[i]));
  }

  return 0;
}

static int tx_audio_session_flush_port(struct st_tx_audio_sessions_mgr* mgr,
                                       enum mtl_port port) {
  struct mtl_main_impl* impl = mgr->parent;
  int ret;
  int burst_pkts = mt_if_nb_tx_desc(impl, port);
  struct rte_mbuf* pad = mt_get_pad(impl, port);

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

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];
    if (pool && rte_mempool_in_use_count(pool) &&
        rte_atomic32_read(&mgr->transmitter_started)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_audio_session_flush_port(mgr, mt_port_logic2phy(s->port_maps, i));
      info("%s(%d,%d), flush port %d end\n", __func__, mgr_idx, s_idx, i);

      int retry = 100; /* max 1000ms */
      while (retry > 0) {
        retry--;
        if (!rte_mempool_in_use_count(pool)) break;
        mt_sleep_ms(10);
      }
      info("%s(%d,%d), check in_use retry %d\n", __func__, mgr_idx, s_idx, retry);
    }
  }

  return 0;
}

int tx_audio_session_mempool_free(struct st_tx_audio_session_impl* s) {
  int ret;

  if (s->mbuf_mempool_chain && !s->tx_mono_pool) {
    ret = mt_mempool_free(s->mbuf_mempool_chain);
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i] && !s->tx_mono_pool) {
      ret = mt_mempool_free(s->mbuf_mempool_hdr[i]);
      if (ret >= 0) s->mbuf_mempool_hdr[i] = NULL;
    }
  }

  return 0;
}

static bool tx_audio_session_has_chain_buf(struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  for (int port = 0; port < num_ports; port++) {
    if (!s->eth_has_chain[port]) return false;
  }

  /* all ports capable chain */
  return true;
}

static int tx_audio_session_mempool_init(struct mtl_main_impl* impl,
                                         struct st_tx_audio_sessions_mgr* mgr,
                                         struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum mtl_port port;
  unsigned int n;

  uint16_t hdr_room_size = sizeof(struct mt_udp_hdr);
  uint16_t chain_room_size = s->pkt_len + sizeof(struct st_rfc3550_rtp_hdr);

  if (s->tx_no_chain) {
    hdr_room_size += chain_room_size;
  }

  /* allocate hdr pool */
  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    n = mt_if_nb_tx_desc(impl, port) + ST_TX_AUDIO_SESSIONS_RING_SIZE;
    if (s->tx_mono_pool) {
      s->mbuf_mempool_hdr[i] = mt_get_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx,
           s->mbuf_mempool_hdr[i], i);
    } else if (s->mbuf_mempool_hdr[i]) {
      warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "TXAUDIOHDR-M%d-R%d-P%d", mgr->idx, idx, i);
      struct rte_mempool* mbuf_pool =
          mt_mempool_create(impl, port, pool_name, n, MT_MBUF_CACHE_SIZE,
                            sizeof(struct mt_muf_priv_data), hdr_room_size);
      if (!mbuf_pool) {
        tx_audio_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  /* allocate payload(chain) pool */
  if (!s->tx_no_chain) {
    port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    n = mt_if_nb_tx_desc(impl, port) + ST_TX_AUDIO_SESSIONS_RING_SIZE;
    if (ops->type == ST30_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;

    if (s->tx_mono_pool) {
      s->mbuf_mempool_chain = mt_get_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx,
           s->mbuf_mempool_chain);
    } else if (s->mbuf_mempool_chain) {
      warn("%s(%d), use previous chain mempool\n", __func__, idx);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "TXAUDIOCHAIN-M%d-R%d", mgr->idx, idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create(
          impl, port, pool_name, n, MT_MBUF_CACHE_SIZE, 0, chain_room_size);
      if (!mbuf_pool) {
        tx_audio_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_chain = mbuf_pool;
    }
  }

  return 0;
}

static int tx_audio_session_init_rtp(struct mtl_main_impl* impl,
                                     struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  snprintf(ring_name, 32, "TX-AUDIO-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->packet_ring = ring;

  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
  return 0;
}

static int tx_audio_session_uinit_trans_ring(struct st_tx_audio_session_impl* s) {
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    if (s->trans_ring[port]) {
      mt_ring_dequeue_clean(s->trans_ring[port]);
      rte_ring_free(s->trans_ring[port]);
      s->trans_ring[port] = NULL;
    }
  }

  return 0;
}

static int tx_audio_session_init_trans_ring(struct mtl_main_impl* impl,
                                            struct st_tx_audio_sessions_mgr* mgr,
                                            struct st_tx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = ST_TX_AUDIO_SESSIONS_RING_SIZE;
  int mgr_idx = mgr->idx, idx = s->idx;
  int num_port = s->ops.num_port;
  uint16_t trans_ring_thresh = s->ops.fifo_size;

  /* make sure the ring is smaller than max_onward_epochs */
  while (count > s->pacing.max_onward_epochs) {
    count /= 2;
  }

  for (int port = 0; port < num_port; port++) {
    snprintf(ring_name, 32, "TX-AUDIO-MBUF-RING-M%d-S%d-P%d", mgr_idx, idx, port);
    flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
    ring = rte_ring_create(ring_name, count, mt_socket_id(impl, port), flags);
    if (!ring) {
      err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
      tx_audio_session_uinit_trans_ring(s);
      return -ENOMEM;
    }
    s->trans_ring[port] = ring;
  }

  if (!trans_ring_thresh) {
    trans_ring_thresh = (double)(10 * NS_PER_MS) / s->pacing.frame_time;
    trans_ring_thresh = RTE_MAX(trans_ring_thresh, 2); /* min: 2 frame */
  }
  s->trans_ring_thresh = trans_ring_thresh;

  info("%s(%d,%d), trans_ring_thresh %u\n", __func__, mgr_idx, idx, trans_ring_thresh);
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
    if (s->trans_ring_inflight[port]) {
      info("%s(%d), free inflight buf for port %d\n", __func__, idx, port);
      rte_pktmbuf_free(s->trans_ring_inflight[port]);
      s->trans_ring_inflight[port] = NULL;
    }
  }

  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tx_audio_session_uinit_trans_ring(s);

  tx_audio_session_flush(mgr, s);
  tx_audio_session_mempool_free(s);

  tx_audio_session_free_frames(s);

  return 0;
}

static int tx_audio_session_init_sw(struct mtl_main_impl* impl,
                                    struct st_tx_audio_sessions_mgr* mgr,
                                    struct st_tx_audio_session_impl* s) {
  struct st30_tx_ops* ops = &s->ops;
  int idx = s->idx, ret;

  /* free the pool if any in previous session */
  tx_audio_session_mempool_free(s);
  ret = tx_audio_session_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), mempool init fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  ret = tx_audio_session_init_trans_ring(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), mbuf ring init fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  if (ops->type == ST30_TYPE_RTP_LEVEL) {
    ret = tx_audio_session_init_rtp(impl, mgr, s);
  } else {
    ret = tx_audio_session_alloc_frames(impl, s);
  }
  if (ret < 0) {
    err("%s(%d), mode init fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  return 0;
}

static int tx_audio_session_attach(struct mtl_main_impl* impl,
                                   struct st_tx_audio_sessions_mgr* mgr,
                                   struct st_tx_audio_session_impl* s,
                                   struct st30_tx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10100 + idx);
    if (mt_random_src_port(impl))
      s->st30_src_port[i] = mt_random_port(s->st30_dst_port[i]);
    else
      s->st30_src_port[i] =
          (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st30_dst_port[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = mt_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = mt_if_has_multi_seg(impl, port);
  }
  s->tx_mono_pool = mt_has_tx_mono_pool(impl);
  /* manually disable chain or any port can't support chain */
  s->tx_no_chain = mt_has_tx_no_chain(impl) || !tx_audio_session_has_chain_buf(s);
  s->st30_ipv4_packet_id = 0;

  s->st30_frames_cnt = ops->framebuff_cnt;

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
  s->stat_last_time = mt_get_monotonic_time();

  s->st30_rtp_time_app = 0xFFFFFFFF;
  s->st30_rtp_time = 0xFFFFFFFF;

  for (int i = 0; i < num_port; i++) {
    s->has_inflight[i] = false;
    s->inflight_cnt[i] = 0;
  }
  if (ops->flags & ST30_TX_FLAG_BUILD_PACING) s->pacing_in_build = true;
  s->calculate_time_cursor = true;
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
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->st30_stat_frame_cnt, 0);
  s->stat_last_time = cur_time_ns;

  notice(
      "TX_AUDIO_SESSION(%d:%s): fps %f frame cnt %d, pkt cnt %d, inflight count %d: %d\n",
      idx, s->ops_name, framerate, frame_cnt, s->st30_stat_pkt_cnt,
      s->inflight_cnt[MTL_SESSION_PORT_P], s->inflight_cnt[MTL_SESSION_PORT_R]);
  s->st30_stat_pkt_cnt = 0;

  if (s->stat_epoch_mismatch) {
    notice("TX_AUDIO_SESSION(%d): epoch mismatch %u\n", idx, s->stat_epoch_mismatch);
    s->stat_epoch_mismatch = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_AUDIO_SESSION(%d): epoch drop %u\n", idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_epoch_onward) {
    notice("TX_AUDIO_SESSION(%d): epoch onward %u\n", idx, s->stat_epoch_onward);
    s->stat_epoch_onward = 0;
  }
  if (s->stat_epoch_late) {
    notice("TX_AUDIO_SESSION(%d): epoch late %u\n", idx, s->stat_epoch_late);
    s->stat_epoch_late = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_AUDIO_SESSION(%d): build timeout frames %u\n", idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (frame_cnt <= 0) {
    warn("TX_AUDIO_SESSION(%d): build ret %d, transmit ret %d\n", idx,
         s->stat_build_ret_code, s->stat_transmit_ret_code);
  }

  if (s->stat_error_user_timestamp) {
    notice("TX_AUDIO_SESSION(%d): error user timestamp %u\n", idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }
}

static int tx_audio_session_detach(struct st_tx_audio_sessions_mgr* mgr,
                                   struct st_tx_audio_session_impl* s) {
  tx_audio_session_stat(s);
  tx_audio_session_uinit_sw(mgr, s);
  return 0;
}

static int tx_audio_sessions_mgr_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch,
                                      struct st_tx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mt_sch_tasklet_ops ops;
  int ret, i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc3550_audio_hdr) != 54);

  mgr->parent = impl;
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

  mgr->tasklet = mt_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    tx_audio_sessions_mgr_uinit_hw(impl, mgr);
    err("%s(%d), mt_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_tx_audio_session_impl* tx_audio_sessions_mgr_attach(
    struct st_tx_audio_sessions_mgr* mgr, struct st30_tx_ops* ops) {
  int midx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  int ret;
  struct st_tx_audio_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (!tx_audio_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), mt_socket_id(impl, MTL_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      return NULL;
    }
    ret = tx_audio_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = tx_audio_session_attach(mgr->parent, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      mt_rte_free(s);
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

static int tx_audio_sessions_mgr_detach(struct st_tx_audio_sessions_mgr* mgr,
                                        struct st_tx_audio_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_audio_session_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  tx_audio_session_put(mgr, idx);

  return 0;
}

static int tx_audio_sessions_mgr_update(struct st_tx_audio_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

int st_tx_audio_sessions_mgr_uinit(struct st_tx_audio_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_audio_session_impl* s;

  if (mgr->tasklet) {
    mt_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_TX_AUDIO_SESSIONS; i++) {
    s = tx_audio_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_audio_sessions_mgr_detach(mgr, s);
    tx_audio_session_put(mgr, i);
  }

  tx_audio_sessions_mgr_uinit_hw(impl, mgr);

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

void st_tx_audio_sessions_stat(struct mtl_main_impl* impl) {
  struct st_tx_audio_sessions_mgr* mgr = &impl->tx_a_mgr;
  struct st_tx_audio_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_audio_session_get(mgr, j);
    if (!s) continue;
    tx_audio_session_stat(s);
    tx_audio_session_put(mgr, j);
  }
  if (mgr->st30_stat_pkts_burst > 0) {
    notice("TX_AUDIO_MGR, pkts burst %d\n", mgr->st30_stat_pkts_burst);
    mgr->st30_stat_pkts_burst = 0;
  } else {
    if (mgr->max_idx > 0) {
      for (int i = 0; i < mt_num_ports(impl); i++) {
        warn("TX_AUDIO_MGR: trs ret %d:%d\n", i, mgr->stat_trs_ret_code[i]);
      }
    }
  }
}

static int tx_audio_ops_check(struct st30_tx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > MTL_SESSION_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->dip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->dip_addr[0], ops->dip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST30_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST30_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if ((ops->sample_size <= 0) || (ops->sample_size > MTL_PKT_MAX_RTP_BYTES)) {
      err("%s, invalid sample_size %d\n", __func__, ops->sample_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_done) {
      err("%s, pls set notify_rtp_done\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_tx_audio_init(struct mtl_main_impl* impl) {
  int ret;

  if (impl->tx_a_init) return 0;

  /* create tx audio context */
  ret = tx_audio_sessions_mgr_init(impl, impl->main_sch, &impl->tx_a_mgr);
  if (ret < 0) {
    err("%s, tx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_audio_transmitter_init(impl, impl->main_sch, &impl->tx_a_mgr, &impl->a_trs);
  if (ret < 0) {
    st_tx_audio_sessions_mgr_uinit(&impl->tx_a_mgr);
    err("%s, st_audio_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  impl->tx_a_init = true;
  return 0;
}

st30_tx_handle st30_tx_create(mtl_handle mt, struct st30_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st_tx_audio_session_handle_impl* s_impl;
  struct st_tx_audio_session_impl* s;
  int ret;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  ret = st_tx_audio_init(impl);
  mt_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), mt_socket_id(impl, MTL_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  s = tx_audio_sessions_mgr_attach(&impl->tx_a_mgr, ops);
  mt_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
  if (!s) {
    err("%s, tx_audio_sessions_mgr_attach fail\n", __func__);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_TX_AUDIO;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st30_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, s->idx);
  return s_impl;
}

int st30_tx_free(st30_tx_handle handle) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_tx_audio_session_impl* s;
  int ret, idx;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = tx_audio_sessions_mgr_detach(&impl->tx_a_mgr, s);
  if (ret < 0) err("%s(%d), st_tx_audio_sessions_mgr_detach fail\n", __func__, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  tx_audio_sessions_mgr_update(&impl->tx_a_mgr);
  mt_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

void* st30_tx_get_framebuffer(st30_tx_handle handle, uint16_t idx) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct st_tx_audio_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->ops.framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->ops.framebuff_cnt);
    return NULL;
  }
  if (!s->st30_frames) {
    err("%s, st30_frames not allocated\n", __func__);
    return NULL;
  }

  struct st_frame_trans* frame_info = &s->st30_frames[idx];

  return frame_info->addr;
}

void* st30_tx_get_mbuf(st30_tx_handle handle, void** usrptr) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_audio_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    return NULL;
  }

  if (rte_ring_full(packet_ring)) {
    dbg("%s(%d), packet ring is full\n", __func__, idx);
    return NULL;
  }

  pkt = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  *usrptr = rte_pktmbuf_mtod(pkt, void*);
  return pkt;
}

int st30_tx_put_mbuf(st30_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_audio_session_impl* s;
  int idx, ret;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (!mt_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}
