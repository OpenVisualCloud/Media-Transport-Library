/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_ancillary_session.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_stat.h"
#include "st_ancillary_transmitter.h"
#include "st_err.h"

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

/* call tx_ancillary_session_put always if get successfully */
static inline struct st_tx_ancillary_session_impl* tx_ancillary_session_get_timeout(
    struct st_tx_ancillary_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
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

static inline bool tx_ancillary_test_frame_active(
    struct st_tx_ancillary_session_impl* s) {
  return s->test.pattern != ST40_TX_TEST_NONE && s->test_frame_active;
}

static inline void tx_ancillary_seq_advance(struct st_tx_ancillary_session_impl* s,
                                            uint16_t step) {
  uint32_t seq = s->st40_seq_id;
  uint32_t ext = s->st40_ext_seq_id;

  seq += step;
  while (seq > UINT16_MAX) {
    seq -= (UINT16_MAX + 1);
    ext++;
  }

  s->st40_seq_id = seq;
  s->st40_ext_seq_id = ext;
}

static inline void tx_ancillary_set_rtp_seq(struct st_tx_ancillary_session_impl* s,
                                            struct st40_rfc8331_rtp_hdr* rtp) {
  uint16_t step = 1;

  if (tx_ancillary_test_frame_active(s) && s->test.pattern == ST40_TX_TEST_SEQ_GAP &&
      !s->test_seq_gap_fired) {
    step = 2;
    s->test_seq_gap_fired = true;
  }

  rtp->base.seq_number = htons(s->st40_seq_id);
  rtp->seq_number_ext = htons(s->st40_ext_seq_id);

  tx_ancillary_seq_advance(s, step);
}

static inline uint16_t tx_ancillary_apply_parity(struct st_tx_ancillary_session_impl* s,
                                                 uint16_t value) {
  if (tx_ancillary_test_frame_active(s) && s->test.pattern == ST40_TX_TEST_BAD_PARITY) {
    return value & 0x3FF; /* strip parity to intentionally corrupt */
  }
  return st40_add_parity_bits(value);
}

static int tx_ancillary_session_free_frames(struct st_tx_ancillary_session_impl* s) {
  if (s->st40_frames) {
    struct st_frame_trans* frame;

    /* dec ref for current frame */
    frame = &s->st40_frames[s->st40_frame_idx];
    if (rte_atomic32_read(&frame->refcnt)) rte_atomic32_dec(&frame->refcnt);

    for (int i = 0; i < s->st40_frames_cnt; i++) {
      frame = &s->st40_frames[i];
      st_frame_trans_uinit(frame, NULL);
    }

    mt_rte_free(s->st40_frames);
    s->st40_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_ancillary_session_alloc_frames(struct st_tx_ancillary_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  if (s->st40_frames) {
    err("%s(%d), st40_frames already alloc\n", __func__, idx);
    return -EIO;
  }

  s->st40_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st40_frames) * s->st40_frames_cnt, soc_id);
  if (!s->st40_frames) {
    err("%s(%d), st30_frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st40_frames_cnt; i++) {
    frame_info = &s->st40_frames[i];
    rte_atomic32_set(&frame_info->refcnt, 0);
    frame_info->idx = i;
  }

  for (int i = 0; i < s->st40_frames_cnt; i++) {
    frame_info = &s->st40_frames[i];

    void* frame = mt_rte_zmalloc_socket(sizeof(struct st40_frame), soc_id);
    if (!frame) {
      err("%s(%d), frame malloc fail at %d\n", __func__, idx, i);
      tx_ancillary_session_free_frames(s);
      return -ENOMEM;
    }
    frame_info->iova = rte_mem_virt2iova(frame);
    frame_info->addr = frame;
    frame_info->flags = ST_FT_FLAG_RTE_MALLOC;
  }

  dbg("%s(%d), succ with %u frames\n", __func__, idx, s->st40_frames_cnt);
  return 0;
}

static int tx_ancillary_session_init_hdr(struct mtl_main_impl* impl,
                                         struct st_tx_ancillary_sessions_mgr* mgr,
                                         struct st_tx_ancillary_session_impl* s,
                                         enum mtl_session_port s_port) {
  MTL_MAY_UNUSED(mgr);
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct st40_tx_ops* ops = &s->ops;
  int ret;
  struct st_rfc8331_anc_hdr* hdr = &s->hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st40_rfc8331_rtp_hdr* rtp = &hdr->rtp;

  /* Print rtp_header in both host and network byte order to check endianness */
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = mt_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);

  /* ether hdr */
  if ((s_port == MTL_SESSION_PORT_P) && (ops->flags & ST40_TX_FLAG_USER_P_MAC)) {
    rte_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == MTL_SESSION_PORT_R) && (ops->flags & ST40_TX_FLAG_USER_R_MAC)) {
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
  rtp->base.payload_type =
      ops->payload_type ? ops->payload_type : ST_RANCRTP_PAYLOAD_TYPE_ANCILLARY;
  uint32_t ssrc = ops->ssrc ? ops->ssrc : s->idx + 0x323450;
  rtp->base.ssrc = htonl(ssrc);
  s->st40_seq_id = 0;
  s->st40_ext_seq_id = 0;
  s->st40_rtp_time = -1;

  info("%s(%d,%d), ip %u.%u.%u.%u port %u:%u\n", __func__, idx, s_port, dip[0], dip[1],
       dip[2], dip[3], s->st40_src_port[s_port], s->st40_dst_port[s_port]);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx, ssrc %u\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5], ssrc);
  return 0;
}

static int tx_ancillary_session_init_pacing(struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
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

static int tx_ancillary_session_init_pacing_epoch(
    struct mtl_main_impl* impl, struct st_tx_ancillary_session_impl* s) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  pacing->cur_epochs = ptp_time / pacing->frame_time;
  return 0;
}

static inline uint64_t tx_ancillary_pacing_time(
    struct st_tx_ancillary_session_pacing* pacing, uint64_t epochs) {
  return nextafter(epochs * pacing->frame_time, INFINITY);
}

static inline __attribute__((unused)) uint32_t tx_ancillary_pacing_time_stamp(
    struct st_tx_ancillary_session_pacing* pacing, uint64_t epochs) {
  uint64_t tmstamp64 = epochs * pacing->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;

  return tmstamp32;
}

