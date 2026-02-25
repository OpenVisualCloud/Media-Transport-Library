/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#include "st_tx_fastmetadata_session.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_stat.h"
#include "st_err.h"
#include "st_fastmetadata_transmitter.h"

/* call tx_fastmetadata_session_put always if get successfully */
static inline struct st_tx_fastmetadata_session_impl* tx_fastmetadata_session_get(
    struct st_tx_fastmetadata_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_fastmetadata_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_fastmetadata_session_put always if get successfully */
static inline struct st_tx_fastmetadata_session_impl* tx_fastmetadata_session_try_get(
    struct st_tx_fastmetadata_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_tx_fastmetadata_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_fastmetadata_session_put always if get successfully */
static inline struct st_tx_fastmetadata_session_impl* tx_fastmetadata_session_get_timeout(
    struct st_tx_fastmetadata_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
  struct st_tx_fastmetadata_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_fastmetadata_session_put always if get successfully */
static inline bool tx_fastmetadata_session_get_empty(
    struct st_tx_fastmetadata_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_fastmetadata_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void tx_fastmetadata_session_put(
    struct st_tx_fastmetadata_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static int tx_fastmetadata_session_free_frames(
    struct st_tx_fastmetadata_session_impl* s) {
  if (s->st41_frames) {
    struct st_frame_trans* frame;

    /* dec ref for current frame */
    frame = &s->st41_frames[s->st41_frame_idx];
    if (rte_atomic32_read(&frame->refcnt)) rte_atomic32_dec(&frame->refcnt);

    for (int i = 0; i < s->st41_frames_cnt; i++) {
      frame = &s->st41_frames[i];
      st_frame_trans_uinit(frame, NULL);
    }

    mt_rte_free(s->st41_frames);
    s->st41_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_fastmetadata_session_alloc_frames(
    struct st_tx_fastmetadata_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  if (s->st41_frames) {
    err("%s(%d), st41_frames already alloc\n", __func__, idx);
    return -EIO;
  }

  s->st41_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st41_frames) * s->st41_frames_cnt, soc_id);
  if (!s->st41_frames) {
    err("%s(%d), st30_frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st41_frames_cnt; i++) {
    frame_info = &s->st41_frames[i];
    rte_atomic32_set(&frame_info->refcnt, 0);
    frame_info->idx = i;
  }

  for (int i = 0; i < s->st41_frames_cnt; i++) {
    frame_info = &s->st41_frames[i];

    void* frame = mt_rte_zmalloc_socket(sizeof(struct st41_frame), soc_id);
    if (!frame) {
      err("%s(%d), frame malloc fail at %d\n", __func__, idx, i);
      tx_fastmetadata_session_free_frames(s);
      return -ENOMEM;
    }
    frame_info->iova = rte_mem_virt2iova(frame);
    frame_info->addr = frame;
    frame_info->flags = ST_FT_FLAG_RTE_MALLOC;
  }

  dbg("%s(%d), succ with %u frames\n", __func__, idx, s->st41_frames_cnt);
  return 0;
}

static int tx_fastmetadata_session_init_hdr(struct mtl_main_impl* impl,
                                            struct st_tx_fastmetadata_sessions_mgr* mgr,
                                            struct st_tx_fastmetadata_session_impl* s,
                                            enum mtl_session_port s_port) {
  MTL_MAY_UNUSED(mgr);
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct st41_tx_ops* ops = &s->ops;
  int ret;
  struct st41_fmd_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st41_rtp_hdr* rtp = &hdr->rtp;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = mt_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);

  /* ether hdr */
  if ((s_port == MTL_SESSION_PORT_P) && (ops->flags & ST41_TX_FLAG_USER_P_MAC)) {
    rte_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == MTL_SESSION_PORT_R) && (ops->flags & ST41_TX_FLAG_USER_R_MAC)) {
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
  ipv4->next_proto_id = IPPROTO_UDP;
  mtl_memcpy(&ipv4->src_addr, sip, MTL_IP_ADDR_LEN);
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st41_src_port[s_port]);
  udp->dst_port = htons(s->st41_dst_port[s_port]);
  udp->dgram_cksum = 0;

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = ST_RVRTP_VERSION_2;
  rtp->base.marker = 0;
  rtp->base.payload_type =
      ops->payload_type ? ops->payload_type : ST_RFMDRTP_PAYLOAD_TYPE_FASTMETADATA;
  uint32_t ssrc = ops->ssrc ? ops->ssrc : s->idx + 0x323450;
  rtp->base.ssrc = htonl(ssrc);
  s->st41_seq_id = 0;
  s->st41_rtp_time = -1;

  info("%s(%d,%d), ip %u.%u.%u.%u port %u:%u\n", __func__, idx, s_port, dip[0], dip[1],
       dip[2], dip[3], s->st41_src_port[s_port], s->st41_dst_port[s_port]);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx, ssrc %u\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5], ssrc);
  return 0;
}

static int tx_fastmetadata_session_init_pacing(
    struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx;
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  double frame_time = (double)1000000000.0 * s->fps_tm.den / s->fps_tm.mul;

  pacing->frame_time = frame_time;
  pacing->frame_time_sampling =
      (double)(s->fps_tm.sampling_clock_rate) * s->fps_tm.den / s->fps_tm.mul;
  pacing->max_onward_epochs = (double)(NS_PER_S * 1) / frame_time; /* 1s */
  dbg("%s[%02d], max_onward_epochs %u\n", __func__, idx, pacing->max_onward_epochs);

  info("%s[%02d], frame_time %f frame_time_sampling %f\n", __func__, idx,
       pacing->frame_time, pacing->frame_time_sampling);
  return 0;
}

static int tx_fastmetadata_session_init_pacing_epoch(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_session_impl* s) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  pacing->cur_epochs = ptp_time / pacing->frame_time;
  return 0;
}

static inline double tx_fastmetadata_pacing_time(
    struct st_tx_fastmetadata_session_pacing* pacing, uint64_t epochs) {
  return epochs * pacing->frame_time;
}

static inline uint32_t tx_fastmetadata_pacing_time_stamp(
    struct st_tx_fastmetadata_session_pacing* pacing, uint64_t epochs) {
  uint64_t tmstamp64 = epochs * pacing->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;

  return tmstamp32;
}

static uint64_t tx_fastmetadata_pacing_required_tai(
    struct st_tx_fastmetadata_session_impl* s, enum st10_timestamp_fmt tfmt,
    uint64_t timestamp) {
  uint64_t required_tai = 0;

  if (!(s->ops.flags & ST41_TX_FLAG_USER_PACING)) return 0;
  if (!timestamp) return 0;

  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    if (timestamp > 0xFFFFFFFF) {
      err("%s(%d), invalid timestamp %" PRIu64 "\n", __func__, s->idx, timestamp);
    }
    required_tai = st10_media_clk_to_ns((uint32_t)timestamp, 90 * 1000);
  } else {
    required_tai = timestamp;
  }

  return required_tai;
}

