/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_audio_session.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_stat.h"
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
static inline struct st_tx_audio_session_impl* tx_audio_session_get_timeout(
    struct st_tx_audio_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
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
      st_frame_trans_uinit(frame, NULL);
    }

    mt_rte_free(s->st30_frames);
    s->st30_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_audio_session_alloc_frames(struct st_tx_audio_session_impl* s) {
  int soc_id = s->socket_id;
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
  MTL_MAY_UNUSED(mgr);
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
    ret = mt_dst_ip_mac(impl, dip, d_addr, port, impl->arp_timeout_ms);
    if (ret < 0) {
      err("%s(%d), get mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0], dip[1],
          dip[2], dip[3]);
      return ret;
    }
  }

  ret = mt_macaddr_get(impl, port, mt_eth_s_addr(eth));
  if (ret < 0) {
    err("%s(%d), macaddr get fail %d for port %d\n", __func__, idx, ret, port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ipv4 hdr */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0;
  ipv4->packet_id = 0;
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
  rtp->payload_type =
      ops->payload_type ? ops->payload_type : ST_RARTP_PAYLOAD_TYPE_PCM_AUDIO;
  uint32_t ssrc = ops->ssrc ? ops->ssrc : s->idx + 0x223450;
  rtp->ssrc = htonl(ssrc);

  s->st30_seq_id = 0;
  s->st30_rtp_time = -1;

  info("%s(%d,%d), ip %u.%u.%u.%u port %u:%u payload_type %u\n", __func__, idx, s_port,
       dip[0], dip[1], dip[2], dip[3], s->st30_src_port[s_port], s->st30_dst_port[s_port],
       rtp->payload_type);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx, ssrc %u\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5], ssrc);
  return 0;
}

static int tx_audio_session_init_pacing(struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  struct st30_tx_ops* ops = &s->ops;
  double pkt_time = st30_get_packet_time(ops->ptime);
  if (pkt_time < 0) return -EINVAL;

  pacing->pkt_time_sampling = (double)(s->sample_num * 1000) * 1 / 1000;
  pacing->trs = pkt_time;

  pacing->max_onward_epochs = (double)(NS_PER_S * 1) / pkt_time;     /* 1s */
  pacing->max_late_epochs = (double)(NS_PER_S * 1) / pkt_time / 100; /* 10ms */
  dbg("%s[%02d], max_onward_epochs %u max_late_epochs %u\n", __func__, idx,
      pacing->max_onward_epochs, pacing->max_late_epochs);

  info("%s[%02d], trs %f pkt_time_sampling %f\n", __func__, idx, pacing->trs,
       pacing->pkt_time_sampling);
  return 0;
}

static int tx_audio_session_init_pacing_epoch(struct mtl_main_impl* impl,
                                              struct st_tx_audio_session_impl* s) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  pacing->cur_epochs = ptp_time / pacing->trs;
  return 0;
}

static inline double tx_audio_pacing_time(struct st_tx_audio_session_pacing* pacing,
                                          uint64_t epochs) {
  return epochs * pacing->trs;
}