static uint64_t tx_ancillary_pacing_required_tai(struct st_tx_ancillary_session_impl* s,
                                                 enum st10_timestamp_fmt tfmt,
                                                 uint64_t timestamp) {
  uint64_t required_tai = 0;

  if (!(s->ops.flags & ST40_TX_FLAG_USER_PACING)) return 0;
  if (!timestamp) {
    if (s->ops.flags & ST40_TX_FLAG_EXACT_USER_PACING) {
      err("%s(%d), EXACT_USER_PACING requires non-zero timestamp\n", __func__, s->idx);
    }
    return 0;
  }

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

static void tx_ancillary_validate_user_timestamp(struct st_tx_ancillary_session_impl* s,
                                                 uint64_t requested_epoch,
                                                 uint64_t current_epoch) {
  if (requested_epoch < current_epoch) {
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
    dbg("%s(%d), user requested transmission time in the past, required_epoch %" PRIu64
        ", cur_epoch %" PRIu64 "\n",
        __func__, s->idx, requested_epoch, current_epoch);
  } else if (requested_epoch > current_epoch + (NS_PER_S / s->pacing.frame_time)) {
    dbg("%s(%d), requested epoch %" PRIu64
        " too far in the future, current epoch %" PRIu64 "\n",
        __func__, s->idx, requested_epoch, current_epoch);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
  }
}

static inline uint64_t tx_ancillary_calc_epoch(struct st_tx_ancillary_session_impl* s,
                                               uint64_t cur_tai, uint64_t required_tai) {
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  uint64_t current_epoch = cur_tai / pacing->frame_time;
  uint64_t next_free_epoch = pacing->cur_epochs + 1;
  uint64_t epoch = next_free_epoch;

  if (required_tai) {
    epoch = (required_tai + pacing->frame_time / 2) / pacing->frame_time;
    tx_ancillary_validate_user_timestamp(s, epoch, current_epoch);
  }

  if (current_epoch <= next_free_epoch) {
    if (next_free_epoch - current_epoch > pacing->max_onward_epochs) {
      dbg("%s(%d), onward range exceeded, next_free_epoch %" PRIu64
          ", current_epoch %" PRIu64 "\n",
          __func__, s->idx, next_free_epoch, current_epoch);
      ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_onward,
                          (next_free_epoch - current_epoch));
    }

    if (!required_tai) epoch = next_free_epoch;
  } else {
    dbg("%s(%d), frame is late, current_epoch %" PRIu64 " next_free_epoch %" PRIu64 "\n",
        __func__, s->idx, current_epoch, next_free_epoch);
    ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_drop,
                        (current_epoch - next_free_epoch));

    if (s->ops.notify_frame_late) {
      s->ops.notify_frame_late(s->ops.priv, current_epoch - next_free_epoch);
    }

    epoch = current_epoch;
  }

  return epoch;
}

static int tx_ancillary_session_sync_pacing(struct mtl_main_impl* impl,
                                            struct st_tx_ancillary_session_impl* s,
                                            uint64_t required_tai) {
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  uint64_t cur_tai = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t cur_tsc = mt_get_tsc(impl);
  uint64_t start_time_tai;
  int64_t time_to_tx_ns;

  pacing->cur_epochs = tx_ancillary_calc_epoch(s, cur_tai, required_tai);

  if ((s->ops.flags & ST40_TX_FLAG_EXACT_USER_PACING) && required_tai) {
    start_time_tai = required_tai;
  } else {
    start_time_tai = tx_ancillary_pacing_time(pacing, pacing->cur_epochs);
  }
  time_to_tx_ns = (int64_t)start_time_tai - (int64_t)cur_tai;
  if (time_to_tx_ns < 0) {
    /* time bigger than the assigned epoch time */
    ST_SESSION_STAT_INC(s, port_user_stats, stat_epoch_mismatch);
    time_to_tx_ns = 0; /* send asap */
  }

  pacing->ptp_time_cursor = start_time_tai;
  pacing->tsc_time_cursor = (double)cur_tsc + (double)time_to_tx_ns;
  dbg("%s(%d), epochs %" PRIu64 " ptp_time_cursor %" PRIu64 " time_to_tx_ns %" PRId64
      "\n",
      __func__, s->idx, pacing->cur_epochs, pacing->ptp_time_cursor, time_to_tx_ns);

  return 0;
}

static void tx_ancillary_update_rtp_time_stamp(struct st_tx_ancillary_session_impl* s,
                                               enum st10_timestamp_fmt tfmt,
                                               uint64_t timestamp) {
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;

  if (s->ops.flags & ST40_TX_FLAG_USER_TIMESTAMP) {
    pacing->rtp_time_stamp =
        st10_get_media_clk(tfmt, timestamp, s->fps_tm.sampling_clock_rate);
  } else {
    pacing->rtp_time_stamp =
        st10_tai_to_media_clk(pacing->ptp_time_cursor, s->fps_tm.sampling_clock_rate);
  }
}

static int tx_ancillary_session_init_next_meta(struct st_tx_ancillary_session_impl* s,
                                               struct st40_tx_frame_meta* meta) {
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
  struct st40_tx_ops* ops = &s->ops;

  memset(meta, 0, sizeof(*meta));
  meta->fps = ops->fps;
  if (ops->interlaced) { /* init second_field but user still can customize also */
    meta->second_field = s->second_field;
  }
  /* point to next epoch */
  meta->epoch = pacing->cur_epochs + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tx_ancillary_pacing_time(pacing, meta->epoch);
  return 0;
}

static int tx_ancillary_session_init(struct st_tx_ancillary_sessions_mgr* mgr,
                                     struct st_tx_ancillary_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
  s->idx = idx;
  return 0;
}