static int tx_fastmetadata_session_sync_pacing(struct mtl_main_impl* impl,
                                               struct st_tx_fastmetadata_session_impl* s,
                                               bool sync, uint64_t required_tai,
                                               bool second_field) {
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  double frame_time = pacing->frame_time;
  /* always use MTL_PORT_P for ptp now */
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t next_epochs = pacing->cur_epochs + 1;
  uint64_t epochs;
  double to_epoch;
  bool interlaced = s->ops.interlaced;

  if (required_tai) {
    uint64_t ptp_epochs = ptp_time / frame_time;
    epochs = (required_tai + frame_time / 2) / frame_time;
    dbg("%s(%d), required tai %" PRIu64 " ptp_epochs %" PRIu64 " epochs %" PRIu64 "\n",
        __func__, s->idx, required_tai, ptp_epochs, epochs);
    if (epochs < ptp_epochs) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
    }
  } else {
    epochs = ptp_time / frame_time;
  }

  dbg("%s(%d), epochs %" PRIu64 " %" PRIu64 "\n", __func__, s->idx, epochs,
      pacing->cur_epochs);
  if (epochs <= pacing->cur_epochs) {
    uint64_t diff = pacing->cur_epochs - epochs;
    if (diff < pacing->max_onward_epochs) {
      /* point to next epoch since if it in the range of onward */
      epochs = next_epochs;
    }
  }

  if (interlaced) {
    if (second_field) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
    } else {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
    }
  }

  to_epoch = tx_fastmetadata_pacing_time(pacing, epochs) - ptp_time;
  if (to_epoch < 0) {
    /* time bigger than the assigned epoch time */
    ST_SESSION_STAT_INC(s, port_user_stats, stat_epoch_mismatch);
    to_epoch = 0; /* send asap */
  }

  if (epochs > next_epochs) s->stat_epoch_drop += (epochs - next_epochs);
  if (epochs < next_epochs) {
    ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_onward,
                        (next_epochs - epochs));
  }

  pacing->cur_epochs = epochs;
  pacing->ptp_time_cursor = tx_fastmetadata_pacing_time(pacing, epochs);
  pacing->pacing_time_stamp = tx_fastmetadata_pacing_time_stamp(pacing, epochs);
  pacing->rtp_time_stamp = pacing->pacing_time_stamp;
  pacing->tsc_time_cursor = (double)mt_get_tsc(impl) + to_epoch;
  dbg("%s(%d), epochs %" PRIu64 " time_stamp %u time_cursor %f to_epoch %f\n", __func__,
      s->idx, pacing->cur_epochs, pacing->pacing_time_stamp, pacing->tsc_time_cursor,
      to_epoch);

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, s->idx,
        pacing->tsc_time_cursor, mt_get_tsc(impl));
    mt_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tx_fastmetadata_session_init_next_meta(
    struct st_tx_fastmetadata_session_impl* s, struct st41_tx_frame_meta* meta) {
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  struct st41_tx_ops* ops = &s->ops;

  memset(meta, 0, sizeof(*meta));
  meta->fps = ops->fps;
  if (ops->interlaced) { /* init second_field but user still can customize also */
    meta->second_field = s->second_field;
  }
  /* point to next epoch */
  meta->epoch = pacing->cur_epochs + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tx_fastmetadata_pacing_time(pacing, meta->epoch);
  return 0;
}

static int tx_fastmetadata_session_init(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                        struct st_tx_fastmetadata_session_impl* s,
                                        int idx) {
  MTL_MAY_UNUSED(mgr);
  s->idx = idx;
  return 0;
}