static inline uint32_t tx_audio_pacing_time_stamp(
    struct st_tx_audio_session_pacing* pacing, uint64_t epochs) {
  uint64_t tmstamp64 = epochs * pacing->pkt_time_sampling;
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
  long double pkt_time = pacing->trs;
  /* always use MTL_PORT_P for ptp now */
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t next_epochs = pacing->cur_epochs + 1;
  uint64_t epochs;
  double to_epoch;
  uint64_t ptp_epochs;
  uint64_t diff;

  if (required_tai) {
    ptp_epochs = ptp_time / pkt_time;
    epochs = (required_tai + pkt_time / 2) / pkt_time;
    if (epochs < ptp_epochs) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
      dbg("%s(%d), required tai %" PRIu64 " ptp_epochs %" PRIu64 " epochs %" PRIu64 "\n",
          __func__, s->idx, required_tai, ptp_epochs, epochs);
    }
  } else {
    epochs = ptp_time / pkt_time;
  }

  dbg("%s(%d), epochs %" PRIu64 " %" PRIu64 "\n", __func__, s->idx, epochs,
      pacing->cur_epochs);
  if (epochs <= pacing->cur_epochs) {
    diff = pacing->cur_epochs - epochs;
    if (diff < pacing->max_onward_epochs) {
      /* point to next epoch since if it in the range of onward */
      epochs = next_epochs;
    }
  } else if (epochs > next_epochs) {
    diff = epochs - next_epochs;
    if (diff < pacing->max_late_epochs) {
      /* point to next epoch since if it in the range of late */
      epochs = next_epochs;
      ST_SESSION_STAT_INC(s, port_user_stats, stat_epoch_late);
    }
  }

  if (required_tai) {
    to_epoch = (double)required_tai - ptp_time;
    if (to_epoch > NS_PER_S) {
      dbg("%s(%d), required tai %" PRIu64 " ptp_epochs %" PRIu64 " epochs %" PRIu64 "\n",
          __func__, s->idx, required_tai, ptp_epochs, epochs);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
      to_epoch = NS_PER_S;  // do our best to slow down
    }
  } else {
    to_epoch = tx_audio_pacing_time(pacing, epochs) - ptp_time;
  }

  if (to_epoch < 0) {
    /* time bigger than the assigned epoch time */
    ST_SESSION_STAT_INC(s, port_user_stats, stat_epoch_mismatch);
    to_epoch = 0; /* send asap */
  }

  if (epochs > next_epochs) {
    ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_drop,
                        (epochs - next_epochs));

    if (s->ops.notify_frame_late) {
      s->ops.notify_frame_late(s->ops.priv, epochs - next_epochs);
    }
  }

  if (epochs < next_epochs) {
    ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_onward,
                        (next_epochs - epochs));
  }

  pacing->cur_epochs = epochs;

  if (required_tai) {
    pacing->ptp_time_cursor = required_tai + pkt_time;  // prepare next packet
    /*
     * Cast [double] to intermediate [uint64_t] to extract 32 least significant bits.
     * If calculated time stored in [double] is larger than max uint32_t,
     * then result of direct cast to [uint32_t] results in max uint32_t which is not
     * what we want. "& 0xffffffff" is used to extract 32 least significant bits
     * without compiler trying to optimize-out intermediate cast.
     */
    pacing->rtp_time_stamp =
        ((uint64_t)((required_tai / pkt_time) * pacing->pkt_time_sampling) & 0xffffffff);
  } else {
    pacing->ptp_time_cursor = tx_audio_pacing_time(pacing, epochs);
    pacing->rtp_time_stamp = tx_audio_pacing_time_stamp(pacing, epochs);
  }

  if (s->ops.rtp_timestamp_delta_us) {
    double rtp_timestamp_delta_us = s->ops.rtp_timestamp_delta_us;
    int32_t rtp_timestamp_delta =
        (rtp_timestamp_delta_us * NS_PER_US) * pacing->pkt_time_sampling / pkt_time;
    pacing->rtp_time_stamp += rtp_timestamp_delta;
  }
  pacing->tsc_time_cursor = (double)mt_get_tsc(impl) + to_epoch;
  dbg("%s(%d), epochs %" PRIu64 ", rtp_time_stamp %u\n", __func__, s->idx, epochs,
      pacing->rtp_time_stamp);

  if (sync) {
    dbg("%s(%d), delay to epoch_time %" PRIu64 ", cur %" PRIu64 "\n", __func__, s->idx,
        pacing->tsc_time_cursor, mt_get_tsc(impl));
    mt_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tx_audio_session_init_next_meta(struct st_tx_audio_session_impl* s,
                                           struct st30_tx_frame_meta* meta) {
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  struct st30_tx_ops* ops = &s->ops;

  memset(meta, 0, sizeof(*meta));
  meta->fmt = ops->fmt;
  meta->channel = ops->channel;
  meta->ptime = ops->ptime;
  meta->sampling = ops->sampling;
  /* point to next epoch */
  meta->epoch = pacing->cur_epochs + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tx_audio_pacing_time(pacing, meta->epoch);
  return 0;
}

static int tx_audio_session_init(struct st_tx_audio_sessions_mgr* mgr,
                                 struct st_tx_audio_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
  s->idx = idx;
  return 0;
}

static int tx_audio_sessions_tasklet_start(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_audio_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_audio_session_get(mgr, sidx);
    if (!s) continue;

    tx_audio_session_init_pacing_epoch(impl, s);
    tx_audio_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_audio_session_update_redundant(struct st_tx_audio_session_impl* s,
                                             struct rte_mbuf* pkt_r) {
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt_r, struct mt_udp_hdr*);
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;

  /* update the hdr: eth, ip, udp */
  rte_memcpy(hdr, &s->hdr[MTL_SESSION_PORT_R], sizeof(*hdr));

  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);

  udp->dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
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
  rtp = (struct st_rfc3550_rtp_hdr*)((uint8_t*)udp + sizeof(struct rte_udp_hdr));

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

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
                                             struct rte_mbuf* pkt) {
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

static int tx_audio_session_rtp_update_packet(struct st_tx_audio_session_impl* s,
                                              struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st_rfc3550_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp =
      rte_pktmbuf_mtod_offset(pkt, struct st_rfc3550_rtp_hdr*, sizeof(struct mt_udp_hdr));

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

  if (rtp->tmstamp != s->st30_rtp_time_app) {
    /* start of a new epoch */
    s->st30_rtp_time_app = rtp->tmstamp;
    if (s->ops.flags & ST30_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    }
    s->st30_rtp_time = s->pacing.rtp_time_stamp;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
  }
  /* update rtp time */
  rtp->tmstamp = htonl(s->st30_rtp_time);

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);

  /* update udp header */
  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

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
  /* update only for primary */
  if (s_port == MTL_SESSION_PORT_P) {
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
        rte_atomic32_inc(&s->stat_frame_cnt);
        s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
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

static int tx_audio_session_usdt_dump_close(struct st_tx_audio_session_impl* s) {
  int idx = s->idx;

  if (s->usdt_dump_fd >= 0) {
    info("%s(%d), close fd %d, dumped frames %d\n", __func__, idx, s->usdt_dump_fd,
         s->usdt_dumped_frames);
    close(s->usdt_dump_fd);
    s->usdt_dump_fd = -1;
  }
  return 0;
}

static int tx_audio_session_usdt_dump_frame(struct st_tx_audio_session_impl* s,
                                            struct st_frame_trans* frame) {
  struct st_tx_audio_sessions_mgr* mgr = s->mgr;
  int idx = s->idx;
  int ret;

  if (s->usdt_dump_fd < 0) {
    struct st30_tx_ops* ops = &s->ops;
    snprintf(s->usdt_dump_path, sizeof(s->usdt_dump_path),
             "imtl_usdt_st30tx_m%ds%d_%d_%d_c%u_XXXXXX.pcm", mgr->idx, idx,
             st30_get_sample_rate(ops->sampling), st30_get_sample_size(ops->fmt) * 8,
             ops->channel);
    ret = mt_mkstemps(s->usdt_dump_path, strlen(".pcm"));
    if (ret < 0) {
      err("%s(%d), mkstemps %s fail %d\n", __func__, idx, s->usdt_dump_path, ret);
      return ret;
    }
    s->usdt_dump_fd = ret;
    info("%s(%d), mkstemps succ on %s fd %d\n", __func__, idx, s->usdt_dump_path,
         s->usdt_dump_fd);
  }

  /* write frame to dump file */
  ssize_t n = write(s->usdt_dump_fd, frame->addr, s->st30_frame_size);
  if (n != s->st30_frame_size) {
    warn("%s(%d), write fail %" PRIu64 "\n", __func__, idx, n);
  } else {
    s->usdt_dumped_frames++;
    /* logging every 1 sec */
    if ((s->usdt_dumped_frames % (s->frames_per_sec * 1)) == 0) {
      MT_USDT_ST30_TX_FRAME_DUMP(mgr->idx, s->idx, s->usdt_dump_path,
                                 s->usdt_dumped_frames);
    }
  }

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
  struct mt_u64_fifo* ring_p = s->trans_ring[MTL_SESSION_PORT_P];
  struct mt_u64_fifo* ring_r = NULL;

  if (mt_u64_fifo_full(ring_p)) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }
  if (mt_u64_fifo_count(ring_p) >= s->trans_ring_thresh) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (ops->num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->trans_ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P]) {
    ret = mt_u64_fifo_put(ring_p, (uint64_t)s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = mt_u64_fifo_put(ring_r, (uint64_t)s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_R] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st30_pkt_idx) {
    if (ST30_TX_STAT_WAIT_FRAME == s->st30_frame_stat) {
      uint16_t next_frame_idx;
      struct st30_tx_frame_meta meta;
      uint64_t tsc_start = 0;

      if (s->check_frame_done_time) {
        uint64_t frame_end_time = mt_get_tsc(impl);
        if (frame_end_time > pacing->tsc_time_cursor) {
          ST_SESSION_STAT_INC(s, port_user_stats.common, stat_exceed_frame_time);
          dbg("%s(%d), frame %d build time out %" PRIu64 " us\n", __func__, idx,
              s->st30_frame_idx, (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
        }
        s->check_frame_done_time = false;
      }

      tx_audio_session_init_next_meta(s, &meta);
      /* Query next frame buffer idx */
      bool time_measure = mt_sessions_time_measure(impl);
      if (time_measure) tsc_start = mt_get_tsc(impl);
      ret = ops->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (time_measure) {
        uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
        s->stat_max_next_frame_us = RTE_MAX(s->stat_max_next_frame_us, delta_us);
      }
      if (ret < 0) { /* no frame ready from app */
        dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        s->stat_build_ret_code = -STI_FRAME_APP_GET_FRAME_BUSY;
        return MTL_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st30_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
            refcnt);
        s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
        return MTL_TASKLET_ALL_DONE;
      }
      rte_atomic32_inc(&frame->refcnt);
      frame->ta_meta = meta;
      s->st30_frame_idx = next_frame_idx;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st30_frame_stat = ST30_TX_STAT_SENDING_PKTS;
      MT_USDT_ST30_TX_FRAME_NEXT(s->mgr->idx, s->idx, next_frame_idx, frame->addr);
      /* check if dump USDT enabled */
      if (MT_USDT_ST30_TX_FRAME_DUMP_ENABLED()) {
        tx_audio_session_usdt_dump_frame(s, frame);
      } else {
        tx_audio_session_usdt_dump_close(s);
      }
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
    frame->ta_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
    frame->ta_meta.timestamp = pacing->ptp_time_cursor;
    frame->ta_meta.rtp_timestamp = pacing->rtp_time_stamp;
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
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
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
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (!s->tx_no_chain) {
    struct rte_mbuf* pkt_rtp = rte_pktmbuf_alloc(chain_pool);
    if (!pkt_rtp) {
      err("%s(%d), pkt_rtp alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
    tx_audio_session_build_rtp_packet(s, pkt_rtp);
    tx_audio_session_build_packet_chain(s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc redundant fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
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
        return MTL_TASKLET_ALL_DONE;
      }
      tx_audio_session_update_redundant(s, pkt_r);
    }
  }

  st_tx_mbuf_set_idx(pkt, s->st30_pkt_idx);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P]++;
  s->port_user_stats.common.port[MTL_SESSION_PORT_P].packets++;
  if (send_r) {
    st_tx_mbuf_set_idx(pkt_r, s->st30_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
    s->port_user_stats.common.port[MTL_SESSION_PORT_R].packets++;
  }

  s->st30_pkt_idx++;
  pacing->tsc_time_cursor += pacing->trs;
  /* sync pacing for pkt, even in one frame */
  s->calculate_time_cursor = true;

  bool done = false;
  if (mt_u64_fifo_put(ring_p, (uint64_t)pkt) != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
  }
  if (send_r && mt_u64_fifo_put(ring_r, (uint64_t)pkt_r) != 0) {
    s->inflight[MTL_SESSION_PORT_R] = pkt_r;
    s->inflight_cnt[MTL_SESSION_PORT_R]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
  }

  if (s->st30_pkt_idx >= s->st30_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st30_frame_idx);
    struct st_frame_trans* frame = &s->st30_frames[s->st30_frame_idx];
    struct st30_tx_frame_meta* ta_meta = &frame->ta_meta;
    uint64_t tsc_start = 0;
    bool time_measure = mt_sessions_time_measure(impl);
    if (time_measure) tsc_start = mt_get_tsc(impl);
    /* end of current frame */
    if (ops->notify_frame_done)
      ops->notify_frame_done(ops->priv, s->st30_frame_idx, ta_meta);
    if (time_measure) {
      uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
      s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
    }

    rte_atomic32_dec(&frame->refcnt);
    s->st30_frame_stat = ST30_TX_STAT_WAIT_FRAME;
    s->check_frame_done_time = true;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    MT_USDT_ST30_TX_FRAME_DONE(s->mgr->idx, s->idx, s->st30_frame_idx,
                               ta_meta->rtp_timestamp);
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tx_audio_session_tasklet_rtp(struct mtl_main_impl* impl,
                                        struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_audio_session_pacing* pacing = &s->pacing;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct mt_u64_fifo* ring_p = s->trans_ring[MTL_SESSION_PORT_P];
  struct mt_u64_fifo* ring_r = NULL;

  if (mt_u64_fifo_full(ring_p)) {
    s->stat_build_ret_code = -STI_RTP_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }
  if (mt_u64_fifo_count(ring_p) >= s->trans_ring_thresh) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->trans_ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P]) {
    ret = mt_u64_fifo_put(ring_p, (uint64_t)s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = mt_u64_fifo_put(ring_r, (uint64_t)s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_R] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
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
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
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
    return MTL_TASKLET_ALL_DONE;
  }
  s->ops.notify_rtp_done(s->ops.priv);

  if (!s->tx_no_chain) {
    pkt = rte_pktmbuf_alloc(hdr_pool_p);
    if (!pkt) {
      err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
      rte_pktmbuf_free(pkt_rtp);
      s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }

    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
    }
  }

  if (s->tx_no_chain) {
    pkt = pkt_rtp;
    tx_audio_session_rtp_update_packet(s, pkt);
  } else {
    tx_audio_session_build_packet_chain(s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
  }
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P]++;
  s->port_user_stats.common.port[MTL_SESSION_PORT_P].packets++;

  if (send_r) {
    if (s->tx_no_chain) {
      pkt_r = rte_pktmbuf_copy(pkt, hdr_pool_r, 0, UINT32_MAX);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_copy fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
      tx_audio_session_update_redundant(s, pkt_r);
    } else {
      tx_audio_session_build_packet_chain(s, pkt_r, pkt_rtp, MTL_SESSION_PORT_R);
    }
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
    s->port_user_stats.common.port[MTL_SESSION_PORT_R].packets++;
  }
  pacing->tsc_time_cursor = 0;

  bool done = true;
  if (mt_u64_fifo_put(ring_p, (uint64_t)pkt) != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
  }
  if (send_r && mt_u64_fifo_put(ring_r, (uint64_t)pkt_r) != 0) {
    s->inflight[MTL_SESSION_PORT_R] = pkt_r;
    s->inflight_cnt[MTL_SESSION_PORT_R]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
  }
  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
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
      return MTL_TASKLET_ALL_DONE;
    }
    if (s->queue[s_port]) {
      uint16_t tx = mt_txq_burst(s->queue[s_port], &pkt, 1);
      if (tx < 1) {
        s->stat_transmit_ret_code = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
    } else {
      ret = rte_ring_mp_enqueue(trs_ring, pkt);
      if (ret < 0) {
        s->stat_transmit_ret_code = -STI_TSCTRS_INFLIGHT_ENQUEUE_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
    }
    s->trans_ring_inflight[s_port] = NULL;

    bool time_measure = mt_sessions_time_measure(impl);
    if (time_measure) {
      uint64_t delta_ns = cur_tsc - target_tsc;
      mt_stat_u64_update(&s->stat_tx_delta, delta_ns);
    }
  }

  /* try to dequeue pkt */
  ret = mt_u64_fifo_get(s->trans_ring[s_port], (uint64_t*)&pkt);
  if (ret < 0) {
    s->stat_transmit_ret_code = -STI_TSCTRS_PKT_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE; /* no pkt */
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
      return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                              : MTL_TASKLET_ALL_DONE;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_tsc, target_tsc);
    }
  }

  if (s->queue[s_port]) {
    uint16_t tx = mt_txq_burst(s->queue[s_port], &pkt, 1);
    if (tx < 1) {
      s->stat_transmit_ret_code = -STI_TSCTRS_BURST_FAIL;
      s->trans_ring_inflight[s_port] = pkt;
      return MTL_TASKLET_ALL_DONE;
    }
  } else {
    ret = rte_ring_mp_enqueue(trs_ring, pkt);
    if (ret < 0) { /* save to inflight */
      s->stat_transmit_ret_code = -STI_TSCTRS_PKT_ENQUEUE_FAIL;
      s->trans_ring_inflight[s_port] = pkt;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  bool time_measure = mt_sessions_time_measure(impl);
  if (time_measure) {
    uint64_t delta_ns = cur_tsc - target_tsc;
    mt_stat_u64_update(&s->stat_tx_delta, delta_ns);
  }

  return 0;
}

static const char* audio_pacing_way_names[ST30_TX_PACING_WAY_MAX] = {
    "auto",
    "ratelimit",
    "tsc",
};

const char* audio_pacing_way_name(enum st30_tx_pacing_way way) {
  return audio_pacing_way_names[way];
}

static int tx_audio_session_uinit_rl(struct mtl_main_impl* impl,
                                     struct st_tx_audio_session_impl* s) {
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  MTL_MAY_UNUSED(impl);

  for (int i = 0; i < s->ops.num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[i];

    for (int j = 0; j < ST30_TX_RL_QUEUES_USED; j++) {
      if (rl_port->queue[j]) {
        mt_txq_done_cleanup(rl_port->queue[j]);
        mt_txq_flush(rl_port->queue[j], mt_get_pad(impl, port));
        mt_txq_done_cleanup(rl_port->queue[j]);
        mt_txq_put(rl_port->queue[j]);
        rl_port->queue[j] = NULL;
      }
    }

    if (rl_port->pad) {
      rte_pktmbuf_free(rl_port->pad);
      rl_port->pad = NULL;
    }
    if (rl_port->pkt) {
      rte_pktmbuf_free(rl_port->pkt);
      rl_port->pkt = NULL;
    }
  }
  return 0;
}

static inline uint64_t tx_audio_session_initial_rl_bps(
    struct st_tx_audio_session_impl* s) {
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  double bps = (double)(s->st30_pkt_size + rl->pad_pkt_size * rl->pads_per_st30_pkt) *
               (double)NS_PER_S / s->pacing.trs;
  return bps;
}

static int double_cmp(const void* a, const void* b) {
  const double* ai = a;
  const double* bi = b;

  if (*ai < *bi) {
    return -1;
  } else if (*ai > *bi) {
    return 1;
  }
  return 0;
}

static inline uint64_t tx_audio_session_profiling_rl_bps(
    struct mtl_main_impl* impl, struct st_tx_audio_session_impl* s,
    enum mtl_session_port s_port, uint64_t initial_bytes_per_sec, int rl_q_idx) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  int idx = s->idx;
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[s_port];
  struct mt_txq_entry* queue = rl_port->queue[rl_q_idx];

  /* wait tsc calibrate done */
  mt_wait_tsc_stable(impl);

  uint64_t train_start_tsc = mt_get_tsc(impl);

  /* warm-up stage to consume all nix tx buf */
  int pad_pkts = mt_if_nb_tx_desc(impl, port) * 1;
  struct rte_mbuf* pad = rl_port->pad;
  for (int i = 0; i < pad_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    mt_txq_burst_busy(queue, &pad, 1, 10);
  }

  /* profiling stage */
  double expect_per_sec = NS_PER_S / s->pacing.trs;
  int total = expect_per_sec / 5;
  int loop_cnt = 10;
  double loop_actual_per_sec[loop_cnt];
  for (int loop = 0; loop < loop_cnt; loop++) {
    uint64_t tsc_start = mt_get_tsc(impl);
    for (int i = 0; i < total; i++) {
      pad = rl_port->pkt;
      rte_mbuf_refcnt_update(pad, 1);
      mt_txq_burst_busy(queue, &pad, 1, 10);

      pad = rl_port->pad;
      rte_mbuf_refcnt_update(pad, rl->pads_per_st30_pkt);
      for (int i = 0; i < rl->pads_per_st30_pkt; i++) {
        mt_txq_burst_busy(queue, &pad, 1, 10);
      }
    }
    uint64_t tsc_end = mt_get_tsc(impl);
    double time_sec = (double)(tsc_end - tsc_start) / NS_PER_S;
    loop_actual_per_sec[loop] = total / time_sec;
    dbg("%s(%d), pkts per second expect %f actual %f\n", __func__, idx, expect_per_sec,
        loop_actual_per_sec[loop]);
  }
  /* sort */
  qsort(loop_actual_per_sec, loop_cnt, sizeof(double), double_cmp);
  double actual_per_sec_sum = 0;
  int entry_in_sum = 0;
  for (int i = 1; i < (loop_cnt - 1); i++) {
    actual_per_sec_sum += loop_actual_per_sec[i];
    entry_in_sum++;
  }
  double actual_per_sec = actual_per_sec_sum / entry_in_sum;
  double ratio = actual_per_sec / expect_per_sec;
  if (ratio > 1.15 || ratio < 0.9) {
    err("%s(%d), fail, expect %f but actual %f\n", __func__, idx, expect_per_sec,
        actual_per_sec);
    return 0;
  }
  info("%s(%d), pkts per second, expect %f actual %f with time %fs\n", __func__, idx,
       expect_per_sec, actual_per_sec,
       ((double)mt_get_tsc(impl) - train_start_tsc) / NS_PER_S);
  return initial_bytes_per_sec * expect_per_sec / actual_per_sec;
}

static int tx_audio_session_init_rl(struct mtl_main_impl* impl,
                                    struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  enum mtl_port port;
  uint16_t queue_id;
  uint64_t profiled_per_sec = 0;

  rl->pad_pkt_size = MTL_UDP_MAX_BYTES;
  if (s->ops.rl_accuracy_ns) {
    rl->required_accuracy_ns = s->ops.rl_accuracy_ns; /* 40us */
    info("%s(%d), user required accuracy %uns\n", __func__, idx,
         rl->required_accuracy_ns);
  } else {
    rl->required_accuracy_ns = 40 * NS_PER_US; /* 40us */
  }
  if (s->ops.rl_offset_ns) {
    info("%s(%d), user required offset %dns\n", __func__, idx, s->ops.rl_offset_ns);
  }
  rl->pkts_prepare_warmup = 4;
  rl->pads_per_st30_pkt = 3;
  rl->max_warmup_trs = 4; /* max 4 trs warmup sync */
  /* sync every 10ms */
  rl->pkts_per_sync = (double)NS_PER_S / s->pacing.trs / 100;

  for (int i = 0; i < s->ops.num_port; i++) {
    struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[i];
    port = mt_port_logic2phy(s->port_maps, i);

    uint64_t initial_bytes_per_sec = tx_audio_session_initial_rl_bps(s);
    int profiled = mt_pacing_train_bps_result_search(impl, port, initial_bytes_per_sec,
                                                     &profiled_per_sec);

    /* pad pkt */
    rl_port->pad = mt_build_pad(impl, mt_sys_tx_mempool(impl, port), port,
                                RTE_ETHER_TYPE_IPV4, rl->pad_pkt_size);
    if (!rl_port->pad) {
      tx_audio_session_uinit_rl(impl, s);
      return -ENOMEM;
    }
    rl_port->pkt = mt_build_pad(impl, mt_sys_tx_mempool(impl, port), port,
                                RTE_ETHER_TYPE_IPV4, s->st30_pkt_size);
    if (!rl_port->pkt) {
      tx_audio_session_uinit_rl(impl, s);
      return -ENOMEM;
    }

    for (int j = 0; j < ST30_TX_RL_QUEUES_USED; j++) {
      struct mt_txq_flow flow;
      memset(&flow, 0, sizeof(flow));
      if (profiled < 0)
        flow.bytes_per_sec = initial_bytes_per_sec;
      else
        flow.bytes_per_sec = profiled_per_sec;
      mtl_memcpy(&flow.dip_addr, &s->ops.dip_addr[i], MTL_IP_ADDR_LEN);
      flow.dst_port = s->ops.udp_port[i];
      flow.gso_sz = s->st30_pkt_size - sizeof(struct mt_udp_hdr);
      rl_port->queue[j] = mt_txq_get(impl, port, &flow);
      if (!rl_port->queue[j]) {
        tx_audio_session_uinit_rl(impl, s);
        return -EIO;
      }
      if ((j == 0) && (profiled < 0)) { /* only profile on the first */
        uint64_t trained =
            tx_audio_session_profiling_rl_bps(impl, s, i, initial_bytes_per_sec, j);
        if (!trained) {
          tx_audio_session_uinit_rl(impl, s);
          return -EIO;
        }

        mt_pacing_train_bps_result_add(impl, port, initial_bytes_per_sec, trained);
        info("%s(%d), trained bytes_per_sec %" PRIu64 "\n", __func__, idx, trained);
        int ret = mt_txq_set_tx_bps(rl_port->queue[j], trained);
        if (ret < 0) {
          tx_audio_session_uinit_rl(impl, s);
          return ret;
        }
        initial_bytes_per_sec = trained;
      }
      queue_id = mt_txq_queue_id(rl_port->queue[j]);
      info("%s(%d), port(l:%d,p:%d), queue %d at sync %d\n", __func__, idx, i, port,
           queue_id, j);
    }
  }

  return 0;
}

static void tx_audio_session_rl_switch_queue(
    struct st_tx_audio_session_rl_port* rl_port) {
  int cur_queue = rl_port->cur_queue;
  cur_queue++;
  if (cur_queue >= ST30_TX_RL_QUEUES_USED) cur_queue = 0;
  rl_port->cur_queue = cur_queue;
}

static void tx_audio_session_rl_inc_pkt_idx(struct st_tx_audio_session_rl_info* rl,
                                            struct st_tx_audio_session_rl_port* rl_port) {
  rl_port->cur_pkt_idx++;
  if (rl_port->cur_pkt_idx >= rl->pkts_per_sync) {
    dbg("%s, switch to next queue, cur queue %d pkts %d send\n", __func__,
        rl_port->cur_queue, rl_port->cur_pkt_idx);
    rl_port->cur_pkt_idx = 0;
    tx_audio_session_rl_switch_queue(rl_port);
  }
}

static uint16_t tx_audio_session_rl_tx_pkt(struct st_tx_audio_session_impl* s, int s_port,
                                           struct rte_mbuf* pkt) {
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[s_port];
  int pads_per_st30_pkt = rl->pads_per_st30_pkt;
  int cur_queue = rl_port->cur_queue;
  struct mt_txq_entry* queue = rl_port->queue[cur_queue];
  struct rte_mbuf* pads[pads_per_st30_pkt];
  uint16_t tx;
  uint16_t burst_size = 1;

  tx = mt_txq_burst(queue, &pkt, 1);
  if (tx < 1) {
    dbg("%s(%d,%d), sending pkt fail\n", __func__, s->idx, s_port);
    return 0;
  }
  rl_port->stat_pkts_burst += burst_size;
  s->port_user_stats.common.port[s_port].packets += burst_size;
  s->port_user_stats.stat_pkts_burst += burst_size;

  /* insert the pads */
  for (int i = 0; i < pads_per_st30_pkt; i++) {
    pads[i] = rl_port->pad;
  }
  rte_mbuf_refcnt_update(rl_port->pad, pads_per_st30_pkt);
  tx = mt_txq_burst(queue, pads, pads_per_st30_pkt);
  rl_port->stat_pad_pkts_burst += tx;
  s->port_user_stats.common.port[s_port].packets += tx;
  s->port_user_stats.stat_pkts_burst += tx;
  if (tx != pads_per_st30_pkt) {
    dbg("%s(%d,%d), sending %u pad pkts only %u succ\n", __func__, s->idx, s_port,
        pads_per_st30_pkt, tx);
    /* save to pad inflight */
    rl_port->trs_pad_inflight_num = pads_per_st30_pkt - tx;
  } else {
    tx_audio_session_rl_inc_pkt_idx(rl, rl_port);
  }

  return 1;
}

static uint16_t tx_audio_session_rl_warmup_pkt(struct st_tx_audio_session_impl* s,
                                               int s_port, int pre, int pkts) {
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[s_port];
  int cur_queue = rl_port->cur_queue;
  struct mt_txq_entry* queue = rl_port->queue[cur_queue];
  struct rte_mbuf* pad;

  /* sending the prepare warmup pkts */
  pad = rl_port->pad;
  rte_mbuf_refcnt_update(pad, pre);
  for (int i = 0; i < pre; i++) {
    mt_txq_burst(queue, &pad, 1);
  }
  rl_port->stat_warmup_pkts_burst += pre;
  s->port_user_stats.common.port[s_port].packets += pre;
  s->port_user_stats.stat_pkts_burst += pre;

  /* sending the pattern pkts */
  for (int i = 0; i < pkts; i++) {
    pad = rl_port->pkt;
    rte_mbuf_refcnt_update(pad, 1);
    mt_txq_burst(queue, &pad, 1);

    pad = rl_port->pad;
    rte_mbuf_refcnt_update(pad, rl->pads_per_st30_pkt);
    for (int j = 0; j < rl->pads_per_st30_pkt; j++) {
      mt_txq_burst(queue, &pad, 1);
    }
  }
  uint64_t warmup_pkts_burst = ((uint64_t)pkts * rl->pads_per_st30_pkt);
  rl_port->stat_warmup_pkts_burst += warmup_pkts_burst;
  s->port_user_stats.stat_pkts_burst += warmup_pkts_burst;
  s->port_user_stats.common.port[s_port].packets += warmup_pkts_burst;

  return 0;
}

static uint16_t tx_audio_session_rl_first_pkt(struct mtl_main_impl* impl,
                                              struct st_tx_audio_session_impl* s,
                                              int s_port, struct rte_mbuf* pkt) {
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[s_port];
  uint64_t target_tsc = rl_port->trs_target_tsc + s->ops.rl_offset_ns;
  uint64_t cur_tsc;

  cur_tsc = mt_get_tsc(impl);
  if (cur_tsc > target_tsc) { /* time already reach */
    dbg("%s(%d,%d), warmup fail, cur %" PRIu64 " target %" PRIu64 ", burst directly\n",
        __func__, s->idx, s_port, cur_tsc, target_tsc);
    rl_port->trs_target_tsc = 0; /* clear target tsc */
    rl_port->stat_mismatch_sync_point++;
    s->port_user_stats.stat_mismatch_sync_point++;
    rl_port->force_sync_first_tsc = false;
    /* dummy pkts to fill the rl burst buffer */
    tx_audio_session_rl_warmup_pkt(s, s_port, rl->pkts_prepare_warmup, 0);
    return tx_audio_session_rl_tx_pkt(s, s_port, pkt);
  }

  if (rl_port->force_sync_first_tsc) return 0;

  /* check if we are reaching the warmup stage */
  uint32_t delta_tsc = target_tsc - cur_tsc;
  uint32_t trs = s->pacing.trs;
  uint32_t delta_pkts = delta_tsc / trs;
  if (delta_pkts > rl->max_warmup_trs) {
    dbg("%s(%d,%d), delta_pkts %u too large\n", __func__, s->idx, s_port, delta_pkts);
    return 0;
  }
  uint32_t accuracy = delta_tsc % trs;
  if (accuracy > rl->required_accuracy_ns) {
    dbg("%s(%d,%d), accuracy %u too large, delta_tsc %u trs %u\n", __func__, s->idx,
        s_port, accuracy, delta_tsc, trs);
    return 0;
  }
  dbg("%s(%d,%d), accuracy %u succ\n", __func__, s->idx, s_port, accuracy);
  if (delta_pkts != rl->max_warmup_trs) {
    /* hit on backup check point */
    rl_port->stat_hit_backup_cp++;
    s->port_user_stats.stat_hit_backup_cp++;
  }

  /* sending the prepare pkts */
  tx_audio_session_rl_warmup_pkt(s, s_port, rl->pkts_prepare_warmup, 0);
  /* sending the warmup pkts */
  for (int i = delta_pkts; i > 0; i--) {
    tx_audio_session_rl_warmup_pkt(s, s_port, 0, 1);

    /* re-calculate the delta */
    uint32_t delta_tsc_now = target_tsc - mt_get_tsc(impl);
    uint32_t delta_pkts_now = delta_tsc_now / trs;
    if (delta_pkts_now < (i - 0)) {
      dbg("%s(%d), mismatch delta_pkts_now %d at %d\n", __func__, s->idx, delta_pkts_now,
          i);
      /* try next sync point */
      s->port_user_stats.stat_recalculate_warmup++;
      rl_port->stat_recalculate_warmup++;
      rl_port->force_sync_first_tsc = true;
      return 0;
    }
  }

  rl_port->trs_target_tsc = 0; /* clear target tsc */
  /* sending the first pkt now */
  return tx_audio_session_rl_tx_pkt(s, s_port, pkt);
}

static int tx_audio_session_tasklet_rl_transmit(struct mtl_main_impl* impl,
                                                struct st_tx_audio_session_impl* s,
                                                int s_port) {
  int ret;
  struct rte_mbuf* pkt;
  struct st_tx_audio_session_rl_info* rl = &s->rl;
  struct st_tx_audio_session_rl_port* rl_port = &rl->port_info[s_port];
  uint16_t tx;

  /* check if any pending pkt */
  pkt = s->trans_ring_inflight[s_port];
  if (pkt) {
    if (rl_port->trs_target_tsc) { /* waiting the first pkt */
      tx = tx_audio_session_rl_first_pkt(impl, s, s_port, pkt);
    } else {
      tx = tx_audio_session_rl_tx_pkt(s, s_port, pkt);
    }
    if (tx < 1) {
      s->stat_transmit_ret_code = -STI_RLTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
    s->trans_ring_inflight[s_port] = NULL;
  }

  /* check if any padding inflight pkts in transmitter */
  if (rl_port->trs_pad_inflight_num > 0) {
    int cur_queue = rl_port->cur_queue;
    struct mt_txq_entry* queue = rl_port->queue[cur_queue];
    struct rte_mbuf* pad = rl_port->pad;

    tx = mt_txq_burst(queue, &pad, 1);
    rl_port->trs_pad_inflight_num -= tx;
    if (tx < 1) {
      s->stat_transmit_ret_code = -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL;
    }
    if (rl_port->trs_pad_inflight_num == 0) { /* all done for current pkt */
      tx_audio_session_rl_inc_pkt_idx(rl, rl_port);
    }
    return MTL_TASKLET_HAS_PENDING;
  }

  /* try to dequeue pkt */
  ret = mt_u64_fifo_get(s->trans_ring[s_port], (uint64_t*)&pkt);
  if (ret < 0) {
    s->stat_transmit_ret_code = -STI_RLTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE; /* no pkt */
  }

  if (0 == rl_port->cur_pkt_idx) {
    /* the first pkt, start warmup */
    rl_port->trs_target_tsc = st_tx_mbuf_get_tsc(pkt);
    tx = tx_audio_session_rl_first_pkt(impl, s, s_port, pkt);
  } else {
    tx = tx_audio_session_rl_tx_pkt(s, s_port, pkt);
  }
  if (tx < 1) {
    s->trans_ring_inflight[s_port] = pkt;
    s->stat_transmit_ret_code = -STI_RLTRS_BURST_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  return 0;
}

static int tx_audio_sessions_tasklet(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_audio_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_audio_session_try_get(mgr, sidx);
    if (!s) continue;
    if (!s->active) goto exit;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    s->stat_build_ret_code = 0;
    if (s->ops.type == ST30_TYPE_FRAME_LEVEL)
      pending += tx_audio_session_tasklet_frame(impl, s);
    else
      pending += tx_audio_session_tasklet_rtp(impl, s);

    for (int port = 0; port < s->ops.num_port; port++) {
      if (s->tx_pacing_way == ST30_TX_PACING_WAY_RL)
        pending += tx_audio_session_tasklet_rl_transmit(impl, s, port);
      else
        pending += tx_audio_session_tasklet_transmit(impl, mgr, s, port);
    }

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
  exit:
    tx_audio_session_put(mgr, sidx);
  }

  return pending;
}

static int tx_audio_sessions_mgr_uinit_hw(struct st_tx_audio_sessions_mgr* mgr,
                                          enum mtl_port port) {
  if (mgr->ring[port]) {
    rte_ring_free(mgr->ring[port]);
    mgr->ring[port] = NULL;
  }
  if (mgr->queue[port]) {
    struct rte_mbuf* pad = mt_get_pad(mgr->parent, port);
    /* free completed mbufs from NIC tx ring before flushing */
    mt_txq_done_cleanup(mgr->queue[port]);
    /* flush all the pkts in the tx ring desc */
    if (pad) mt_txq_flush(mgr->queue[port], pad);
    /* clean any remaining mbufs after flush */
    mt_txq_done_cleanup(mgr->queue[port]);
    mt_txq_put(mgr->queue[port]);
    mgr->queue[port] = NULL;
  }

  dbg("%s(%d,%d), succ\n", __func__, mgr->idx, port);
  return 0;
}

static int tx_audio_sessions_mgr_init_hw(struct mtl_main_impl* impl,
                                         struct st_tx_audio_sessions_mgr* mgr,
                                         enum mtl_port port) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx;

  if (mgr->queue[port]) return 0; /* init already */

  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  mgr->queue[port] = mt_txq_get(impl, port, &flow);
  if (!mgr->queue[port]) {
    return -EIO;
  }

  snprintf(ring_name, 32, "%sM%dP%d", ST_TX_AUDIO_PREFIX, mgr_idx, port);
  flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
  count = ST_TX_AUDIO_SESSIONS_RING_SIZE;
  ring = rte_ring_create(ring_name, count, mgr->socket_id, flags);
  if (!ring) {
    err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, port);
    tx_audio_sessions_mgr_uinit_hw(mgr, port);
    return -ENOMEM;
  }
  mgr->ring[port] = ring;
  info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, port,
       mt_txq_queue_id(mgr->queue[port]));
  mgr->last_burst_succ_time_tsc[port] = mt_get_tsc(impl);

  return 0;
}

static int tx_audio_session_sq_flush_port(struct st_tx_audio_sessions_mgr* mgr,
                                          enum mtl_port port) {
  struct mtl_main_impl* impl = mgr->parent;
  int ret;
  int burst_pkts = mt_if_nb_tx_desc(impl, port);
  struct rte_mbuf* pad = mt_get_pad(impl, port);

  for (int i = 0; i < burst_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    int retry = 0;
    do {
      ret = rte_ring_mp_enqueue(mgr->ring[port], (void*)pad);
      if (ret != 0) {
        dbg("%s(%d), timeout at %d, ret %d\n", __func__, mgr->idx, i, ret);
        retry++;
        if (retry > 100) {
          err("%s(%d), timeout at %d\n", __func__, mgr->idx, i);
          return -EIO;
        }
        mt_sleep_ms(1);
      }
    } while (ret != 0);
  }

  return 0;
}

/* wa to flush the audio transmitter tx queue */
static int tx_audio_session_sq_flush(struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
  int mgr_idx = mgr->idx, s_idx = s->idx;

  if (!s->shared_queue) return 0; /* skip as not shared queue */

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];

    if (pool && rte_mempool_in_use_count(pool) &&
        rte_atomic32_read(&mgr->transmitter_started)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_audio_session_sq_flush_port(mgr, mt_port_logic2phy(s->port_maps, i));
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
  int retry;
  int max_retry = 5;

  if (s->mbuf_mempool_chain && !s->tx_mono_pool) {
    for (retry = 0; retry < max_retry; retry++) {
      ret = mt_mempool_free(s->mbuf_mempool_chain);
      if (ret >= 0) break;
      mt_sleep_ms(1);
    }
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i] && !s->tx_mono_pool) {
      for (retry = 0; retry < max_retry; retry++) {
        ret = mt_mempool_free(s->mbuf_mempool_hdr[i]);
        if (ret >= 0) break;
        mt_sleep_ms(1);
      }
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
    if (s->tx_mono_pool) {
      s->mbuf_mempool_hdr[i] = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx,
           s->mbuf_mempool_hdr[i], i);
    } else if (s->mbuf_mempool_hdr[i]) {
      warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
    } else {
      n = mt_if_nb_tx_desc(impl, port) + ST_TX_AUDIO_SESSIONS_RING_SIZE;
      if (ops->type == ST30_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%dP%d_HDR_%d", ST_TX_AUDIO_PREFIX, mgr->idx, idx, i,
               s->recovery_idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
          hdr_room_size, s->socket_id);
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
      s->mbuf_mempool_chain = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx,
           s->mbuf_mempool_chain);
    } else if (s->mbuf_mempool_chain) {
      warn("%s(%d), use previous chain mempool\n", __func__, idx);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%d_CHAIN_%d", ST_TX_AUDIO_PREFIX, mgr->idx, idx,
               s->recovery_idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, 0, chain_room_size, s->socket_id);
      if (!mbuf_pool) {
        tx_audio_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_chain = mbuf_pool;
    }
  }

  return 0;
}