static int tx_ancillary_sessions_tasklet_start(void* priv) {
  struct st_tx_ancillary_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_ancillary_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_ancillary_session_get(mgr, sidx);
    if (!s) continue;

    tx_ancillary_session_init_pacing_epoch(impl, s);
    tx_ancillary_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_ancillary_session_update_redundant(struct st_tx_ancillary_session_impl* s,
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

static int tx_ancillary_session_build_packet(struct st_tx_ancillary_session_impl* s,
                                             struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st40_rfc8331_rtp_hdr* rtp;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = (struct st40_rfc8331_rtp_hdr*)&udp[1];

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
  tx_ancillary_set_rtp_seq(s, rtp);
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  bool test_no_marker =
      tx_ancillary_test_frame_active(s) && s->test.pattern == ST40_TX_TEST_NO_MARKER;

  /* Set place for payload just behind rtp header */
  uint8_t* payload = (uint8_t*)&rtp[1];
  struct st_frame_trans* frame_info = &s->st40_frames[s->st40_frame_idx];
  struct st40_frame* src = frame_info->addr;
  int anc_idx = s->st40_anc_idx;
  int anc_count = src->meta_num;
  if (tx_ancillary_test_frame_active(s) && s->split_payload && anc_count > 0 &&
      anc_idx >= anc_count)
    anc_idx = anc_count - 1; /* repeat last ANC when test demands extra packets */
  int total_udw = 0;
  int idx = 0;
  for (idx = anc_idx; idx < anc_count; idx++) {
    uint16_t udw_size = src->meta[idx].udw_size;
    total_udw += udw_size;
    if (!s->split_payload && (total_udw * 10 / 8) > s->max_pkt_len) break;
    struct st40_rfc8331_payload_hdr* pktBuff =
        (struct st40_rfc8331_payload_hdr*)(payload);
    pktBuff->first_hdr_chunk.c = src->meta[idx].c;
    pktBuff->first_hdr_chunk.line_number = src->meta[idx].line_number;
    pktBuff->first_hdr_chunk.horizontal_offset = src->meta[idx].hori_offset;
    pktBuff->first_hdr_chunk.s = src->meta[idx].s;
    pktBuff->first_hdr_chunk.stream_num = src->meta[idx].stream_num;
    pktBuff->second_hdr_chunk.did = tx_ancillary_apply_parity(s, src->meta[idx].did);
    pktBuff->second_hdr_chunk.sdid = tx_ancillary_apply_parity(s, src->meta[idx].sdid);
    pktBuff->second_hdr_chunk.data_count = tx_ancillary_apply_parity(s, udw_size);

    pktBuff->swapped_first_hdr_chunk = htonl(pktBuff->swapped_first_hdr_chunk);
    pktBuff->swapped_second_hdr_chunk = htonl(pktBuff->swapped_second_hdr_chunk);
    int i = 0;
    int offset = src->meta[idx].udw_offset;
    for (; i < udw_size; i++) {
      st40_set_udw(i + 3, tx_ancillary_apply_parity(s, src->data[offset++]),
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

    if (s->split_payload) {
      idx++;
      break;
    }
  }
  int payload_size = payload - (uint8_t*)&rtp[1];
  pkt->data_len += payload_size + sizeof(struct st40_rfc8331_rtp_hdr);
  pkt->pkt_len = pkt->data_len;
  rtp->length = htons(payload_size);
  rtp->first_hdr_chunk.anc_count = idx - anc_idx;
  if (s->ops.interlaced) {
    if (frame_info->tc_meta.second_field)
      rtp->first_hdr_chunk.f = 0b11;
    else
      rtp->first_hdr_chunk.f = 0b10;
  } else {
    rtp->first_hdr_chunk.f = 0b00;
  }
  if (!test_no_marker && idx == anc_count) rtp->base.marker = 1;
  dbg("%s(%d), anc_count %d, payload_size %d\n", __func__, s->idx, anc_count,
      payload_size);

  rtp->swapped_first_hdr_chunk = htonl(rtp->swapped_first_hdr_chunk);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return idx;
}

static int tx_ancillary_session_build_rtp_packet(struct st_tx_ancillary_session_impl* s,
                                                 struct rte_mbuf* pkt, int anc_idx) {
  struct st40_rfc8331_rtp_hdr* rtp;

  rtp = rte_pktmbuf_mtod(pkt, struct st40_rfc8331_rtp_hdr*);
  rte_memcpy(rtp, &s->hdr[MTL_SESSION_PORT_P].rtp, sizeof(*rtp));

  /* update rtp */
  tx_ancillary_set_rtp_seq(s, rtp);
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  bool test_no_marker =
      tx_ancillary_test_frame_active(s) && s->test.pattern == ST40_TX_TEST_NO_MARKER;

  /* Set place for payload just behind rtp header */
  uint8_t* payload = (uint8_t*)&rtp[1];
  struct st_frame_trans* frame_info = &s->st40_frames[s->st40_frame_idx];
  struct st40_frame* src = frame_info->addr;
  int anc_count = src->meta_num;
  if (tx_ancillary_test_frame_active(s) && s->split_payload && anc_count > 0 &&
      anc_idx >= anc_count)
    anc_idx = anc_count - 1;
  int total_udw = 0;
  int idx = 0;
  for (idx = anc_idx; idx < anc_count; idx++) {
    uint16_t udw_size = src->meta[idx].udw_size;
    total_udw += udw_size;
    if (!s->split_payload && (total_udw * 10 / 8) > s->max_pkt_len) break;
    struct st40_rfc8331_payload_hdr* pktBuff =
        (struct st40_rfc8331_payload_hdr*)(payload);
    pktBuff->first_hdr_chunk.c = src->meta[idx].c;
    pktBuff->first_hdr_chunk.line_number = src->meta[idx].line_number;
    pktBuff->first_hdr_chunk.horizontal_offset = src->meta[idx].hori_offset;
    pktBuff->first_hdr_chunk.s = src->meta[idx].s;
    pktBuff->first_hdr_chunk.stream_num = src->meta[idx].stream_num;
    pktBuff->second_hdr_chunk.did = tx_ancillary_apply_parity(s, src->meta[idx].did);
    pktBuff->second_hdr_chunk.sdid = tx_ancillary_apply_parity(s, src->meta[idx].sdid);
    pktBuff->second_hdr_chunk.data_count = tx_ancillary_apply_parity(s, udw_size);

    pktBuff->swapped_first_hdr_chunk = htonl(pktBuff->swapped_first_hdr_chunk);
    pktBuff->swapped_second_hdr_chunk = htonl(pktBuff->swapped_second_hdr_chunk);
    int i = 0;
    int offset = src->meta[idx].udw_offset;
    for (; i < udw_size; i++) {
      st40_set_udw(i + 3, tx_ancillary_apply_parity(s, src->data[offset++]),
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

    if (s->split_payload) {
      idx++;
      break;
    }
  }
  int payload_size = payload - (uint8_t*)&rtp[1];
  pkt->data_len = payload_size + sizeof(struct st40_rfc8331_rtp_hdr);
  pkt->pkt_len = pkt->data_len;
  rtp->length = htons(payload_size);
  rtp->first_hdr_chunk.anc_count = idx - anc_idx;
  if (s->ops.interlaced) {
    if (frame_info->tc_meta.second_field)
      rtp->first_hdr_chunk.f = 0b11;
    else
      rtp->first_hdr_chunk.f = 0b10;
  } else {
    rtp->first_hdr_chunk.f = 0b00;
  }
  if (!test_no_marker && idx == anc_count) rtp->base.marker = 1;

  rtp->swapped_first_hdr_chunk = htonl(rtp->swapped_first_hdr_chunk);

  dbg("%s(%d), anc_count %d, payload_size %d\n", __func__, s->idx, anc_count,
      payload_size);
  return idx;
}

static int tx_ancillary_session_rtp_update_packet(struct mtl_main_impl* impl,
                                                  struct st_tx_ancillary_session_impl* s,
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

  if (rtp->tmstamp != s->st40_rtp_time) {
    /* start of a new frame */
    s->st40_pkt_idx = 0;
    s->st40_anc_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st40_rtp_time = rtp->tmstamp;
    bool second_field = false;
    if (s->ops.interlaced) {
      struct st40_rfc8331_rtp_hdr* rfc8331 = (struct st40_rfc8331_rtp_hdr*)rtp;
      second_field = (rfc8331->first_hdr_chunk.f == 0b11) ? true : false;
      rfc8331->swapped_first_hdr_chunk = htonl(rfc8331->swapped_first_hdr_chunk);
    }
    if (s->ops.interlaced) {
      if (second_field) {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
      } else {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
      }
    }
    if (s->test.pattern != ST40_TX_TEST_NONE && s->test_frames_left) {
      s->test_frame_active = true;
      s->test_frames_left--;
      s->test_seq_gap_fired = false;
    } else {
      s->test_frame_active = false;
    }
    if (s->test_frame_active && s->test.paced_pkt_count)
      s->st40_total_pkts = RTE_MAX(1, (int)s->test.paced_pkt_count);
    tx_ancillary_session_sync_pacing(impl, s, 0);
    tx_ancillary_update_rtp_time_stamp(s, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       ntohl(rtp->tmstamp));
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

static int tx_ancillary_session_build_packet_chain(struct mtl_main_impl* impl,
                                                   struct st_tx_ancillary_session_impl* s,
                                                   struct rte_mbuf* pkt,
                                                   struct rte_mbuf* pkt_rtp,
                                                   enum mtl_session_port s_port) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st40_tx_ops* ops = &s->ops;

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
    if (ops->type == ST40_TYPE_RTP_LEVEL) {
      struct st40_rfc8331_rtp_hdr* rtp =
          rte_pktmbuf_mtod(pkt_rtp, struct st40_rfc8331_rtp_hdr*);
      if (rtp->base.tmstamp != s->st40_rtp_time) {
        /* start of a new frame */
        s->st40_pkt_idx = 0;
        s->st40_anc_idx = 0;
        rte_atomic32_inc(&s->stat_frame_cnt);
        s->port_user_stats.common.port[s_port].frames++;
        s->st40_rtp_time = rtp->base.tmstamp;
        bool second_field = false;
        if (s->ops.interlaced) {
          struct st40_rfc8331_rtp_hdr* rfc8331 = (struct st40_rfc8331_rtp_hdr*)&udp[1];
          second_field = (rfc8331->first_hdr_chunk.f == 0b11) ? true : false;
          rfc8331->swapped_first_hdr_chunk = htonl(rfc8331->swapped_first_hdr_chunk);
        }
        if (s->ops.interlaced) {
          if (second_field) {
            ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
          } else {
            ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
          }
        }
        tx_ancillary_session_sync_pacing(impl, s, 0);
        tx_ancillary_update_rtp_time_stamp(s, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                           ntohl(rtp->base.tmstamp));
      }
      rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
      rtp->swapped_first_hdr_chunk = htonl(rtp->swapped_first_hdr_chunk);
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

static inline int tx_ancillary_session_send_pkt(struct st_tx_ancillary_sessions_mgr* mgr,
                                                struct st_tx_ancillary_session_impl* s,
                                                enum mtl_session_port s_port,
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

static int tx_ancillary_session_tasklet_frame(struct mtl_main_impl* impl,
                                              struct st_tx_ancillary_sessions_mgr* mgr,
                                              struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  struct st40_tx_ops* ops = &s->ops;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
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
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_P,
                                        s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_R,
                                        s->inflight[MTL_SESSION_PORT_R]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_R] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (ST40_TX_STAT_WAIT_FRAME == s->st40_frame_stat) {
    uint16_t next_frame_idx;
    int total_udw = 0;
    struct st40_tx_frame_meta meta;

    if (s->check_frame_done_time) {
      uint64_t frame_end_time = mt_get_tsc(impl);
      if (frame_end_time > pacing->tsc_time_cursor) {
        ST_SESSION_STAT_INC(s, port_user_stats.common, stat_exceed_frame_time);
        dbg("%s(%d), frame %d build time out %" PRIu64 " us\n", __func__, idx,
            s->st40_frame_idx,
            (uint64_t)((frame_end_time - pacing->tsc_time_cursor) / NS_PER_US));
      }
      s->check_frame_done_time = false;
    }

    tx_ancillary_session_init_next_meta(s, &meta);
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
    struct st_frame_trans* frame = &s->st40_frames[next_frame_idx];
    int refcnt = rte_atomic32_read(&frame->refcnt);
    if (refcnt) {
      err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx, refcnt);
      s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
      return MTL_TASKLET_ALL_DONE;
    }
    rte_atomic32_inc(&frame->refcnt);
    frame->tc_meta = meta;
    s->st40_frame_idx = next_frame_idx;
    dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
    s->st40_frame_stat = ST40_TX_STAT_SENDING_PKTS;
    struct st40_frame* src = (struct st40_frame*)frame->addr;
    for (int i = 0; i < src->meta_num; i++) total_udw += src->meta[i].udw_size;
    int total_size = total_udw * 10 / 8;
    s->st40_pkt_idx = 0;
    s->st40_anc_idx = 0;
    if (s->split_payload) {
      s->st40_total_pkts = src->meta_num ? src->meta_num : 1;
    } else {
      s->st40_total_pkts = total_size / s->max_pkt_len;
      if (total_size % s->max_pkt_len) s->st40_total_pkts++;
      if (!s->st40_total_pkts) s->st40_total_pkts = 1;
      dbg("%s(%d), st40_total_pkts %d total_udw %d meta_num %u src %p\n", __func__, idx,
          s->st40_total_pkts, total_udw, src->meta_num, src);
      if (s->st40_total_pkts > 1) {
        err("%s(%d), frame %u invalid st40_total_pkts %d\n", __func__, idx,
            next_frame_idx, s->st40_total_pkts);
        s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
        return MTL_TASKLET_ALL_DONE;
      }
    }

    if (s->test.pattern != ST40_TX_TEST_NONE && s->test_frames_left) {
      s->test_frame_active = true;
      s->test_frames_left--;
      s->test_seq_gap_fired = false;
    } else {
      s->test_frame_active = false;
    }

    MT_USDT_ST40_TX_FRAME_NEXT(s->mgr->idx, s->idx, next_frame_idx, frame->addr,
                               src->meta_num, total_udw);
  }

  /* sync pacing */
  if (s->calculate_time_cursor) {
    struct st_frame_trans* frame = &s->st40_frames[s->st40_frame_idx];
    /* user timestamp control if any */
    uint64_t required_tai = tx_ancillary_pacing_required_tai(s, frame->tc_meta.tfmt,
                                                             frame->tc_meta.timestamp);
    bool second_field = frame->tc_meta.second_field;
    if (s->ops.interlaced) {
      if (second_field) {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
      } else {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
      }
    }
    tx_ancillary_session_sync_pacing(impl, s, required_tai);
    tx_ancillary_update_rtp_time_stamp(s, frame->tc_meta.tfmt, frame->tc_meta.timestamp);
    frame->tc_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
    frame->tc_meta.timestamp = pacing->ptp_time_cursor;
    frame->tc_meta.rtp_timestamp = pacing->rtp_time_stamp;
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
    int next_anc_idx = tx_ancillary_session_build_rtp_packet(s, pkt_rtp, s->st40_anc_idx);
    tx_ancillary_session_build_packet_chain(impl, s, pkt, pkt_rtp, MTL_SESSION_PORT_P);

    if (send_r) {
      pkt_r = rte_pktmbuf_alloc(hdr_pool_r);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, idx);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        rte_pktmbuf_free(pkt);
        rte_pktmbuf_free(pkt_rtp);
        return MTL_TASKLET_ALL_DONE;
      }
      tx_ancillary_session_build_packet_chain(impl, s, pkt_r, pkt_rtp,
                                              MTL_SESSION_PORT_R);
    }
    s->st40_anc_idx = next_anc_idx;
  } else {
    int next_anc_idx = tx_ancillary_session_build_packet(s, pkt);
    if (send_r) {
      pkt_r = rte_pktmbuf_copy(pkt, hdr_pool_r, 0, UINT32_MAX);
      if (!pkt_r) {
        err("%s(%d), rte_pktmbuf_copy redundant fail\n", __func__, idx);
        rte_pktmbuf_free(pkt);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
      tx_ancillary_session_update_redundant(s, pkt_r);
    }
    s->st40_anc_idx = next_anc_idx;
  }

  st_tx_mbuf_set_idx(pkt, s->st40_pkt_idx);
  st_tx_mbuf_set_tsc(pkt, pacing->tsc_time_cursor);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P]++;
  if (send_r) {
    st_tx_mbuf_set_idx(pkt_r, s->st40_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
  }

  s->st40_pkt_idx++;
  double pkt_time = pacing->frame_time / RTE_MAX(1, s->st40_total_pkts);
  if (tx_ancillary_test_frame_active(s) && s->test.pattern == ST40_TX_TEST_PACED &&
      s->test.paced_gap_ns)
    pkt_time = s->test.paced_gap_ns;
  pacing->tsc_time_cursor += pkt_time;
  /* keep one RTP timestamp across a multi-packet frame; re-sync after the last pkt */
  s->calculate_time_cursor = s->st40_pkt_idx >= s->st40_total_pkts;

  bool done = false;
  ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_P, pkt);
  if (ret != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = true;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_R, pkt_r);
    if (ret != 0) {
      s->inflight[MTL_SESSION_PORT_R] = pkt_r;
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      done = true;
      s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
    }
  }

  if (s->st40_pkt_idx >= s->st40_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st40_frame_idx);
    struct st_frame_trans* frame = &s->st40_frames[s->st40_frame_idx];
    struct st40_tx_frame_meta* tc_meta = &frame->tc_meta;
    uint64_t tsc_start = 0;
    bool time_measure = mt_sessions_time_measure(impl);
    if (time_measure) tsc_start = mt_get_tsc(impl);
    /* end of current frame */
    if (s->ops.notify_frame_done)
      ops->notify_frame_done(ops->priv, s->st40_frame_idx, tc_meta);
    if (time_measure) {
      uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
      s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
    }
    rte_atomic32_dec(&frame->refcnt);
    s->st40_frame_stat = ST40_TX_STAT_WAIT_FRAME;
    s->st40_pkt_idx = 0;
    s->st40_anc_idx = 0;
    s->test_frame_active = false;
    s->test_seq_gap_fired = false;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (send_r) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    pacing->tsc_time_cursor = 0;

    MT_USDT_ST40_TX_FRAME_DONE(s->mgr->idx, s->idx, s->st40_frame_idx,
                               tc_meta->rtp_timestamp);
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tx_ancillary_session_tasklet_rtp(struct mtl_main_impl* impl,
                                            struct st_tx_ancillary_sessions_mgr* mgr,
                                            struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  int ret;
  struct st_tx_ancillary_session_pacing* pacing = &s->pacing;
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
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_P,
                                        s->inflight[MTL_SESSION_PORT_P]);
    if (ret == 0) {
      s->inflight[MTL_SESSION_PORT_P] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r && s->inflight[MTL_SESSION_PORT_R]) {
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_R,
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
    tx_ancillary_session_rtp_update_packet(impl, s, pkt);
  } else {
    tx_ancillary_session_build_packet_chain(impl, s, pkt, pkt_rtp, MTL_SESSION_PORT_P);
  }
  st_tx_mbuf_set_idx(pkt, s->st40_pkt_idx);
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
      tx_ancillary_session_update_redundant(s, pkt_r);
    } else {
      tx_ancillary_session_build_packet_chain(impl, s, pkt_r, pkt_rtp,
                                              MTL_SESSION_PORT_R);
    }
    st_tx_mbuf_set_idx(pkt_r, s->st40_pkt_idx);
    st_tx_mbuf_set_tsc(pkt_r, pacing->tsc_time_cursor);
    s->stat_pkt_cnt[MTL_SESSION_PORT_R]++;
  }

  bool done = true;
  ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_P, pkt);
  if (ret != 0) {
    s->inflight[MTL_SESSION_PORT_P] = pkt;
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    done = false;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    ret = tx_ancillary_session_send_pkt(mgr, s, MTL_SESSION_PORT_R, pkt_r);
    if (ret != 0) {
      s->inflight[MTL_SESSION_PORT_R] = pkt_r;
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      done = false;
      s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_tx_ancillary_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_ancillary_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_ancillary_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    s->stat_build_ret_code = 0;
    if (s->ops.type == ST40_TYPE_FRAME_LEVEL)
      pending += tx_ancillary_session_tasklet_frame(impl, mgr, s);
    else
      pending += tx_ancillary_session_tasklet_rtp(impl, mgr, s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
    tx_ancillary_session_put(mgr, sidx);
  }

  return pending;
}

static int tx_ancillary_sessions_mgr_uinit_hw(struct st_tx_ancillary_sessions_mgr* mgr,
                                              enum mtl_port port) {
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

static int tx_ancillary_sessions_mgr_init_hw(struct mtl_main_impl* impl,
                                             struct st_tx_ancillary_sessions_mgr* mgr,
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

  snprintf(ring_name, 32, "%sM%dP%d", ST_TX_ANCILLARY_PREFIX, mgr_idx, port);
  flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ; /* multi-producer and single-consumer */
  count = ST_TX_ANC_SESSIONS_RING_SIZE;
  ring = rte_ring_create(ring_name, count, mgr->socket_id, flags);
  if (!ring) {
    err("%s(%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, port);
    tx_ancillary_sessions_mgr_uinit_hw(mgr, port);
    return -ENOMEM;
  }
  mgr->ring[port] = ring;
  info("%s(%d,%d), succ, queue %d\n", __func__, mgr_idx, port,
       mt_txq_queue_id(mgr->queue[port]));

  return 0;
}

static int tx_ancillary_session_sq_flush_port(struct st_tx_ancillary_sessions_mgr* mgr,
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

/* wa to flush the ancillary transmitter tx queue */
static int tx_ancillary_session_flush(struct st_tx_ancillary_sessions_mgr* mgr,
                                      struct st_tx_ancillary_session_impl* s) {
  int mgr_idx = mgr->idx, s_idx = s->idx;

  if (!s->shared_queue) return 0; /* skip as not shared queue */

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_hdr[i];
    if (pool && rte_mempool_in_use_count(pool) &&
        rte_atomic32_read(&mgr->transmitter_started)) {
      info("%s(%d,%d), start to flush port %d\n", __func__, mgr_idx, s_idx, i);
      tx_ancillary_session_sq_flush_port(mgr, mt_port_logic2phy(s->port_maps, i));
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

int tx_ancillary_session_mempool_free(struct st_tx_ancillary_session_impl* s) {
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

static bool tx_ancillary_session_has_chain_buf(struct st_tx_ancillary_session_impl* s) {
  struct st40_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  for (int port = 0; port < num_ports; port++) {
    if (!s->eth_has_chain[port]) return false;
  }

  /* all ports capable chain */
  return true;
}

static int tx_ancillary_session_mempool_init(struct mtl_main_impl* impl,
                                             struct st_tx_ancillary_sessions_mgr* mgr,
                                             struct st_tx_ancillary_session_impl* s) {
  struct st40_tx_ops* ops = &s->ops;
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
      n = mt_if_nb_tx_desc(impl, port) + ST_TX_ANC_SESSIONS_RING_SIZE;
      if (ops->type == ST40_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%dP%d_HDR", ST_TX_ANCILLARY_PREFIX, mgr->idx, idx,
               i);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
          hdr_room_size, s->socket_id);
      if (!mbuf_pool) {
        tx_ancillary_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  /* allocate payload(chain) pool */
  if (!s->tx_no_chain) {
    port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    n = mt_if_nb_tx_desc(impl, port) + ST_TX_ANC_SESSIONS_RING_SIZE;
    if (ops->type == ST40_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;

    if (s->tx_mono_pool) {
      s->mbuf_mempool_chain = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx,
           s->mbuf_mempool_chain);
    } else if (s->mbuf_mempool_chain) {
      warn("%s(%d), use previous chain mempool\n", __func__, idx);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%d_CHAIN", ST_TX_ANCILLARY_PREFIX, mgr->idx, idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
          chain_room_size, s->socket_id);
      if (!mbuf_pool) {
        tx_ancillary_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_chain = mbuf_pool;
    }
  }

  return 0;
}

static int tx_ancillary_session_init_rtp(struct st_tx_ancillary_sessions_mgr* mgr,
                                         struct st_tx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_TX_ANCILLARY_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
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

  tx_ancillary_session_flush(mgr, s);
  tx_ancillary_session_mempool_free(s);

  tx_ancillary_session_free_frames(s);

  return 0;
}

static int tx_ancillary_session_init_sw(struct mtl_main_impl* impl,
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
    ret = tx_ancillary_session_init_rtp(mgr, s);
  } else {
    ret = tx_ancillary_session_alloc_frames(s);
  }
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_ancillary_session_uinit_sw(mgr, s);
    return ret;
  }

  return 0;
}

static int tx_ancillary_session_uinit_queue(struct mtl_main_impl* impl,
                                            struct st_tx_ancillary_session_impl* s) {
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

static int tx_ancillary_session_init_queue(struct mtl_main_impl* impl,
                                           struct st_tx_ancillary_session_impl* s) {
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
      tx_ancillary_session_uinit_queue(impl, s);
      return -EIO;
    }
    queue_id = mt_txq_queue_id(s->queue[i]);
    info("%s(%d), port(l:%d,p:%d), queue %d\n", __func__, idx, i, port, queue_id);
  }

  return 0;
}

static int tx_ancillary_session_uinit(struct st_tx_ancillary_sessions_mgr* mgr,
                                      struct st_tx_ancillary_session_impl* s) {
  tx_ancillary_session_uinit_queue(mgr->parent, s);
  tx_ancillary_session_uinit_sw(mgr, s);
  return 0;
}

static int tx_ancillary_session_attach(struct mtl_main_impl* impl,
                                       struct st_tx_ancillary_sessions_mgr* mgr,
                                       struct st_tx_ancillary_session_impl* s,
                                       struct st40_tx_ops* ops) {
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
    snprintf(s->ops_name, sizeof(s->ops_name), "TX_ANC_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  s->split_payload = (ops->flags & ST40_TX_FLAG_SPLIT_ANC_BY_PKT) ? true : false;

  /* test-only mutation config */
  s->test = ops->test;
  if (s->test.pattern != ST40_TX_TEST_NONE && !s->test.frame_count)
    s->test.frame_count = 1;
  s->test_frames_left = s->test.frame_count;
  s->test_frame_active = false;
  s->test_seq_gap_fired = false;
  if (s->test.pattern != ST40_TX_TEST_NONE) s->split_payload = true;

  /* if disable shared queue */
  s->shared_queue = true;
  if (ops->flags & ST40_TX_FLAG_DEDICATE_QUEUE) s->shared_queue = false;

  for (int i = 0; i < num_port; i++) {
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10200 + idx * 2);
    if (mt_user_random_src_port(impl))
      s->st40_src_port[i] = mt_random_port(s->st40_dst_port[i]);
    else
      s->st40_src_port[i] =
          (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st40_dst_port[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = mt_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = mt_if_has_multi_seg(impl, port);

    if (s->shared_queue) {
      ret = tx_ancillary_sessions_mgr_init_hw(impl, mgr, port);
      if (ret < 0) {
        err("%s(%d), mgr init hw fail for port %d\n", __func__, idx, port);
        return ret;
      }
    }
  }
  s->tx_mono_pool = mt_user_tx_mono_pool(impl);
  /* manually disable chain or any port can't support chain */
  s->tx_no_chain = mt_user_tx_no_chain(impl) || !tx_ancillary_session_has_chain_buf(s);
  s->max_pkt_len = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc8331_anc_hdr);

  s->st40_frames_cnt = ops->framebuff_cnt;

  s->st40_frame_stat = ST40_TX_STAT_WAIT_FRAME;
  s->st40_frame_idx = 0;
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
  ret = tx_ancillary_session_init_pacing(s);
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
    tx_ancillary_session_uinit(mgr, s);
    return ret;
  }

  if (!s->shared_queue) {
    ret = tx_ancillary_session_init_queue(impl, s);
    if (ret < 0) {
      err("%s(%d), init dedicated queue fail %d\n", __func__, idx, ret);
      tx_ancillary_session_uinit(mgr, s);
      return ret;
    }
  } else {
    rte_atomic32_inc(&mgr->transmitter_clients);
  }

  info("%s(%d), type %d flags 0x%x pt %u, %s\n", __func__, idx, ops->type, ops->flags,
       ops->payload_type, ops->interlaced ? "interlace" : "progressive");
  return 0;
}

static void tx_ancillary_session_stat(struct st_tx_ancillary_session_impl* s) {
  int idx = s->idx;
  int frame_cnt = rte_atomic32_read(&s->stat_frame_cnt);
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = cur_time_ns;

  notice("TX_ANC_SESSION(%d:%s): fps %f frames %d pkts %d:%d\n", idx, s->ops_name,
         framerate, frame_cnt, s->stat_pkt_cnt[MTL_SESSION_PORT_P],
         s->stat_pkt_cnt[MTL_SESSION_PORT_R]);
  s->stat_pkt_cnt[MTL_SESSION_PORT_P] = 0;
  s->stat_pkt_cnt[MTL_SESSION_PORT_R] = 0;

  if (s->stat_epoch_mismatch) {
    notice("TX_ANC_SESSION(%d): st40 epoch mismatch %d\n", idx, s->stat_epoch_mismatch);
    s->stat_epoch_mismatch = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_ANC_SESSION(%d): epoch drop %u\n", idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_epoch_onward) {
    notice("TX_ANC_SESSION(%d): epoch onward %d\n", idx, s->stat_epoch_onward);
    s->stat_epoch_onward = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_AUDIO_SESSION(%d): build timeout frames %u\n", idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (frame_cnt <= 0) {
    warn("TX_ANC_SESSION(%d): build ret %d\n", idx, s->stat_build_ret_code);
  }
  if (s->ops.interlaced) {
    notice("TX_ANC_SESSION(%d): interlace first field %u second field %u\n", idx,
           s->stat_interlace_first_field, s->stat_interlace_second_field);
    s->stat_interlace_first_field = 0;
    s->stat_interlace_second_field = 0;
  }

  if (s->stat_error_user_timestamp) {
    notice("TX_ANC_SESSION(%d): error user timestamp %u\n", idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("TX_ANC_SESSION(%d): tasklet time avg %.2fus max %.2fus min %.2fus\n", idx,
           (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_next_frame_us > 8 || s->stat_max_notify_frame_us > 8) {
    notice("TX_ANC_SESSION(%d): get next frame max %uus, notify done max %uus\n", idx,
           s->stat_max_next_frame_us, s->stat_max_notify_frame_us);
  }
  s->stat_max_next_frame_us = 0;
  s->stat_max_notify_frame_us = 0;
}

static int tx_ancillary_session_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                       struct st_tx_ancillary_session_impl* s) {
  tx_ancillary_session_stat(s);
  tx_ancillary_session_uinit(mgr, s);
  if (s->shared_queue) {
    rte_atomic32_dec(&mgr->transmitter_clients);
  }
  return 0;
}

static int tx_ancillary_session_update_dst(struct mtl_main_impl* impl,
                                           struct st_tx_ancillary_sessions_mgr* mgr,
                                           struct st_tx_ancillary_session_impl* s,
                                           struct st_tx_dest_info* dest) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st40_tx_ops* ops = &s->ops;

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->dip_addr[i], dest->dip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = dest->udp_port[i];
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
    s->st40_src_port[i] =
        (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st40_dst_port[i];

    /* update hdr */
    ret = tx_ancillary_session_init_hdr(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  return 0;
}

static int tx_ancillary_sessions_mgr_update_dst(struct st_tx_ancillary_sessions_mgr* mgr,
                                                struct st_tx_ancillary_session_impl* s,
                                                struct st_tx_dest_info* dest) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = tx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = tx_ancillary_session_update_dst(mgr->parent, mgr, s, dest);
  tx_ancillary_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int st_tx_ancillary_sessions_stat(void* priv) {
  struct st_tx_ancillary_sessions_mgr* mgr = priv;
  struct st_tx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_ancillary_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    tx_ancillary_session_stat(s);
    tx_ancillary_session_put(mgr, j);
  }
  if (mgr->stat_pkts_burst > 0) {
    notice("TX_ANC_MGR, pkts burst %d\n", mgr->stat_pkts_burst);
    mgr->stat_pkts_burst = 0;
  } else {
    int32_t clients = rte_atomic32_read(&mgr->transmitter_clients);
    if ((clients > 0) && (mgr->max_idx > 0)) {
      for (int i = 0; i < mt_num_ports(mgr->parent); i++) {
        warn("TX_ANC_MGR: trs ret %d:%d\n", i, mgr->stat_trs_ret_code[i]);
      }
    }
  }

  return 0;
}

static int tx_ancillary_sessions_mgr_init(struct mtl_main_impl* impl,
                                          struct mtl_sch_impl* sch,
                                          struct st_tx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;
  int i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc8331_anc_hdr) != 62);

  mgr->parent = impl;
  mgr->idx = idx;
  mgr->socket_id = mt_sch_socket_id(sch);

  for (i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_ancillary_sessions_mgr";
  ops.start = tx_ancillary_sessions_tasklet_start;
  ops.handler = tx_ancillary_sessions_tasklet_handler;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_tx_ancillary_sessions_stat, mgr, "tx_anc");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_tx_ancillary_session_impl* tx_ancillary_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st40_tx_ops* ops) {
  struct st_tx_ancillary_sessions_mgr* mgr = &sch->tx_anc_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_tx_ancillary_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    if (!tx_ancillary_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = tx_ancillary_session_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = tx_ancillary_session_attach(mgr->parent, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
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

static int tx_ancillary_sessions_mgr_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                            struct st_tx_ancillary_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_ancillary_session_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  tx_ancillary_session_put(mgr, idx);

  return 0;
}

static int tx_ancillary_sessions_mgr_update(struct st_tx_ancillary_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

static int tx_ancillary_sessions_mgr_uinit(struct st_tx_ancillary_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_ancillary_session_impl* s;

  mt_stat_unregister(mgr->parent, st_tx_ancillary_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_TX_ANC_SESSIONS; i++) {
    s = tx_ancillary_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tx_ancillary_sessions_mgr_detach(mgr, s);
    tx_ancillary_session_put(mgr, i);
  }

  for (int i = 0; i < mt_num_ports(impl); i++) {
    tx_ancillary_sessions_mgr_uinit_hw(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int tx_ancillary_ops_check(struct st40_tx_ops* ops) {
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

  if (ops->type == ST40_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST40_TYPE_RTP_LEVEL) {
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

  if ((ops->flags & ST40_TX_FLAG_EXACT_USER_PACING) &&
      !(ops->flags & ST40_TX_FLAG_USER_PACING)) {
    err("%s, invalid flags 0x%x, need set USER_PACING with EXACT_USER_PACING\n", __func__,
        ops->flags);
    return -EINVAL;
  }

  return 0;
}

static int st_tx_anc_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->tx_anc_init) return 0;

  /* create tx ancillary context */
  ret = tx_ancillary_sessions_mgr_init(impl, sch, &sch->tx_anc_mgr);
  if (ret < 0) {
    err("%s, tx_ancillary_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_ancillary_transmitter_init(impl, sch, &sch->tx_anc_mgr, &sch->anc_trs);
  if (ret < 0) {
    tx_ancillary_sessions_mgr_uinit(&sch->tx_anc_mgr);
    err("%s, st_ancillary_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  sch->tx_anc_init = true;
  return 0;
}

int st_tx_ancillary_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->tx_anc_init) return 0;

  /* free tx ancillary context */
  st_ancillary_transmitter_uinit(&sch->anc_trs);
  tx_ancillary_sessions_mgr_uinit(&sch->tx_anc_mgr);

  sch->tx_anc_init = false;
  return 0;
}

st40_tx_handle st40_tx_create(mtl_handle mt, struct st40_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st_tx_ancillary_session_handle_impl* s_impl;
  struct st_tx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int quota_mbs, ret;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_ancillary_ops_check fail %d\n", __func__, ret);
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

  mt_pthread_mutex_lock(&sch->tx_anc_mgr_mutex);
  ret = st_tx_anc_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->tx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_anc_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_anc_mgr_mutex);
  s = tx_ancillary_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->tx_anc_mgr_mutex);
  if (!s) {
    err("%s, tx_ancillary_sessions_mgr_attach fail\n", __func__);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_TX_ANC;
  s_impl->impl = s;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st40_tx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

void* st40_tx_get_mbuf(st40_tx_handle handle, void** usrptr) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = NULL;
  struct st_tx_ancillary_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_HANDLE_TX_ANC) {
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

int st40_tx_put_mbuf(st40_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_ancillary_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_TX_ANC) {
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

int st40_tx_update_destination(st40_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct st_tx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) return ret;

  ret = tx_ancillary_sessions_mgr_update_dst(&sch->tx_anc_mgr, s, dst);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st40_tx_free(st40_tx_handle handle) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct st_tx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  struct mtl_main_impl* impl;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->tx_anc_mgr_mutex);
  ret = tx_ancillary_sessions_mgr_detach(&sch->tx_anc_mgr, s);
  mt_pthread_mutex_unlock(&sch->tx_anc_mgr_mutex);
  if (ret < 0) err("%s(%d), tx_ancillary_sessions_mgr_detach fail\n", __func__, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->tx_anc_mgr_mutex);
  tx_ancillary_sessions_mgr_update(&sch->tx_anc_mgr);
  mt_pthread_mutex_unlock(&sch->tx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_tx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

void* st40_tx_get_framebuffer(st40_tx_handle handle, uint16_t idx) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct st_tx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  if (idx >= s->st40_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st40_frames_cnt);
    return NULL;
  }
  if (!s->st40_frames) {
    err("%s, st40_frames not allocated\n", __func__);
    return NULL;
  }

  struct st_frame_trans* frame_info = &s->st40_frames[idx];

  return frame_info->addr;
}

int st40_tx_get_session_stats(st40_tx_handle handle, struct st40_tx_user_stats* stats) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_ancillary_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st40_tx_reset_session_stats(st40_tx_handle handle) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_ancillary_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}