static int tx_fastmetadata_sessions_tasklet_start(void* priv) {
  struct st_tx_fastmetadata_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_fastmetadata_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_fastmetadata_session_get(mgr, sidx);
    if (!s) continue;

    tx_fastmetadata_session_init_pacing_epoch(impl, s);
    tx_fastmetadata_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_fastmetadata_session_update_redundant(
    struct st_tx_fastmetadata_session_impl* s, struct rte_mbuf* pkt_r) {
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

static void tx_fastmetadata_session_build_packet(
    struct st_tx_fastmetadata_session_impl* s, struct rte_mbuf* pkt) {
  struct st41_fmd_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st41_rtp_hdr* rtp;

  if (rte_pktmbuf_data_len(pkt) < sizeof(*hdr)) {
    err("%s: packet is less than fmd hdr size", __func__);
    return;
  }

  hdr = rte_pktmbuf_mtod(pkt, struct st41_fmd_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = &hdr->rtp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                  sizeof(struct rte_udp_hdr);

  rte_memcpy(rtp, &s->hdr[MTL_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->base.seq_number = htons(s->st41_seq_id);
  s->st41_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* Set place for payload just behind rtp header */
  struct st_frame_trans* frame_info = &s->st41_frames[s->st41_frame_idx];
  uint32_t offset = s->st41_pkt_idx * s->max_pkt_len;
  void* src_addr = frame_info->addr + offset;
  struct st41_frame* src = src_addr;
  uint16_t data_item_length_bytes = src->data_item_length_bytes;
  uint16_t data_item_length =
      (data_item_length_bytes + 3) / 4; /* expressed in number of 4-byte words */

  if (rte_pktmbuf_data_len(pkt) < sizeof(*hdr) + data_item_length_bytes) {
    err("%s: packet doesn't contain RTP payload", __func__);
    return;
  }

  uint8_t* payload = (uint8_t*)(rtp + 1);
  if (!(data_item_length_bytes > s->max_pkt_len)) {
    int offset = 0;
    for (int i = 0; i < data_item_length_bytes; i++) {
      payload[i] = src->data[offset++];
    }
    /* filling with 0's the remianing bytes of last 4-byte word */
    for (int i = data_item_length_bytes; i < data_item_length * 4; i++) {
      payload[i] = 0;
    }
  }

  pkt->data_len += sizeof(struct st41_rtp_hdr) + data_item_length * 4;
  pkt->pkt_len = pkt->data_len;
  rtp->st41_hdr_chunk.data_item_type = s->ops.fmd_dit;
  rtp->st41_hdr_chunk.data_item_k_bit = s->ops.fmd_k_bit;
  rtp->st41_hdr_chunk.data_item_length = data_item_length;
  rtp->swaped_st41_hdr_chunk = htonl(rtp->swaped_st41_hdr_chunk);
  dbg("%s(%d), payload_size (data_item_length_bytes) %d\n", __func__, s->idx,
      data_item_length_bytes);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return;
}

static void tx_fastmetadata_session_build_rtp_packet(
    struct st_tx_fastmetadata_session_impl* s, struct rte_mbuf* pkt) {
  struct st41_rtp_hdr* rtp;

  rtp = rte_pktmbuf_mtod(pkt, struct st41_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr[MTL_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  rtp->base.seq_number = htons(s->st41_seq_id);
  s->st41_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* Set place for payload just behind rtp header */
  uint8_t* payload = (uint8_t*)&rtp[1];
  struct st_frame_trans* frame_info = &s->st41_frames[s->st41_frame_idx];
  uint32_t offset = s->st41_pkt_idx * s->max_pkt_len;
  void* src_addr = frame_info->addr + offset;
  struct st41_frame* src = src_addr;
  uint16_t data_item_length_bytes = src->data_item_length_bytes;
  uint16_t data_item_length =
      (data_item_length_bytes + 3) / 4; /* expressed in number of 4-byte words */

  if (!(data_item_length_bytes > s->max_pkt_len)) {
    int offset = 0;
    for (int i = 0; i < data_item_length_bytes; i++) {
      payload[i] = src->data[offset++];
    }
    /* filling with 0's the remianing bytes of last 4-byte word */
    for (int i = data_item_length_bytes; i < data_item_length * 4; i++) {
      payload[i] = 0;
    }
  }

  pkt->data_len = sizeof(struct st41_rtp_hdr) + data_item_length * 4;
  pkt->pkt_len = pkt->data_len;
  rtp->st41_hdr_chunk.data_item_type = s->ops.fmd_dit;
  rtp->st41_hdr_chunk.data_item_k_bit = s->ops.fmd_k_bit;
  rtp->st41_hdr_chunk.data_item_length = data_item_length;
  rtp->swaped_st41_hdr_chunk = htonl(rtp->swaped_st41_hdr_chunk);

  dbg("%s(%d), payload_size (data_item_length_bytes) %d\n", __func__, s->idx,
      data_item_length_bytes);

  return;
}

static int tx_fastmetadata_session_rtp_update_packet(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_session_impl* s,
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

  if (rtp->tmstamp != s->st41_rtp_time) {
    /* start of a new frame */
    s->st41_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st41_rtp_time = rtp->tmstamp;
    bool second_field = false;

    tx_fastmetadata_session_sync_pacing(impl, s, false, 0, second_field);
  }
  if (s->ops.flags & ST41_TX_FLAG_USER_TIMESTAMP) {
    s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
  }
  rtp->tmstamp = htonl(s->pacing.rtp_time_stamp);

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

static int tx_fastmetadata_session_build_packet_chain(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_session_impl* s,
    struct rte_mbuf* pkt, struct rte_mbuf* pkt_rtp, enum mtl_session_port s_port) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st41_tx_ops* ops = &s->ops;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->hdr[s_port].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->hdr[s_port].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->hdr[s_port].udp, sizeof(hdr->udp));

  /* update only for primary */
  if (s_port == MTL_SESSION_PORT_P) {
    /* update rtp time for rtp path */
    if (ops->type == ST41_TYPE_RTP_LEVEL) {
      struct st41_rtp_hdr* rtp = rte_pktmbuf_mtod(pkt_rtp, struct st41_rtp_hdr*);
      if (rtp->base.tmstamp != s->st41_rtp_time) {
        /* start of a new frame */
        s->st41_pkt_idx = 0;
        rte_atomic32_inc(&s->stat_frame_cnt);
        s->port_user_stats.common.port[s_port].frames++;
        s->st41_rtp_time = rtp->base.tmstamp;
        bool second_field = false;
        tx_fastmetadata_session_sync_pacing(impl, s, false, 0, second_field);
      }
      if (s->ops.flags & ST41_TX_FLAG_USER_TIMESTAMP) {
        s->pacing.rtp_time_stamp = ntohl(rtp->base.tmstamp);
      }
      rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
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

static inline int tx_fastmetadata_session_send_pkt(
    struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s, enum mtl_session_port s_port,
    struct rte_mbuf* pkt) {
  int ret;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct rte_ring* ring = mgr->ring[port];

  if (s->queue[s_port]) {
    uint16_t tx = mt_txq_burst(s->queue[s_port], &pkt, 1);
    if (tx < 1)
      ret = -EIO;
    else
      ret = 0;
  } else {
    ret = rte_ring_mp_enqueue(ring, (void*)pkt);
  }
  return ret;
}

static int tx_fastmetadata_session_tasklet_frame(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx;
  struct st41_tx_ops* ops = &s->ops;
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  enum mtl_port port_p = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = mgr->ring[port_p];

  if (ring_p && rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (ops->num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P]) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_P,
                                           s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_R,
                                           s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_R] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (ST41_TX_STAT_WAIT_FRAME == s->st41_frame_stat) {
    uint16_t next_frame_idx;
    int data_item_length_bytes = 0;
    struct st41_tx_frame_meta meta;

    if (s->check_frame_done_time) {
      uint64_t frame_end_time = mt_get_tsc(impl);
      if (frame_end_time > pacing->tsc_time_cursor) {
        ST_SESSION_STAT_INC(s, port_user_stats.common, stat_exceed_frame_time);
        dbg("%s(%d), frame %" PRIu16 " build time out %f us\n", __func__, idx,
            s->st41_frame_idx, (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
      }
      s->check_frame_done_time = false;
    }

    tx_fastmetadata_session_init_next_meta(s, &meta);
    /* Query next frame buffer idx */
    uint64_t tsc_start = 0;
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
    struct st_frame_trans* frame = &s->st41_frames[next_frame_idx];
    int refcnt = rte_atomic32_read(&frame->refcnt);
    if (refcnt) {
      err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx, refcnt);
      s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
      return MTL_TASKLET_ALL_DONE;
    }
    rte_atomic32_inc(&frame->refcnt);
    frame->tf_meta = meta;
    s->st41_frame_idx = next_frame_idx;
    dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
    s->st41_frame_stat = ST41_TX_STAT_SENDING_PKTS;
    struct st41_frame* src = (struct st41_frame*)frame->addr;
    data_item_length_bytes += src->data_item_length_bytes;
    int total_size = data_item_length_bytes;
    s->st41_pkt_idx = 0;
    s->st41_total_pkts = total_size / s->max_pkt_len;
    if (total_size % s->max_pkt_len) s->st41_total_pkts++;
    /* how do we split if it need two or more pkts? */
    dbg("%s(%d), st41_total_pkts %d data_item_length_bytes %d src %p\n", __func__, idx,
        s->st41_total_pkts, data_item_length_bytes, src);
    if (s->st41_total_pkts > 1) {
      err("%s(%d), frame %u invalid st41_total_pkts %d\n", __func__, idx, next_frame_idx,
          s->st41_total_pkts);
      s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
      return MTL_TASKLET_ALL_DONE;
    }

    MT_USDT_ST41_TX_FRAME_NEXT(s->mgr->idx, s->idx, next_frame_idx, frame->addr, 0,
                               data_item_length_bytes);
  }

  /* sync pacing */
  if (s->calculate_time_cursor) {
    struct st_frame_trans* frame = &s->st41_frames[s->st41_frame_idx];
    /* user timestamp control if any */
    uint64_t required_tai = tx_fastmetadata_pacing_required_tai(s, frame->tf_meta.tfmt,
                                                                frame->tf_meta.timestamp);
    bool second_field = frame->tf_meta.second_field;
    tx_fastmetadata_session_sync_pacing(impl, s, false, required_tai, second_field);
    if (ops->flags & ST41_TX_FLAG_USER_TIMESTAMP &&
        (frame->ta_meta.tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK)) {
      pacing->rtp_time_stamp = (uint32_t)frame->tf_meta.timestamp;
    }
    frame->tf_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
    frame->tf_meta.timestamp = pacing->ptp_time_cursor;
    frame->tf_meta.rtp_timestamp = pacing->rtp_time_stamp;
    /* init to next field */
    if (ops->interlaced) {
      s->second_field = second_field ? false : true;
    }
    s->calculate_time_cursor = false; /* clear */
  }

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

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;

  pkt = rte_pktmbuf_alloc(hdr_pool_p);
  if (!pkt) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
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
    tx_fastmetadata_session_build_rtp_packet(s, pkt_rtp);
    tx_fastmetadata_session_build_packet_chain(impl, s, pkt, pkt_rtp, MTL_SESSION_PORT_P);

    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        return MTL_TASKLET_ALL_DONE;
      }
      tx_fastmetadata_session_build_packet_chain(impl, s, pkt_r, pkt_rtp,
                                                 MTL_SESSION_PORT_R);
    }
  } else {
    tx_fastmetadata_session_build_packet(s, pkt);
    if (send_r) {
      pkt_r = rte_pktmbuf_copy(pkt, hdr_pool_r, 0, UINT32_MAX);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_copy redundant fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
      tx_fastmetadata_session_update_redundant(s, pkt_r);
    }
  }

  st_tx_mbuf_set_idx(pkt, s->st41_pkt_idx);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P]++;
  if (send_r) {
    st_tx_mbuf_set_idx(pkt_r, s->st41_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
  }

  s->st41_pkt_idx++;
  pacing->tsc_time_cursor += pacing->frame_time;
  s->calculate_time_cursor = true;

  bool done = false;
  ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_P, pkt);
  if (ret != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_R, pkt_r);
    if (ret != 0) {
      s->inflight[MTL_SESSION_PORT_R] = pkt_r;
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      done = true;
      s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
    }
  }

  if (s->st41_pkt_idx >= s->st41_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st41_frame_idx);
    struct st_frame_trans* frame = &s->st41_frames[s->st41_frame_idx];
    struct st41_tx_frame_meta* tf_meta = &frame->tf_meta;
    uint64_t tsc_start = 0;
    bool time_measure = mt_sessions_time_measure(impl);
    if (time_measure) tsc_start = mt_get_tsc(impl);
    /* end of current frame */
    if (s->ops.notify_frame_done)
      ops->notify_frame_done(ops->priv, s->st41_frame_idx, tf_meta);
    if (time_measure) {
      uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
      s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
    }
    rte_atomic32_dec(&frame->refcnt);
    s->st41_frame_stat = ST41_TX_STAT_WAIT_FRAME;
    s->st41_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    pacing->tsc_time_cursor = 0;

    MT_USDT_ST41_TX_FRAME_DONE(s->mgr->idx, s->idx, s->st41_frame_idx,
                               tf_meta->rtp_timestamp);
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tx_fastmetadata_session_tasklet_rtp(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_fastmetadata_session_pacing* pacing = &s->pacing;
  bool send_r = false;
  enum mtl_port port_p = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_ring* ring_p = mgr->ring[port_p];

  if (ring_p && rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_RTP_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P]) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_P,
                                           s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_R,
                                           s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_R] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

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

  struct rte_mbuf* pkt = NULL;
  struct rte_mbuf* pkt_r = NULL;
  struct rte_mbuf* pkt_rtp = NULL;

  if (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt_rtp) != 0) {
    dbg("%s(%d), rtp pkts not ready %d\n", __func__, idx, ret);
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
    tx_fastmetadata_session_rtp_update_packet(impl, s, pkt);
  } else {
    tx_fastmetadata_session_build_packet_chain(impl, s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
  }
  st_tx_mbuf_set_idx(pkt, s->st41_pkt_idx);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P]++;

  if (send_r) {
    if (s->tx_no_chain) {
      pkt_r = rte_pktmbuf_copy(pkt, hdr_pool_r, 0, UINT32_MAX);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_copy fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
      tx_fastmetadata_session_update_redundant(s, pkt_r);
    } else {
      tx_fastmetadata_session_build_packet_chain(impl, s, pkt_r, pkt_rtp,
                                                 MTL_SESSION_PORT_R);
    }
    st_tx_mbuf_set_idx(pkt_r, s->st41_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
  }

  bool done = true;
  ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_P, pkt);
  if (ret != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    ret = tx_fastmetadata_session_send_pkt(mgr, s, MTL_SESSION_PORT_R, pkt_r);
    if (ret != 0) {
      s->inflight[MTL_SESSION_PORT_R] = pkt_r;
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      done = false;
      s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tx_fastmetadata_sessions_tasklet_handler(void* priv) {
  struct st_tx_fastmetadata_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_fastmetadata_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_fastmetadata_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    s->stat_build_ret_code = 0;
    if (s->ops.type == ST41_TYPE_FRAME_LEVEL)
      pending += tx_fastmetadata_session_tasklet_frame(impl, mgr, s);
    else
      pending += tx_fastmetadata_session_tasklet_rtp(impl, mgr, s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
    tx_fastmetadata_session_put(mgr, sidx);
  }

  return pending;
}

static int tx_fastmetadata_sessions_mgr_uinit_hw(
    struct st_tx_fastmetadata_sessions_mgr* mgr, enum mtl_port port) {
  if (mgr->ring[port]) {
    rte_ring_free(mgr->ring[port]);
    mgr->ring[port] = NULL;
  }
  if (mgr->queue[port]) {
    struct rte_mbuf* pad = mt_get_pad(mgr->parent, port);
    /* flush all the pkts in the tx ring desc */
    if (pad) mt_txq_flush(mgr->queue[port], pad);
    mt_txq_put(mgr->queue[port]);
    mgr->queue[port] = NULL;
  }

  dbg("%s(%d,%d), succ\n", __func__, mgr->idx, port);
  return 0;
}

static int tx_fastmetadata_sessions_mgr_init_hw(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_sessions_mgr* mgr,
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

  snprintf(ring_name, 32, "%sM%dP%d", ST_TX_FASTMETADATA_PREFIX, mgr_idx, port);
  flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
  count = ST_TX_FMD_SESSIONS_RING_SIZE;
  ring = rte_ring_create(ring_name, count, mgr->socket_id, flags);
  if (!ring) {
    err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, port);
    tx_fastmetadata_sessions_mgr_uinit_hw(mgr, port);
    return -ENOMEM;
  }
  mgr->ring[port] = ring;
  info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, port,
       mt_txq_queue_id(mgr->queue[port]));

  return 0;
}

static int tx_fastmetadata_session_sq_flush_port(
    struct st_tx_fastmetadata_sessions_mgr* mgr, enum mtl_port port) {
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

/* wa to flush the fastmetadata transmitter tx queue */
static int tx_fastmetadata_session_flush(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                         struct st_tx_fastmetadata_session_impl* s) {
  int mgr_idx = mgr->idx, s_idx = s->idx;

  if (!s->shared_queue) return 0; /* skip as not shared queue */

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];
    if (pool && rte_mempool_in_use_count(pool) &&
        rte_atomic32_read(&mgr->transmitter_started)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_fastmetadata_session_sq_flush_port(mgr, mt_port_logic2phy(s->port_maps, i));
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

int tx_fastmetadata_session_mempool_free(struct st_tx_fastmetadata_session_impl* s) {
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

static bool tx_fastmetadata_session_has_chain_buf(
    struct st_tx_fastmetadata_session_impl* s) {
  struct st41_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  for (int port = 0; port < num_ports; port++) {
    if (!s->eth_has_chain[port]) return false;
  }

  /* all ports capable chain */
  return true;
}

static int tx_fastmetadata_session_mempool_init(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s) {
  struct st41_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum mtl_port port;
  unsigned int n;

  uint16_t hdr_room_size = sizeof(struct mt_udp_hdr);
  uint16_t chain_room_size = ST_PKT_MAX_ETHER_BYTES - hdr_room_size;

  if (s->tx_no_chain) {
    hdr_room_size += chain_room_size; /* enlarge hdr to attach chain */
  }

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    if (s->tx_mono_pool) {
      s->mbuf_mempool_hdr[i] = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx,
           s->mbuf_mempool_hdr[i], i);
    } else if (s->mbuf_mempool_hdr[i]) {
      warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
    } else {
      n = mt_if_nb_tx_desc(impl, port) + ST_TX_FMD_SESSIONS_RING_SIZE;
      if (ops->type == ST41_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%dP%d_HDR", ST_TX_FASTMETADATA_PREFIX, mgr->idx, idx,
               i);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
          hdr_room_size, s->socket_id);
      if (!mbuf_pool) {
        tx_fastmetadata_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  /* allocate payload(chain) pool */
  if (!s->tx_no_chain) {
    port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    n = mt_if_nb_tx_desc(impl, port) + ST_TX_FMD_SESSIONS_RING_SIZE;
    if (ops->type == ST41_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;

    if (s->tx_mono_pool) {
      s->mbuf_mempool_chain = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx,
           s->mbuf_mempool_chain);
    } else if (s->mbuf_mempool_chain) {
      warn("%s(%d), use previous chain mempool\n", __func__, idx);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%d_CHAIN", ST_TX_FASTMETADATA_PREFIX, mgr->idx, idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
          chain_room_size, s->socket_id);
      if (!mbuf_pool) {
        tx_fastmetadata_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_chain = mbuf_pool;
    }
  }

  return 0;
}

static int tx_fastmetadata_session_init_rtp(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                            struct st_tx_fastmetadata_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_TX_FASTMETADATA_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    tx_fastmetadata_session_mempool_free(s);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
  return 0;
}

static int tx_fastmetadata_session_uinit_sw(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                            struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;

  for (int port = 0; port < num_port; port++) {
    if (s->inflight[port]) {
      info("%s(%d), free inflight buf for port %d\n", __func__, idx, port);
      rte_pktmbuf_free(s->inflight[port]);
      s->inflight[port] = NULL;
    }
  }

  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tx_fastmetadata_session_flush(mgr, s);
  tx_fastmetadata_session_mempool_free(s);

  tx_fastmetadata_session_free_frames(s);

  return 0;
}

static int tx_fastmetadata_session_init_sw(struct mtl_main_impl* impl,
                                           struct st_tx_fastmetadata_sessions_mgr* mgr,
                                           struct st_tx_fastmetadata_session_impl* s) {
  struct st41_tx_ops* ops = &s->ops;
  int idx = s->idx, ret;

  /* free the pool if any in previous session */
  tx_fastmetadata_session_mempool_free(s);
  ret = tx_fastmetadata_session_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_fastmetadata_session_uinit_sw(mgr, s);
    return ret;
  }

  if (ops->type == ST41_TYPE_RTP_LEVEL) {
    ret = tx_fastmetadata_session_init_rtp(mgr, s);
  } else {
    ret = tx_fastmetadata_session_alloc_frames(s);
  }
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_fastmetadata_session_uinit_sw(mgr, s);
    return ret;
  }

  return 0;
}

static int tx_fastmetadata_session_uinit_queue(
    struct mtl_main_impl* impl, struct st_tx_fastmetadata_session_impl* s) {
  MTL_MAY_UNUSED(impl);

  for (int i = 0; i < s->ops.num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);

    if (s->queue[i]) {
      mt_txq_flush(s->queue[i], mt_get_pad(impl, port));
      mt_txq_put(s->queue[i]);
      s->queue[i] = NULL;
    }
  }
  return 0;
}

static int tx_fastmetadata_session_init_queue(struct mtl_main_impl* impl,
                                              struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx;
  enum mtl_port port;
  uint16_t queue_id;

  for (int i = 0; i < s->ops.num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    struct mt_txq_flow flow;
    memset(&flow, 0, sizeof(flow));
    mtl_memcpy(&flow.dip_addr, &s->ops.dip_addr[i], MTL_IP_ADDR_LEN);
    flow.dst_port = s->ops.udp_port[i];
    flow.gso_sz = ST_PKT_MAX_ETHER_BYTES;

    s->queue[i] = mt_txq_get(impl, port, &flow);
    if (!s->queue[i]) {
      tx_fastmetadata_session_uinit_queue(impl, s);
      return -EIO;
    }
    queue_id = mt_txq_queue_id(s->queue[i]);
    info("%s(%d), port(l:%d,p:%d), queue %d\n", __func__, idx, i, port, queue_id);
  }

  return 0;
}

static int tx_fastmetadata_session_uinit(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                         struct st_tx_fastmetadata_session_impl* s) {
  tx_fastmetadata_session_uinit_queue(mgr->parent, s);
  tx_fastmetadata_session_uinit_sw(mgr, s);
  return 0;
}

static int tx_fastmetadata_session_attach(struct mtl_main_impl* impl,
                                          struct st_tx_fastmetadata_sessions_mgr* mgr,
                                          struct st_tx_fastmetadata_session_impl* s,
                                          struct st41_tx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  s->mgr = mgr;
  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "TX_FMD_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;

  /* if disable shared queue */
  s->shared_queue = true;
  if (ops->flags & ST41_TX_FLAG_DEDICATE_QUEUE) s->shared_queue = false;

  for (int i = 0; i < num_port; i++) {
    s->st41_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10200 + idx * 2);
    if (mt_user_random_src_port(impl))
      s->st41_src_port[i] = mt_random_port(s->st41_dst_port[i]);
    else
      s->st41_src_port[i] =
          (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st41_dst_port[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = mt_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = mt_if_has_multi_seg(impl, port);

    if (s->shared_queue) {
      ret = tx_fastmetadata_sessions_mgr_init_hw(impl, mgr, port);
      if (ret < 0) {
        err("%s(%d), mgr init hw fail for port %d\n", __func__, idx, port);
        return ret;
      }
    }
  }
  s->tx_mono_pool = mt_user_tx_mono_pool(impl);
  /* manually disable chain or any port can't support chain */
  s->tx_no_chain = mt_user_tx_no_chain(impl) || !tx_fastmetadata_session_has_chain_buf(s);
  s->max_pkt_len = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st41_fmd_hdr);

  s->st41_frames_cnt = ops->framebuff_cnt;

  s->st41_frame_stat = ST41_TX_STAT_WAIT_FRAME;
  s->st41_frame_idx = 0;
  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = mt_get_monotonic_time();
  mt_stat_u64_init(&s->stat_time);

  for (int i = 0; i < num_port; i++) {
    s->inflight[i] = NULL;
    s->inflight_cnt[i] = 0;
  }

  ret = st_get_fps_timing(ops->fps, &s->fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  s->calculate_time_cursor = true;
  ret = tx_fastmetadata_session_init_pacing(s);
  if (ret < 0) {
    err("%s(%d), init pacing fail %d\n", __func__, idx, ret);
    return ret;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tx_fastmetadata_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), port(%d) init hdr fail %d\n", __func__, idx, i, ret);
      return ret;
    }
  }

  ret = tx_fastmetadata_session_init_sw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), init sw fail %d\n", __func__, idx, ret);
    tx_fastmetadata_session_uinit(mgr, s);
    return ret;
  }

  if (!s->shared_queue) {
    ret = tx_fastmetadata_session_init_queue(impl, s);
    if (ret < 0) {
      err("%s(%d), init dedicated queue fail %d\n", __func__, idx, ret);
      tx_fastmetadata_session_uinit(mgr, s);
      return ret;
    }
  } else {
    rte_atomic32_inc(&mgr->transmitter_clients);
  }

  info("%s(%d), type %d flags 0x%x pt %u, %s\n", __func__, idx, ops->type, ops->flags,
       ops->payload_type, ops->interlaced ? "interlace" : "progressive");
  return 0;
}