static int tx_audio_session_init_rtp(struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_TX_AUDIO_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
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
      mt_fifo_mbuf_clean(s->trans_ring[port]);
      mt_u64_fifo_uinit(s->trans_ring[port]);
      s->trans_ring[port] = NULL;
    }
  }

  return 0;
}

static int tx_audio_session_init_trans_ring(struct st_tx_audio_sessions_mgr* mgr,
                                            struct st_tx_audio_session_impl* s) {
  struct mt_u64_fifo* ring;
  unsigned int count = ST_TX_AUDIO_SESSIONS_RING_SIZE;
  int mgr_idx = mgr->idx, idx = s->idx;
  int num_port = s->ops.num_port;
  uint16_t trans_ring_thresh = s->ops.fifo_size;

  /* make sure the ring is smaller than max_onward_epochs */
  while (count > s->pacing.max_onward_epochs) {
    count /= 2;
  }

  for (int port = 0; port < num_port; port++) {
    ring = mt_u64_fifo_init(count, s->socket_id);
    if (!ring) {
      err("%s(%d,%d), mt_u64_fifo_init fail\n", __func__, mgr_idx, idx);
      tx_audio_session_uinit_trans_ring(s);
      return -ENOMEM;
    }
    s->trans_ring[port] = ring;
  }

  if (!trans_ring_thresh) {
    trans_ring_thresh =
        (double)(ST30_TX_FIFO_DEFAULT_TIME_MS * NS_PER_MS) / s->pacing.trs;
    trans_ring_thresh = RTE_MAX(trans_ring_thresh, 2); /* min: 2 frame */
  }
  s->trans_ring_thresh = trans_ring_thresh;

  info("%s(%d,%d), trans_ring_thresh %u fifo %u\n", __func__, mgr_idx, idx,
       trans_ring_thresh, count);
  return 0;
}

static int tx_audio_session_uinit_queue(struct mtl_main_impl* impl,
                                        struct st_tx_audio_session_impl* s) {
  MTL_MAY_UNUSED(impl);

  for (int i = 0; i < s->ops.num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);

    if (s->queue[i]) {
      mt_txq_done_cleanup(s->queue[i]);
      mt_txq_flush(s->queue[i], mt_get_pad(impl, port));
      mt_txq_done_cleanup(s->queue[i]);
      mt_txq_put(s->queue[i]);
      s->queue[i] = NULL;
    }
  }
  return 0;
}

static int tx_audio_session_init_queue(struct mtl_main_impl* impl,
                                       struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  enum mtl_port port;
  uint16_t queue_id;

  for (int i = 0; i < s->ops.num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    struct mt_txq_flow flow;
    memset(&flow, 0, sizeof(flow));
    mtl_memcpy(&flow.dip_addr, &s->ops.dip_addr[i], MTL_IP_ADDR_LEN);
    flow.dst_port = s->ops.udp_port[i];
    flow.gso_sz = s->st30_pkt_size - sizeof(struct mt_udp_hdr);

    s->queue[i] = mt_txq_get(impl, port, &flow);
    if (!s->queue[i]) {
      tx_audio_session_uinit_queue(impl, s);
      return -EIO;
    }
    queue_id = mt_txq_queue_id(s->queue[i]);
    info("%s(%d), port(l:%d,p:%d), queue %d\n", __func__, idx, i, port, queue_id);
  }

  return 0;
}