static void tx_fastmetadata_session_stat(struct st_tx_fastmetadata_session_impl* s) {
  int idx = s->idx;
  int frame_cnt = rte_atomic32_read(&s->stat_frame_cnt);
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = cur_time_ns;

  notice("TX_FMD_SESSION(%d:%s): fps %f frames %d pkts %d:%d\n", idx, s->ops_name,
         framerate, frame_cnt, s->stat_pkt_cnt[MTL_SESSION_PORT_P],
         s->stat_pkt_cnt[MTL_SESSION_PORT_R]);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P] = 0;
  s->stat_pkt_cnt[MTL_SESSION_PORT_R] = 0;

  if (s->stat_epoch_mismatch) {
    notice("TX_FMD_SESSION(%d): st41 epoch mismatch %d\n", idx, s->stat_epoch_mismatch);
    s->stat_epoch_mismatch = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_FMD_SESSION(%d): epoch drop %u\n", idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_epoch_onward) {
    notice("TX_FMD_SESSION(%d): epoch onward %d\n", idx, s->stat_epoch_onward);
    s->stat_epoch_onward = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_AUDIO_SESSION(%d): build timeout frames %u\n", idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (frame_cnt <= 0) {
    warn("TX_FMD_SESSION(%d): build ret %d\n", idx, s->stat_build_ret_code);
  }
  if (s->ops.interlaced) {
    notice("TX_FMD_SESSION(%d): interlace first field %u second field %u\n", idx,
           s->stat_interlace_first_field, s->stat_interlace_second_field);
    s->stat_interlace_first_field = 0;
    s->stat_interlace_second_field = 0;
  }

  if (s->stat_error_user_timestamp) {
    notice("TX_FMD_SESSION(%d): error user timestamp %u\n", idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("TX_FMD_SESSION(%d): tasklet time avg %.2fus max %.2fus min %.2fus\n", idx,
           (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_next_frame_us > 8 || s->stat_max_notify_frame_us > 8) {
    notice("TX_FMD_SESSION(%d): get next frame max %uus, notify done max %uus\n", idx,
           s->stat_max_next_frame_us, s->stat_max_notify_frame_us);
  }
  s->stat_max_next_frame_us = 0;
  s->stat_max_notify_frame_us = 0;
}

static int tx_fastmetadata_session_detach(struct st_tx_fastmetadata_sessions_mgr* mgr,
                                          struct st_tx_fastmetadata_session_impl* s) {
  tx_fastmetadata_session_stat(s);
  tx_fastmetadata_session_uinit(mgr, s);
  if (s->shared_queue) {
    rte_atomic32_dec(&mgr->transmitter_clients);
  }
  return 0;
}

static int tx_fastmetadata_session_update_dst(struct mtl_main_impl* impl,
                                              struct st_tx_fastmetadata_sessions_mgr* mgr,
                                              struct st_tx_fastmetadata_session_impl* s,
                                              struct st_tx_dest_info* dest) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st41_tx_ops* ops = &s->ops;

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->dip_addr[i], dest->dip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = dest->udp_port[i];
    s->st41_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
    s->st41_src_port[i] =
        (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st41_dst_port[i];

    /* update hdr */
    ret = tx_fastmetadata_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  return 0;
}

static int tx_fastmetadata_sessions_mgr_update_dst(
    struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s, struct st_tx_dest_info* dest) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = tx_fastmetadata_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = tx_fastmetadata_session_update_dst(mgr->parent, mgr, s, dest);
  tx_fastmetadata_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int st_tx_fastmetadata_sessions_stat(void* priv) {
  struct st_tx_fastmetadata_sessions_mgr* mgr = priv;
  struct st_tx_fastmetadata_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_fastmetadata_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    tx_fastmetadata_session_stat(s);
    tx_fastmetadata_session_put(mgr, j);
  }
  if (mgr->stat_pkts_burst > 0) {
    notice("TX_FMD_MGR, pkts burst %d\n", mgr->stat_pkts_burst);
    mgr->stat_pkts_burst = 0;
  } else {
    int32_t clients = rte_atomic32_read(&mgr->transmitter_clients);
    if ((clients > 0) && (mgr->max_idx > 0)) {
      for (int i = 0; i < mt_num_ports(mgr->parent); i++) {
        warn("TX_FMD_MGR: trs ret %d:%d\n", i, mgr->stat_trs_ret_code[i]);
      }
    }
  }

  return 0;
}

static int tx_fastmetadata_sessions_mgr_init(
    struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
    struct st_tx_fastmetadata_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;
  int i;

  RTE_BUILD_BUG_ON(sizeof(struct st41_fmd_hdr) != 58);

  mgr->parent = impl;
  mgr->idx = idx;
  mgr->socket_id = mt_sch_socket_id(sch);

  for (i = 0; i < ST_MAX_TX_FMD_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_fastmetadata_sessions_mgr";
  ops.start = tx_fastmetadata_sessions_tasklet_start;
  ops.handler = tx_fastmetadata_sessions_tasklet_handler;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_tx_fastmetadata_sessions_stat, mgr, "tx_fmd");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_tx_fastmetadata_session_impl* tx_fastmetadata_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st41_tx_ops* ops) {
  struct st_tx_fastmetadata_sessions_mgr* mgr = &sch->tx_fmd_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_tx_fastmetadata_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_TX_FMD_SESSIONS; i++) {
    if (!tx_fastmetadata_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_fastmetadata_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = tx_fastmetadata_session_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_fastmetadata_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = tx_fastmetadata_session_attach(mgr->parent, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_fastmetadata_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    tx_fastmetadata_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int tx_fastmetadata_sessions_mgr_detach(
    struct st_tx_fastmetadata_sessions_mgr* mgr,
    struct st_tx_fastmetadata_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_fastmetadata_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_fastmetadata_session_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  tx_fastmetadata_session_put(mgr, idx);

  return 0;
}

static int tx_fastmetadata_sessions_mgr_update(
    struct st_tx_fastmetadata_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_TX_FMD_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

static int tx_fastmetadata_sessions_mgr_uinit(
    struct st_tx_fastmetadata_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_fastmetadata_session_impl* s;

  mt_stat_unregister(mgr->parent, st_tx_fastmetadata_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_TX_FMD_SESSIONS; i++) {
    s = tx_fastmetadata_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_fastmetadata_sessions_mgr_detach(mgr, s);
    tx_fastmetadata_session_put(mgr, i);
  }

  for (int i = 0; i < mt_num_ports(impl); i++) {
    tx_fastmetadata_sessions_mgr_uinit_hw(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

/* Prune down ports that are not available. Shifts port names, destination IP addresses,
 * UDP ports, UDP source ports, and destination MAC addresses for remaining ports. */
static int tx_fastmetadata_ops_prune_down_ports(struct mtl_main_impl* impl,
                                                struct st41_tx_ops* ops) {
  int num_ports = ops->num_port;

  if (num_ports > MTL_SESSION_PORT_MAX || num_ports <= 0) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    enum mtl_port phy = mt_port_by_name(impl, ops->port[i]);
    if (phy >= MTL_PORT_MAX || !mt_if_port_is_down(impl, phy)) continue;

    warn("%s(%d), port %s is down, it will not be used\n", __func__, i, ops->port[i]);

    /* shift all further port-indexed fields one slot down */
    for (int j = i; j < num_ports - 1; j++) {
      rte_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      rte_memcpy(ops->dip_addr[j], ops->dip_addr[j + 1], MTL_IP_ADDR_LEN);
      rte_memcpy(ops->tx_dst_mac[j], ops->tx_dst_mac[j + 1], MTL_MAC_ADDR_LEN);
      ops->udp_port[j] = ops->udp_port[j + 1];
      ops->udp_src_port[j] = ops->udp_src_port[j + 1];
    }

    num_ports--;
    i--;
  }

  if (num_ports == 0) {
    err("%s, all %d port(s) are down, cannot create session\n", __func__, ops->num_port);
    return -EIO;
  }

  if (num_ports < ops->num_port) {
    info("%s, reduced num_port %d -> %d after pruning down ports\n", __func__,
         ops->num_port, num_ports);
    ops->num_port = num_ports;
  }

  return 0;
}

static int tx_fastmetadata_ops_check(struct st41_tx_ops* ops) {
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

  if (ops->type == ST41_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST41_TYPE_RTP_LEVEL) {
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

static int st_tx_fmd_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->tx_fmd_init) return 0;

  /* create tx fastmetadata context */
  ret = tx_fastmetadata_sessions_mgr_init(impl, sch, &sch->tx_fmd_mgr);
  if (ret < 0) {
    err("%s, tx_fastmetadata_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_fastmetadata_transmitter_init(impl, sch, &sch->tx_fmd_mgr, &sch->fmd_trs);
  if (ret < 0) {
    tx_fastmetadata_sessions_mgr_uinit(&sch->tx_fmd_mgr);
    err("%s, st_fastmetadata_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  sch->tx_fmd_init = true;
  return 0;
}

int st_tx_fastmetadata_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->tx_fmd_init) return 0;

  /* free tx fastmetadata context */
  st_fastmetadata_transmitter_uinit(&sch->fmd_trs);
  tx_fastmetadata_sessions_mgr_uinit(&sch->tx_fmd_mgr);

  sch->tx_fmd_init = false;
  return 0;
}

st41_tx_handle st41_tx_create(mtl_handle mt, struct st41_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st_tx_fastmetadata_session_handle_impl* s_impl;
  struct st_tx_fastmetadata_session_impl* s;
  struct mtl_sch_impl* sch;
  int quota_mbs, ret;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tx_fastmetadata_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, tx_fastmetadata_ops_prune_down_ports fail %d\n", __func__, ret);
    return NULL;
  }

  ret = tx_fastmetadata_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_fastmetadata_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  quota_mbs = 0;
  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_fmd_mgr_mutex);
  ret = st_tx_fmd_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->tx_fmd_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_fmd_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_fmd_mgr_mutex);
  s = tx_fastmetadata_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->tx_fmd_mgr_mutex);
  if (!s) {
    err("%s, tx_fastmetadata_sessions_mgr_attach fail\n", __func__);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_TX_FMD;
  s_impl->impl = s;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st41_tx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

void* st41_tx_get_mbuf(st41_tx_handle handle, void** usrptr) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = NULL;
  struct st_tx_fastmetadata_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_HANDLE_TX_FMD) {
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

int st41_tx_put_mbuf(st41_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_fastmetadata_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
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

int st41_tx_update_destination(st41_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;
  struct st_tx_fastmetadata_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) return ret;

  ret = tx_fastmetadata_sessions_mgr_update_dst(&sch->tx_fmd_mgr, s, dst);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st41_tx_free(st41_tx_handle handle) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;
  struct st_tx_fastmetadata_session_impl* s;
  struct mtl_sch_impl* sch;
  struct mtl_main_impl* impl;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->tx_fmd_mgr_mutex);
  ret = tx_fastmetadata_sessions_mgr_detach(&sch->tx_fmd_mgr, s);
  mt_pthread_mutex_unlock(&sch->tx_fmd_mgr_mutex);
  if (ret < 0) err("%s(%d), tx_fastmetadata_sessions_mgr_detach fail\n", __func__, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->tx_fmd_mgr_mutex);
  tx_fastmetadata_sessions_mgr_update(&sch->tx_fmd_mgr);
  mt_pthread_mutex_unlock(&sch->tx_fmd_mgr_mutex);

  rte_atomic32_dec(&impl->st41_tx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

void* st41_tx_get_framebuffer(st41_tx_handle handle, uint16_t idx) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;
  struct st_tx_fastmetadata_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  if (idx >= s->st41_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st41_frames_cnt);
    return NULL;
  }
  if (!s->st41_frames) {
    err("%s, st41_frames not allocated\n", __func__);
    return NULL;
  }

  struct st_frame_trans* frame_info = &s->st41_frames[idx];

  return frame_info->addr;
}

int st41_tx_get_session_stats(st41_tx_handle handle, struct st41_tx_user_stats* stats) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_fastmetadata_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st41_tx_reset_session_stats(st41_tx_handle handle) {
  struct st_tx_fastmetadata_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_FMD) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_fastmetadata_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}