static int tx_audio_session_uinit_sw(struct st_tx_audio_sessions_mgr* mgr,
                                     struct st_tx_audio_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;

  for (int port = 0; port < num_port; port++) {
    if (s->inflight[port]) {
      info("%s(%d), free inflight buf for port %d\n", __func__, idx, port);
      rte_pktmbuf_free(s->inflight[port]);
      s->inflight[port] = NULL;
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

  tx_audio_session_sq_flush(mgr, s);
  tx_audio_session_mempool_free(s);

  tx_audio_session_free_frames(s);
  tx_audio_session_usdt_dump_close(s);

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

  ret = tx_audio_session_init_trans_ring(mgr, s);
  if (ret < 0) {
    err("%s(%d), mbuf ring init fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  if (ops->type == ST30_TYPE_RTP_LEVEL) {
    ret = tx_audio_session_init_rtp(mgr, s);
  } else {
    ret = tx_audio_session_alloc_frames(s);
  }
  if (ret < 0) {
    err("%s(%d), mode init fail %d\n", __func__, idx, ret);
    tx_audio_session_uinit_sw(mgr, s);
    return ret;
  }

  return 0;
}

static int tx_audio_session_uinit(struct st_tx_audio_sessions_mgr* mgr,
                                  struct st_tx_audio_session_impl* s) {
  tx_audio_session_uinit_rl(mgr->parent, s);
  tx_audio_session_uinit_queue(mgr->parent, s);
  tx_audio_session_uinit_sw(mgr, s);
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

  s->mgr = mgr;

  /* detect pacing */
  s->tx_pacing_way = ST30_TX_PACING_WAY_TSC;
  double pkt_time = st30_get_packet_time(ops->ptime);
  bool detect_rl = false;
  if ((ops->pacing_way == ST30_TX_PACING_WAY_AUTO) && (pkt_time < (NS_PER_MS / 2))) {
    info("%s(%d), try detect rl as pkt_time %fns\n", __func__, idx, pkt_time);
    detect_rl = true;
  }
  if ((ops->pacing_way == ST30_TX_PACING_WAY_RL) && (pkt_time < (NS_PER_MS * 2))) {
    detect_rl = true;
  }
  if (detect_rl) {
    bool cap_rl = true;
    /* check if all port support rl */
    for (int i = 0; i < num_port; i++) {
      enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
      enum st21_tx_pacing_way sys_pacing_way = mt_if(impl, port)->tx_pacing_way;
      if (sys_pacing_way != ST21_TX_PACING_WAY_RL) {
        if (ops->pacing_way == ST30_TX_PACING_WAY_AUTO) {
          info("%s(%d,%d), the port sys pacing way %d not capable to RL\n", __func__, idx,
               port, sys_pacing_way);
          cap_rl = false;
          break;
        } else {
          err("%s(%d,%d), the port sys pacing way %d not capable to RL\n", __func__, idx,
              port, sys_pacing_way);
          return -ENOTSUP;
        }
      }
    }
    if (cap_rl) {
      info("%s(%d), select rl based pacing for pkt_time %fns\n", __func__, idx, pkt_time);
      s->tx_pacing_way = ST30_TX_PACING_WAY_RL;
    }
  }

  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "RX_AUDIO_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;

  /* if disable shared queue */
  s->shared_queue = true;
  if (s->tx_pacing_way == ST30_TX_PACING_WAY_RL) s->shared_queue = false;
  if (ops->flags & ST30_TX_FLAG_DEDICATE_QUEUE) s->shared_queue = false;

  for (int i = 0; i < num_port; i++) {
    s->st30_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10100 + idx * 2);
    if (mt_user_random_src_port(impl))
      s->st30_src_port[i] = mt_random_port(s->st30_dst_port[i]);
    else
      s->st30_src_port[i] =
          (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st30_dst_port[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = mt_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = mt_if_has_multi_seg(impl, port);

    if (s->shared_queue) {
      ret = tx_audio_sessions_mgr_init_hw(impl, mgr, port);
      if (ret < 0) {
        err("%s(%d), mgr init hw fail for port %d\n", __func__, idx, port);
        return -EIO;
      }
    }
  }
  s->tx_mono_pool = mt_user_tx_mono_pool(impl);
  /* manually disable chain or any port can't support chain */
  s->tx_no_chain = mt_user_tx_no_chain(impl) || !tx_audio_session_has_chain_buf(s);

  s->st30_frames_cnt = ops->framebuff_cnt;

  ret = st30_get_sample_size(ops->fmt);
  if (ret < 0) return ret;
  s->sample_size = ret;
  ret = st30_get_sample_num(ops->ptime, ops->sampling);
  if (ret < 0) return ret;
  s->sample_num = ret;

  ret = st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  if (ret < 0) return ret;
  s->pkt_len = ret;

  /* calculate pkts in line*/
  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr);

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
  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = mt_get_monotonic_time();
  mt_stat_u64_init(&s->stat_time);
  mt_stat_u64_init(&s->stat_tx_delta);
  s->usdt_dump_fd = -1;

  s->st30_rtp_time_app = 0xFFFFFFFF;
  s->st30_rtp_time = 0xFFFFFFFF;

  for (int i = 0; i < num_port; i++) {
    s->inflight[i] = NULL;
    s->inflight_cnt[i] = 0;
  }
  if (ops->flags & ST30_TX_FLAG_BUILD_PACING) s->pacing_in_build = true;
  s->calculate_time_cursor = true;
  ret = tx_audio_session_init_pacing(s);
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
    tx_audio_session_uinit(mgr, s);
    return ret;
  }

  if (s->tx_pacing_way == ST30_TX_PACING_WAY_RL) {
    ret = tx_audio_session_init_rl(impl, s);
    if (ret < 0) {
      err("%s(%d), init rl fail %d\n", __func__, idx, ret);
      tx_audio_session_uinit(mgr, s);
      return ret;
    }
  } else if (!s->shared_queue) {
    ret = tx_audio_session_init_queue(impl, s);
    if (ret < 0) {
      err("%s(%d), init dedicated queue fail %d\n", __func__, idx, ret);
      tx_audio_session_uinit(mgr, s);
      return ret;
    }
  } else {
    rte_atomic32_inc(&mgr->transmitter_clients);
  }

  s->frames_per_sec = (double)NS_PER_S / s->pacing.trs / s->st30_total_pkts;
  s->active = true;

  info("%s(%d), fmt %d channel %u sampling %d ptime %d pt %u\n", __func__, idx, ops->fmt,
       ops->channel, ops->sampling, ops->ptime, ops->payload_type);
  info("%s(%d), pkt_len %u frame_size %u frames %u fps %f, pacing_way %s\n", __func__,
       idx, s->pkt_len, s->st30_frame_size, s->st30_frames_cnt,
       (double)NS_PER_S / s->pacing.trs / s->st30_total_pkts,
       audio_pacing_way_name(s->tx_pacing_way));
  return 0;
}

static int tx_audio_session_update_dst(struct mtl_main_impl* impl,
                                       struct st_tx_audio_sessions_mgr* mgr,
                                       struct st_tx_audio_session_impl* s,
                                       struct st_tx_dest_info* dst) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st30_tx_ops* ops = &s->ops;

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->dip_addr[i], dst->dip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = dst->udp_port[i];
    s->st30_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx * 2);
    s->st30_src_port[i] =
        (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st30_dst_port[i];

    /* update hdr */
    ret = tx_audio_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  return 0;
}

static int tx_audio_sessions_mgr_update_dst(struct st_tx_audio_sessions_mgr* mgr,
                                            struct st_tx_audio_session_impl* s,
                                            struct st_tx_dest_info* dst) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = tx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = tx_audio_session_update_dst(mgr->parent, mgr, s, dst);
  tx_audio_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static void tx_audio_session_stat(struct st_tx_audio_sessions_mgr* mgr,
                                  struct st_tx_audio_session_impl* s) {
  int idx = s->idx;
  int m_idx = mgr->idx;
  int frame_cnt = rte_atomic32_read(&s->stat_frame_cnt);
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = cur_time_ns;

  notice("TX_AUDIO_SESSION(%d,%d:%s): fps %f frames %d, pkts %d:%d inflight %d:%d\n",
         m_idx, idx, s->ops_name, framerate, frame_cnt,
         s->stat_pkt_cnt[MTL_SESSION_PORT_P], s->stat_pkt_cnt[MTL_SESSION_PORT_R],
         s->inflight_cnt[MTL_SESSION_PORT_P], s->inflight_cnt[MTL_SESSION_PORT_R]);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P] = 0;
  s->stat_pkt_cnt[MTL_SESSION_PORT_R] = 0;

  if (s->stat_epoch_mismatch) {
    notice("TX_AUDIO_SESSION(%d,%d): epoch mismatch %u\n", m_idx, idx,
           s->stat_epoch_mismatch);
    s->stat_epoch_mismatch = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_AUDIO_SESSION(%d,%d): epoch drop %u\n", m_idx, idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_epoch_onward) {
    notice("TX_AUDIO_SESSION(%d,%d): epoch onward %u\n", m_idx, idx,
           s->stat_epoch_onward);
    s->stat_epoch_onward = 0;
  }
  if (s->stat_epoch_late) {
    notice("TX_AUDIO_SESSION(%d,%d): epoch late %u\n", m_idx, idx, s->stat_epoch_late);
    s->stat_epoch_late = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_AUDIO_SESSION(%d,%d): build timeout frames %u\n", m_idx, idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (frame_cnt <= 0) {
    warn("TX_AUDIO_SESSION(%d,%d): build ret %d, transmit ret %d\n", m_idx, idx,
         s->stat_build_ret_code, s->stat_transmit_ret_code);
  }

  if (s->stat_error_user_timestamp) {
    notice("TX_AUDIO_SESSION(%d,%d): error user timestamp %u\n", m_idx, idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }
  if (s->stat_recoverable_error) {
    notice("TX_AUDIO_SESSION(%d,%d): recoverable_error %u \n", m_idx, idx,
           s->stat_recoverable_error);
    s->stat_recoverable_error = 0;
  }
  if (s->stat_unrecoverable_error) {
    err("TX_AUDIO_SESSION(%d,%d): unrecoverable_error %u \n", m_idx, idx,
        s->stat_unrecoverable_error);
    /* not reset unrecoverable_error */
  }
  if (s->tx_pacing_way == ST30_TX_PACING_WAY_RL) {
    struct st_tx_audio_session_rl_port* rl_port = &s->rl.port_info[0];
    notice("TX_AUDIO_SESSION(%d,%d): rl pkts %u pads %u warmup %u\n", m_idx, idx,
           rl_port->stat_pkts_burst, rl_port->stat_pad_pkts_burst,
           rl_port->stat_warmup_pkts_burst);
    rl_port->stat_pkts_burst = 0;
    rl_port->stat_pad_pkts_burst = 0;
    rl_port->stat_warmup_pkts_burst = 0;
    if (rl_port->stat_mismatch_sync_point) {
      warn("TX_AUDIO_SESSION(%d,%d): mismatch sync point %u\n", m_idx, idx,
           rl_port->stat_mismatch_sync_point);
      rl_port->stat_mismatch_sync_point = 0;
    }
    if (rl_port->stat_recalculate_warmup) {
      warn("TX_AUDIO_SESSION(%d,%d): recalculate warmup %u\n", m_idx, idx,
           rl_port->stat_recalculate_warmup);
      rl_port->stat_recalculate_warmup = 0;
    }
    if (rl_port->stat_hit_backup_cp) {
      notice("TX_AUDIO_SESSION(%d,%d): hit backup warmup checkpoint %u\n", m_idx, idx,
             rl_port->stat_hit_backup_cp);
      rl_port->stat_hit_backup_cp = 0;
    }
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("TX_AUDIO_SESSION(%d,%d): tasklet time avg %.2fus max %.2fus min %.2fus\n",
           m_idx, idx, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  struct mt_stat_u64* stat_tx_delta = &s->stat_tx_delta;
  if (stat_tx_delta->cnt) {
    uint64_t avg_ns = stat_tx_delta->sum / stat_tx_delta->cnt;
    notice("TX_AUDIO_SESSION(%d,%d): tx delta avg %.2fus max %.2fus min %.2fus\n", m_idx,
           idx, (float)avg_ns / NS_PER_US, (float)stat_tx_delta->max / NS_PER_US,
           (float)stat_tx_delta->min / NS_PER_US);
    mt_stat_u64_init(stat_tx_delta);
  }
  if (s->stat_max_next_frame_us > 8 || s->stat_max_notify_frame_us > 8) {
    notice("TX_AUDIO_SESSION(%d,%d): get next frame max %uus, notify done max %uus\n",
           m_idx, idx, s->stat_max_next_frame_us, s->stat_max_notify_frame_us);
  }
  s->stat_max_next_frame_us = 0;
  s->stat_max_notify_frame_us = 0;
}

static int tx_audio_session_detach(struct st_tx_audio_sessions_mgr* mgr,
                                   struct st_tx_audio_session_impl* s) {
  tx_audio_session_stat(mgr, s);
  tx_audio_session_uinit(mgr, s);
  if (s->shared_queue) {
    rte_atomic32_dec(&mgr->transmitter_clients);
  }
  return 0;
}

static int st_tx_audio_sessions_stat(void* priv) {
  struct st_tx_audio_sessions_mgr* mgr = priv;
  struct st_tx_audio_session_impl* s;
  int m_idx = mgr->idx;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_audio_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    tx_audio_session_stat(mgr, s);
    tx_audio_session_put(mgr, j);
  }
  if (mgr->stat_pkts_burst > 0) {
    notice("TX_AUDIO_MGR(%d), pkts burst %d\n", m_idx, mgr->stat_pkts_burst);
    mgr->stat_pkts_burst = 0;
  } else {
    int32_t clients = rte_atomic32_read(&mgr->transmitter_clients);
    if ((clients > 0) && (mgr->max_idx > 0)) {
      for (int i = 0; i < mt_num_ports(mgr->parent); i++) {
        warn("TX_AUDIO_MGR(%d): trs ret %d:%d\n", m_idx, i, mgr->stat_trs_ret_code[i]);
      }
    }
  }
  if (mgr->stat_recoverable_error) {
    notice("TX_AUDIO_MGR(%d): recoverable_error %u \n", m_idx,
           mgr->stat_recoverable_error);
    mgr->stat_recoverable_error = 0;
  }
  if (mgr->stat_unrecoverable_error) {
    err("TX_AUDIO_MGR(%d): unrecoverable_error %u \n", m_idx,
        mgr->stat_unrecoverable_error);
    /* not reset unrecoverable_error */
  }

  return 0;
}

static int tx_audio_sessions_mgr_init(struct mtl_main_impl* impl,
                                      struct mtl_sch_impl* sch,
                                      struct st_tx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;
  int i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc3550_audio_hdr) != 54);

  mgr->parent = impl;
  mgr->idx = idx;
  mgr->socket_id = mt_sch_socket_id(sch);
  mgr->tx_hang_detect_time_thresh = NS_PER_S;

  for (i = 0; i < ST_SCH_MAX_TX_AUDIO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_audio_sessions";
  ops.start = tx_audio_sessions_tasklet_start;
  ops.handler = tx_audio_sessions_tasklet;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), tasklet register fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_tx_audio_sessions_stat, mgr, "tx_audio");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_tx_audio_session_impl* tx_audio_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st30_tx_ops* ops) {
  struct st_tx_audio_sessions_mgr* mgr = &sch->tx_a_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_tx_audio_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_TX_AUDIO_SESSIONS; i++) {
    if (!tx_audio_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_audio_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = tx_audio_session_init(mgr, s, i);
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

  err("%s(%d), fail to find free slot\n", __func__, midx);
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

  for (int i = 0; i < ST_SCH_MAX_TX_AUDIO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

static int tx_audio_sessions_mgr_uinit(struct st_tx_audio_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_audio_session_impl* s;

  mt_stat_unregister(mgr->parent, st_tx_audio_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_TX_AUDIO_SESSIONS; i++) {
    s = tx_audio_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_audio_sessions_mgr_detach(mgr, s);
    tx_audio_session_put(mgr, i);
  }

  for (int i = 0; i < mt_num_ports(impl); i++) {
    tx_audio_sessions_mgr_uinit_hw(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int tx_audio_ops_check(struct st30_tx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip = NULL;

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
    if (!ops->framebuff_size) {
      err("%s, pls set framebuff_size\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST30_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
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

static int st_tx_audio_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->tx_a_init) return 0;

  /* create tx audio context */
  ret = tx_audio_sessions_mgr_init(impl, sch, &sch->tx_a_mgr);
  if (ret < 0) {
    err("%s, tx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_audio_transmitter_init(impl, sch, &sch->tx_a_mgr, &sch->a_trs);
  if (ret < 0) {
    tx_audio_sessions_mgr_uinit(&sch->tx_a_mgr);
    err("%s, st_audio_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  sch->tx_a_init = true;
  return 0;
}

int st_audio_queue_fatal_error(struct mtl_main_impl* impl,
                               struct st_tx_audio_sessions_mgr* mgr, enum mtl_port port) {
  int idx = mgr->idx;
  int ret;
  struct st_tx_audio_session_impl* s;

  if (!mgr->queue[port]) {
    err("%s(%d,%d), no queue\n", __func__, idx, port);
    return -EIO;
  }

  /* clean mbuf in the ring as we will free the mempool then */
  if (mgr->ring[port]) mt_ring_dequeue_clean(mgr->ring[port]);
  /* clean the queue done mbuf */
  mt_txq_done_cleanup(mgr->queue[port]);

  mt_txq_fatal_error(mgr->queue[port]);
  mt_txq_put(mgr->queue[port]);
  mgr->queue[port] = NULL;

  /* init all session mempool again as we don't know which session has the bad pkt */
  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_audio_session_get(mgr, sidx);
    if (!s) continue;

    /* clear all tx ring buffer */
    if (s->packet_ring) mt_ring_dequeue_clean(s->packet_ring);
    for (uint8_t i = 0; i < s->ops.num_port; i++) {
      if (s->trans_ring[i]) mt_fifo_mbuf_clean(s->trans_ring[i]);
    }

    s->recovery_idx++;
    tx_audio_session_mempool_free(s);
    ret = tx_audio_session_mempool_init(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d,%d), init mempool fail %d for session %d\n", __func__, idx, port, ret,
          sidx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_unrecoverable_error);
      s->active = false; /* mark current session to dead */
    } else {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_recoverable_error);
    }
    tx_audio_session_put(mgr, sidx);
  }

  /* now create new queue */
  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  mgr->queue[port] = mt_txq_get(impl, port, &flow);
  if (!mgr->queue[port]) {
    err("%s(%d,%d), get new txq fail\n", __func__, idx, port);
    mgr->stat_unrecoverable_error++;
    return -EIO;
  }
  uint16_t queue_id = mt_txq_queue_id(mgr->queue[port]);
  info("%s(%d,%d), new queue_id %u\n", __func__, idx, port, queue_id);
  mgr->stat_recoverable_error++;

  return 0;
}

int st_tx_audio_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->tx_a_init) return 0;

  /* free tx audio context */
  st_audio_transmitter_uinit(&sch->a_trs);
  tx_audio_sessions_mgr_uinit(&sch->tx_a_mgr);

  sch->tx_a_init = false;
  return 0;
}

st30_tx_handle st30_tx_create(mtl_handle mt, struct st30_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st_tx_audio_session_handle_impl* s_impl;
  struct st_tx_audio_session_impl* s;
  struct mtl_sch_impl* sch;
  int quota_mbs, ret;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST30_TX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST30_TX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  quota_mbs = impl->main_sch->data_quota_mbs_limit / impl->tx_audio_sessions_max_per_sch;
  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_a_mgr_mutex);
  ret = st_tx_audio_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->tx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_audio_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_a_mgr_mutex);
  s = tx_audio_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->tx_a_mgr_mutex);
  if (!s) {
    err("%s, tx_audio_sessions_mgr_attach fail\n", __func__);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_TX_AUDIO;
  s_impl->impl = s;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st30_tx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st30_tx_update_destination(st30_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct st_tx_audio_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) return ret;

  ret = tx_audio_sessions_mgr_update_dst(&sch->tx_a_mgr, s, dst);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st30_tx_free(st30_tx_handle handle) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct mtl_sch_impl* sch;
  struct mtl_main_impl* impl;
  struct st_tx_audio_session_impl* s;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->tx_a_mgr_mutex);
  ret = tx_audio_sessions_mgr_detach(&sch->tx_a_mgr, s);
  mt_pthread_mutex_unlock(&sch->tx_a_mgr_mutex);
  if (ret < 0) err("%s(%d, %d), mgr detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->tx_a_mgr_mutex);
  tx_audio_sessions_mgr_update(&sch->tx_a_mgr);
  mt_pthread_mutex_unlock(&sch->tx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_tx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
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

  struct rte_mempool* mp =
      s->tx_no_chain ? s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] : s->mbuf_mempool_chain;
  pkt = rte_pktmbuf_alloc(mp);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  size_t hdr_offset = s->tx_no_chain ? sizeof(struct mt_udp_hdr) : 0;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_offset);
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

  if (s->tx_no_chain) len += sizeof(struct mt_udp_hdr);

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}

int st30_tx_get_session_stats(st30_tx_handle handle, struct st30_tx_user_stats* stats) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_audio_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st30_tx_reset_session_stats(st30_tx_handle handle) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_audio_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}
