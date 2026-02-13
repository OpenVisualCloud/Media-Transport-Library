/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_audio_session.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_pcap.h"
#include "../mt_stat.h"
#include "st_rx_timing_parser.h"

static inline uint16_t rx_audio_queue_id(struct st_rx_audio_session_impl* s,
                                         enum mtl_session_port s_port) {
  return mt_rxq_queue_id(s->rxq[s_port]);
}

/* call rx_audio_session_put always if get successfully */
static inline struct st_rx_audio_session_impl* rx_audio_session_get(
    struct st_rx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_audio_session_put always if get successfully */
static inline struct st_rx_audio_session_impl* rx_audio_session_get_timeout(
    struct st_rx_audio_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_audio_session_put always if get successfully */
static inline struct st_rx_audio_session_impl* rx_audio_session_try_get(
    struct st_rx_audio_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_audio_session_put always if get successfully */
static inline bool rx_audio_session_get_empty(struct st_rx_audio_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_audio_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_audio_session_put(struct st_rx_audio_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static struct st_frame_trans* rx_audio_session_get_frame(
    struct st_rx_audio_session_impl* s) {
  struct st_frame_trans* frame_info;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    frame_info = &s->st30_frames[i];

    if (0 == rte_atomic32_read(&frame_info->refcnt)) {
      dbg("%s(%d), find frame at %d\n", __func__, s->idx, i);
      rte_atomic32_inc(&frame_info->refcnt);
      return frame_info;
    }
  }

  dbg("%s(%d), no free frame\n", __func__, s->idx);
  return NULL;
}

static int rx_audio_session_put_frame(struct st_rx_audio_session_impl* s,
                                      struct st_frame_trans* frame) {
  MTL_MAY_UNUSED(s);
  dbg("%s(%d), put frame at %d\n", __func__, s->idx, frame->idx);
  rte_atomic32_dec(&frame->refcnt);
  MT_USDT_ST30_RX_FRAME_PUT(s->mgr->idx, s->idx, frame->idx, frame->addr);
  return -EIO;
}

static int rx_audio_session_free_frames(struct st_rx_audio_session_impl* s) {
  /* free frames */
  if (s->st30_frames) {
    if (s->st30_cur_frame) {
      rx_audio_session_put_frame(s, s->st30_cur_frame);
      s->st30_cur_frame = NULL;
    }
    struct st_frame_trans* frame;
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

static int rx_audio_session_alloc_frames(struct st_rx_audio_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  size_t size = s->st30_frame_size;
  struct st_frame_trans* st30_frame;
  void* frame;

  s->st30_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st30_frames) * s->st30_frames_cnt, soc_id);
  if (!s->st30_frames) {
    err("%s(%d), st30_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    st30_frame = &s->st30_frames[i];
    rte_atomic32_set(&st30_frame->refcnt, 0);
    st30_frame->idx = i;
  }

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    st30_frame = &s->st30_frames[i];

    frame = mt_rte_zmalloc_socket(size, soc_id);
    if (!frame) {
      err("%s(%d), frame malloc %" PRIu64 " fail for %d\n", __func__, idx, size, i);
      rx_audio_session_free_frames(s);
      return -ENOMEM;
    }
    st30_frame->flags = ST_FT_FLAG_RTE_MALLOC;
    st30_frame->addr = frame;
    st30_frame->iova = rte_malloc_virt2iova(frame);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_free_rtps(struct st_rx_audio_session_impl* s) {
  if (s->st30_rtps_ring) {
    mt_ring_dequeue_clean(s->st30_rtps_ring);
    rte_ring_free(s->st30_rtps_ring);
    s->st30_rtps_ring = NULL;
  }

  return 0;
}

static int rx_audio_session_alloc_rtps(struct st_rx_audio_sessions_mgr* mgr,
                                       struct st_rx_audio_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_RTP", ST_RX_AUDIO_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  if (count <= 0) {
    err("%s(%d,%d), invalid rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
    return -ENOMEM;
  }
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->st30_rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_audio_sessions_tasklet_start(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_sessions_tasklet_stop(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_usdt_dump_close(struct st_rx_audio_session_impl* s) {
  int idx = s->idx;

  if (s->usdt_dump_fd >= 0) {
    info("%s(%d), close fd %d, dumped frames %d\n", __func__, idx, s->usdt_dump_fd,
         s->usdt_dumped_frames);
    close(s->usdt_dump_fd);
    s->usdt_dump_fd = -1;
  }
  return 0;
}

static int rx_audio_session_usdt_dump_frame(struct st_rx_audio_session_impl* s,
                                            struct st_frame_trans* frame) {
  struct st_rx_audio_sessions_mgr* mgr = s->mgr;
  int idx = s->idx;
  int ret;

  if (s->usdt_dump_fd < 0) {
    struct st30_rx_ops* ops = &s->ops;
    snprintf(s->usdt_dump_path, sizeof(s->usdt_dump_path),
             "imtl_usdt_st30rx_m%ds%d_%d_%d_c%u_XXXXXX.pcm", mgr->idx, idx,
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
      MT_USDT_ST30_RX_FRAME_DUMP(mgr->idx, s->idx, s->usdt_dump_path,
                                 s->usdt_dumped_frames);
    }
  }

  return 0;
}

static int rx_audio_session_handle_frame_pkt(struct mtl_main_impl* impl,
                                             struct st_rx_audio_session_impl* s,
                                             struct rte_mbuf* mbuf,
                                             enum mtl_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint8_t payload_type = rtp->payload_type;
  uint32_t pkt_len = mbuf->data_len - sizeof(struct st_rfc3550_audio_hdr);

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);

    return -EINVAL;
  }

  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->ssrc);
    if (ssrc != ops->ssrc) {
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);

      return -EINVAL;
    }
  }

  if (pkt_len != s->pkt_len) {
    dbg("%s(%d,%d), drop as pkt_len mismatch now %u expect %u\n", __func__, s->idx,
        s_port, pkt_len, s->pkt_len);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_len_mismatch_dropped);
    return -EINVAL;
  }

  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);

  if (s->st30_pkt_idx == 0) {
    s->first_pkt_rtp_ts = tmstamp;
    s->first_pkt_ptp_ts = mt_mbuf_time_stamp(impl, mbuf, port);
  }

  if (unlikely(s->latest_seq_id[s_port] == -1)) s->latest_seq_id[s_port] = seq_id - 1;
  if (unlikely(s->session_seq_id == -1)) s->session_seq_id = seq_id - 1;
  if (unlikely(s->tmstamp == -1)) s->tmstamp = tmstamp - 1;

  /* redundant stream seq_id out of order is not a big deal as long as stream is continous
   */
  if (seq_id != (uint16_t)(s->latest_seq_id[s_port] + 1)) {
    dbg("%s(%d,%d), non-continuous seq now %u last %d\n", __func__, s->idx, s_port,
        seq_id, s->latest_seq_id[s_port]);
    s->port_user_stats.common.port[s_port].out_of_order_packets++;
    s->stat_pkts_out_of_order_per_port[s_port]++;
  }
  s->latest_seq_id[s_port] = seq_id;

  /* all packets need to have increasing timestamp */
  if (!mt_seq32_greater(tmstamp, s->tmstamp)) {
    dbg("%s(%d,%d), drop as pkt seq_id %u (%u) or tmstamp %u (%ld) is old\n", __func__,
        s->idx, s_port, seq_id, s->latest_seq_id[s_port], tmstamp, s->tmstamp);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant);
    for (int i = 0; i < s->ops.num_port; i++) {
      if (s->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        return -EIO;
      }
    }
    warn("%s(%d), redundant error threshold reached, accept packet tmstamp (%d) %ld\n",
         __func__, s->idx, tmstamp, s->tmstamp);
  }
  s->redundant_error_cnt[s_port] = 0;
  s->tmstamp = tmstamp;

  /* hole in seq id packets going into the session check if the seq_id of the session is
   * consistent */
  if (seq_id != (uint16_t)(s->session_seq_id + 1)) {
    dbg("%s(%d,%d), session seq_id %u out of order %d\n", __func__, s->idx, s_port,
        seq_id, s->session_seq_id);
    s->stat_pkts_out_of_order++;
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
  }

  /* The package is accepted and goes into the frame */

  s->session_seq_id = seq_id;

  if (!s->st30_cur_frame) {
    s->st30_cur_frame = rx_audio_session_get_frame(s);
    if (!s->st30_cur_frame) {
      dbg("%s(%d,%d), seq %d drop as frame run out\n", __func__, s->idx, s_port, seq_id);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_slot_get_frame_fail);
      MT_USDT_ST30_RX_NO_FRAMEBUFFER(s->mgr->idx, s->idx, tmstamp);
      return -EIO;
    }
  }

  uint32_t offset = s->st30_pkt_idx * s->pkt_len;
  if ((offset + s->pkt_len) > s->st30_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st30_frame_size);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dropped);
    return -EIO;
  }
  rte_memcpy(s->st30_cur_frame->addr + offset, payload, s->pkt_len);
  s->frame_recv_size += s->pkt_len;
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  s->port_user_stats.common.port[s_port].packets++;
  s->st30_pkt_idx++;

  if (s->enable_timing_parser) {
    ra_tp_on_packet(s, s_port, tmstamp, mt_mbuf_time_stamp(impl, mbuf, port));
  }

  // notify frame done
  if (s->frame_recv_size >= s->st30_frame_size) {
    struct st30_rx_frame_meta* meta = &s->meta;
    uint64_t tsc_start = 0;
    struct st_frame_trans* frame = s->st30_cur_frame;

    if (s->enable_timing_parser) {
      /* parse timing result every 200ms */
      struct st_rx_audio_tp* tp = s->tp;
      uint64_t now = mt_get_tsc(impl);
      if ((now - tp->last_parse_time) > (200 * NS_PER_MS)) {
        for (int sp = 0; sp < ops->num_port; sp++) {
          struct st_ra_tp_slot* slot = &s->tp->slot[sp];
          ra_tp_slot_parse_result(s, sp);
          if (s->enable_timing_parser_meta) {
            ops->notify_timing_parser_result(ops->priv, sp, &slot->meta);
          }
          ra_tp_slot_init(slot);
        }
        tp->last_parse_time = now;
      }
    }

    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = s->first_pkt_rtp_ts;
    meta->timestamp_first_pkt = s->first_pkt_ptp_ts;
    meta->fmt = ops->fmt;
    meta->sampling = ops->sampling;
    meta->channel = ops->channel;
    meta->rtp_timestamp = s->first_pkt_rtp_ts;
    meta->frame_recv_size = s->frame_recv_size;

    MT_USDT_ST30_RX_FRAME_AVAILABLE(s->mgr->idx, s->idx, frame->idx, frame->addr,
                                    s->first_pkt_rtp_ts, meta->frame_recv_size);
    /* check if dump USDT enabled */
    if (MT_USDT_ST30_RX_FRAME_DUMP_ENABLED()) {
      rx_audio_session_usdt_dump_frame(s, frame);
    } else {
      rx_audio_session_usdt_dump_close(s);
    }

    /* get a full frame */
    bool time_measure = mt_sessions_time_measure(impl);
    if (time_measure) tsc_start = mt_get_tsc(impl);
    int ret = ops->notify_frame_ready(ops->priv, frame->addr, meta);
    if (time_measure) {
      uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
      s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
    }
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      rx_audio_session_put_frame(s, frame);
    }
    s->frame_recv_size = 0;
    s->st30_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frames_received);
    s->port_user_stats.common.port[s_port].frames++;
    s->st30_cur_frame = NULL;
  }

  return 0;
}

static int rx_audio_session_handle_rtp_pkt(struct mtl_main_impl* impl,
                                           struct st_rx_audio_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum mtl_session_port s_port) {
  struct st30_rx_ops* ops = &s->ops;
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(s_port);

  uint16_t seq_id = ntohs(rtp->seq_number);
  uint8_t payload_type = rtp->payload_type;
  uint32_t tmstamp = ntohl(rtp->tmstamp);

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->ssrc);
    if (ssrc != ops->ssrc) {
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }

  if (unlikely(s->latest_seq_id[s_port] == -1)) s->latest_seq_id[s_port] = seq_id - 1;
  if (unlikely(s->session_seq_id == -1)) s->session_seq_id = seq_id - 1;
  if (unlikely(s->tmstamp == -1)) s->tmstamp = tmstamp - 1;

  /* redundant stream seq_id out of order is not a big deal as long as stream is continous
   */
  if (seq_id != (uint16_t)(s->latest_seq_id[s_port] + 1)) {
    dbg("%s(%d,%d), non-continuous seq now %u last %d\n", __func__, s->idx, s_port,
        seq_id, s->latest_seq_id[s_port]);
    s->port_user_stats.common.port[s_port].out_of_order_packets++;
    s->stat_pkts_out_of_order_per_port[s_port]++;
  }
  s->latest_seq_id[s_port] = seq_id;

  /* all packets need to have increasing timestamp */
  if (!mt_seq32_greater(tmstamp, s->tmstamp)) {
    dbg("%s(%d,%d), drop as pkt seq_id %u (%u) or tmstamp %u (%ld) is old\n", __func__,
        s->idx, s_port, seq_id, s->latest_seq_id[s_port], tmstamp, s->tmstamp);
    s->stat_pkts_redundant++;
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant);
    for (int i = 0; i < s->ops.num_port; i++) {
      if (s->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        return -EIO;
      }
    }

    /* should never happen */
    warn("%s(%d), redundant error threshold reached, accept packet tmstamp (%d) %ld\n",
         __func__, s->idx, tmstamp, s->tmstamp);
  }
  s->redundant_error_cnt[s_port] = 0;
  s->tmstamp = tmstamp;

  /* hole in seq id packets going into the session check if the seq_id of the session is
   * consistent */
  if (seq_id != (uint16_t)(s->session_seq_id + 1)) {
    dbg("%s(%d,%d), session seq_id %u out of order %d\n", __func__, s->idx, s_port,
        seq_id, s->session_seq_id);
    s->stat_pkts_out_of_order++;
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
  }

  /* The package is accepted and goes into the frame */
  s->session_seq_id = seq_id;

  /* enqueue the packet ring to app */
  int ret = rte_ring_sp_enqueue(s->st30_rtps_ring, (void*)mbuf);
  if (ret < 0) {
    dbg("%s(%d,%d), drop as rtps ring full\n", __func__, seq_id, s_port);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_slot_get_frame_fail);
    return -EIO;
  }
  rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */

  ops->notify_rtp_ready(ops->priv);
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  s->port_user_stats.common.port[s_port].packets++;

  return 0;
}

static void rx_audio_session_reset(struct st_rx_audio_session_impl* s,
                                   bool init_stat_time_now) {
  if (!s) return;

  s->session_seq_id = -1;
  s->latest_seq_id[MTL_SESSION_PORT_P] = -1;
  s->latest_seq_id[MTL_SESSION_PORT_R] = -1;
  s->tmstamp = -1;
  s->frame_recv_size = 0;
  s->st30_pkt_idx = 0;
  s->st30_cur_frame = NULL;
  s->first_pkt_rtp_ts = 0;
  s->stat_pkts_dropped = 0;
  s->stat_pkts_redundant = 0;
  s->stat_pkts_out_of_order = 0;
  s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_P] = 0;
  s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_R] = 0;
  s->stat_slot_get_frame_fail = 0;
  s->stat_pkts_wrong_pt_dropped = 0;
  s->stat_pkts_wrong_ssrc_dropped = 0;
  s->stat_pkts_len_mismatch_dropped = 0;
  s->stat_pkts_received = 0;
  s->stat_last_time = init_stat_time_now ? mt_get_monotonic_time() : 0;
  s->stat_max_notify_frame_us = 0;
  rte_atomic32_set(&s->stat_frames_received, 0);
  mt_stat_u64_init(&s->stat_time);
  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    s->redundant_error_cnt[i] = 0;
  }

  if (init_stat_time_now) s->usdt_dump_fd = -1;
}

#ifdef MTL_ENABLE_FUZZING_ST30
int st_rx_audio_session_fuzz_handle_pkt(struct mtl_main_impl* impl,
                                        struct st_rx_audio_session_impl* s,
                                        struct rte_mbuf* mbuf,
                                        enum mtl_session_port s_port) {
  if (!s || !mbuf) return -EINVAL;
  if (s->ops.type == ST30_TYPE_RTP_LEVEL)
    return rx_audio_session_handle_rtp_pkt(impl, s, mbuf, s_port);
  return rx_audio_session_handle_frame_pkt(impl, s, mbuf, s_port);
}

void st_rx_audio_session_fuzz_reset(struct st_rx_audio_session_impl* s) {
  rx_audio_session_reset(s, false);
}
#endif

static int ra_stop_pcap(struct st_rx_audio_session_impl* s,
                        enum mtl_session_port s_port) {
  struct mt_rx_pcap* pcap = &s->pcap[s_port];

  if (!pcap->pcap) return 0;

  info("%s(%d,%d), dumped %u packets to %s, dropped %u packets\n", __func__, s->idx,
       s_port, pcap->dumped_pkts, pcap->file_name, pcap->dropped_pkts);
  MT_USDT_ST30_RX_PCAP_DUMP(s->mgr->idx, s->idx, s_port, pcap->file_name,
                            pcap->dumped_pkts);
  pcap->required_pkts = 0;
  mt_pcap_close(pcap->pcap);
  pcap->pcap = NULL;
  return 0;
}

static int rv_stop_pcap_dump(struct st_rx_audio_session_impl* s) {
  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    ra_stop_pcap(s, s_port);
  }
  return 0;
}

static int ra_start_pcap(struct st_rx_audio_session_impl* s, enum mtl_session_port s_port,
                         uint32_t max_dump_packets) {
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_rx_pcap* pcap = &s->pcap[s_port];

  if (pcap->pcap) {
    err("%s(%d,%d), pcap dump already started\n", __func__, idx, s_port);
    return -EIO;
  }

  snprintf(pcap->file_name, sizeof(pcap->file_name), "st30rx_s%dp%d_%u_XXXXXX.pcapng",
           idx, s_port, max_dump_packets);
  int fd = mt_mkstemps(pcap->file_name, strlen(".pcapng"));
  if (fd < 0) {
    err("%s(%d,%d), failed to create pcap file %s\n", __func__, idx, s_port,
        pcap->file_name);
    return -EIO;
  }
  pcap->pcap = mt_pcap_open(s->mgr->parent, port, fd);
  if (!pcap->pcap) {
    err("%s(%d,%d), failed to open pcap file %s\n", __func__, idx, s_port,
        pcap->file_name);
    close(fd);
    return -EIO;
  }

  pcap->dumped_pkts = 0;
  pcap->dropped_pkts = 0;
  pcap->required_pkts = max_dump_packets;
  info("%s(%d,%d), pcap %s started, required dump pkts %u\n", __func__, idx, s_port,
       pcap->file_name, max_dump_packets);
  return 0;
}

static int ra_dump_pcap(struct st_rx_audio_session_impl* s, struct rte_mbuf** mbufs,
                        uint16_t nb, enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_rx_pcap* pcap = &s->pcap[s_port];
  uint16_t dump = mt_pcap_dump(s->mgr->parent, port, pcap->pcap, mbufs, nb);
  pcap->dumped_pkts += dump;
  pcap->dropped_pkts += nb - dump;
  return 0;
}

static int rx_audio_session_handle_mbuf(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_audio_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_session_port s_port = s_priv->s_port;
  enum st30_type st30_type = s->ops.type;

  if (!s->attached) {
    dbg("%s(%d,%d), session not ready\n", __func__, s->idx, s_port);
    return -EIO;
  }

  struct mt_rx_pcap* pcap = &s->pcap[s_port];
  if (pcap->required_pkts) {
    if (pcap->dumped_pkts < pcap->required_pkts) {
      ra_dump_pcap(s, mbuf, RTE_MIN(nb, pcap->required_pkts - pcap->dumped_pkts), s_port);
    } else { /* got enough packets, stop dumping */
      ra_stop_pcap(s, s_port);
    }
  }

  if (ST30_TYPE_FRAME_LEVEL == st30_type) {
    for (uint16_t i = 0; i < nb; i++)
      rx_audio_session_handle_frame_pkt(impl, s, mbuf[i], s_port);
  } else {
    for (uint16_t i = 0; i < nb; i++) {
      rx_audio_session_handle_rtp_pkt(impl, s, mbuf[i], s_port);
    }
  }

  return 0;
}

static int rx_audio_session_tasklet(struct st_rx_audio_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_AUDIO_BURST_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;

  bool done = true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->rxq[s_port]) continue;

    struct mt_rx_pcap* pcap = &s->pcap[s_port];
    /* if any pcap progress */
    if (MT_USDT_ST30_RX_PCAP_DUMP_ENABLED()) {
      if (!pcap->usdt_dump) {
        /* dump 5 sec */
        int required_pkts = s->st30_total_pkts * s->frames_per_sec * 5;
        ra_start_pcap(s, s_port, required_pkts);
        pcap->usdt_dump = true;
      }
    } else {
      if (pcap->usdt_dump) {
        ra_stop_pcap(s, s_port);
        pcap->usdt_dump = false;
      }
    }

    rv = mt_rxq_burst(s->rxq[s_port], &mbuf[0], ST_RX_AUDIO_BURST_SIZE);
    if (!rv) continue;

    rx_audio_session_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
    rte_pktmbuf_free_bulk(&mbuf[0], rv);
    if (s->enable_timing_parser && s->tp) {
      if (rv > 1) s->tp->stat_bursted_cnt[s_port]++;
    }
    done = false;
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int rx_audio_sessions_tasklet_handler(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_rx_audio_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_audio_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    pending += rx_audio_session_tasklet(s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
    rx_audio_session_put(mgr, sidx);
  }

  return pending;
}

static int rx_audio_session_uinit_hw(struct st_rx_audio_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->rxq[i]) {
      mt_rxq_put(s->rxq[i]);
      s->rxq[i] = NULL;
    }
  }

  return 0;
}

static int rx_audio_session_init_hw(struct mtl_main_impl* impl,
                                    struct st_rx_audio_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  struct mt_rxq_flow flow;
  enum mtl_port port;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].s_port = i;

    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.ip_addr[i], MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow.dip_addr))
      rte_memcpy(flow.sip_addr, s->ops.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    else
      rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st30_dst_port[i];
    if (mt_has_cni_rx(impl, port)) flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;

    /* no flow for data path only */
    if (s->ops.flags & ST30_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), rxq get without flow for port %d as data path only\n", __func__,
           s->idx, i);
      s->rxq[i] = mt_rxq_get(impl, port, NULL);
    } else {
      s->rxq[i] = mt_rxq_get(impl, port, &flow);
    }
    if (!s->rxq[i]) {
      rx_audio_session_uinit_hw(s);
      return -EIO;
    }

    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         rx_audio_queue_id(s, i), flow.dst_port);
  }

  return 0;
}

static int rx_audio_session_uinit_mcast(struct mtl_main_impl* impl,
                                        struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!s->mcast_joined[i]) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    mt_mcast_leave(impl, mt_ip_to_u32(ops->ip_addr[i]),
                   mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
  }

  return 0;
}

static int rx_audio_session_init_mcast(struct mtl_main_impl* impl,
                                       struct st_rx_audio_session_impl* s) {
  struct st30_rx_ops* ops = &s->ops;
  int ret;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!mt_is_multicast_ip(ops->ip_addr[i])) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    if (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), skip mcast join for port %d\n", __func__, s->idx, i);
      return 0;
    }
    ret = mt_mcast_join(impl, mt_ip_to_u32(ops->ip_addr[i]),
                        mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
    if (ret < 0) return ret;
    s->mcast_joined[i] = true;
  }

  return 0;
}

static int rx_audio_session_uinit_sw(struct st_rx_audio_session_impl* s) {
  rx_audio_session_free_frames(s);
  rx_audio_session_free_rtps(s);
  rx_audio_session_usdt_dump_close(s);
  return 0;
}

static int rx_audio_session_init_sw(struct st_rx_audio_sessions_mgr* mgr,
                                    struct st_rx_audio_session_impl* s) {
  enum st30_type st30_type = s->ops.type;
  int idx = s->idx;
  int ret;

  switch (st30_type) {
    case ST30_TYPE_FRAME_LEVEL:
      ret = rx_audio_session_alloc_frames(s);
      break;
    case ST30_TYPE_RTP_LEVEL:
      ret = rx_audio_session_alloc_rtps(mgr, s);
      break;
    default:
      err("%s(%d), error st30_type %d\n", __func__, idx, st30_type);
      return -EIO;
  }
  if (ret < 0) return ret;

  return 0;
}

static int rx_audio_session_uinit(struct mtl_main_impl* impl,
                                  struct st_rx_audio_session_impl* s) {
  rv_stop_pcap_dump(s);
  ra_tp_uinit(s);
  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_sw(s);
  rx_audio_session_uinit_hw(s);
  return 0;
}

static int rx_audio_session_attach(struct mtl_main_impl* impl,
                                   struct st_rx_audio_sessions_mgr* mgr,
                                   struct st_rx_audio_session_impl* s,
                                   struct st30_rx_ops* ops) {
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
    snprintf(s->ops_name, sizeof(s->ops_name), "RX_AUDIO_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st30_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx * 2);
  }

  ret = st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  if (ret < 0) return ret;
  s->pkt_len = ret;

  size_t bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc3550_audio_hdr);
  s->st30_pkt_size = s->pkt_len + sizeof(struct st_rfc3550_audio_hdr);
  if (s->pkt_len > bytes_in_pkt) {
    err("%s(%d), invalid pkt_len %d\n", __func__, idx, s->pkt_len);
    return -EIO;
  }

  s->st30_frames_cnt = ops->framebuff_cnt;
  s->st30_total_pkts = ops->framebuff_size / s->pkt_len;
  if (ops->framebuff_size % s->pkt_len) {
    /* todo: add the support? */
    err("%s(%d), framebuff_size %d not multiple pkt_len %d\n", __func__, idx, s->pkt_len,
        ops->framebuff_size);
    return -EIO;
  }
  s->st30_frame_size = ops->framebuff_size;
  rx_audio_session_reset(s, true);

  if (ops->flags & ST30_RX_FLAG_TIMING_PARSER_STAT) {
    info("%s(%d), enable the timing analyze stat\n", __func__, idx);
    s->enable_timing_parser = true;
    s->enable_timing_parser_stat = true;
  }
  if (ops->flags & ST30_RX_FLAG_TIMING_PARSER_META) {
    if (!ops->notify_timing_parser_result) {
      err("%s(%d), notify_timing_parser_result callback is NULL\n", __func__, idx);
      return -EIO;
    }
    info("%s(%d), enable the timing analyze meta\n", __func__, idx);
    s->enable_timing_parser = true;
    s->enable_timing_parser_meta = true;
  }

  if (s->enable_timing_parser) {
    ret = ra_tp_init(impl, s);
    if (ret < 0) {
      err("%s(%d), ra_tp_init fail %d\n", __func__, idx, ret);
      rx_audio_session_uinit(impl, s);
      return ret;
    }
  }

  ret = rx_audio_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    rx_audio_session_uinit(impl, s);
    return ret;
  }

  ret = rx_audio_session_init_sw(mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_sw fail %d\n", __func__, idx, ret);
    rx_audio_session_uinit(impl, s);
    return ret;
  }

  ret = rx_audio_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_audio_session_uinit(impl, s);
    return ret;
  }

  s->frames_per_sec =
      (double)NS_PER_S / st30_get_packet_time(ops->ptime) / s->st30_total_pkts;
  s->attached = true;
  info("%s(%d), fmt %d channel %u sampling %d ptime %d payload_type %u\n", __func__, idx,
       ops->fmt, ops->channel, ops->sampling, ops->ptime, ops->payload_type);
  info("%s(%d), pkt_len %u frame_size %" PRId64 ", expect fps %d\n", __func__, idx,
       s->pkt_len, s->st30_frame_size, s->frames_per_sec);
  return 0;
}

static void rx_audio_session_stat(struct st_rx_audio_sessions_mgr* mgr,
                                  struct st_rx_audio_session_impl* s) {
  int idx = s->idx;
  int m_idx = mgr->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->stat_frames_received, 0);

  if (s->stat_pkts_redundant) {
    notice("RX_AUDIO_SESSION(%d,%d:%s): fps %f frames %d pkts %d (redundant %d)\n", m_idx,
           idx, s->ops_name, framerate, frames_received, s->stat_pkts_received,
           s->stat_pkts_redundant);
    s->stat_pkts_redundant = 0;
  } else {
    info("RX_AUDIO_SESSION(%d,%d:%s): fps %f frames %d pkts %d\n", m_idx, idx,
         s->ops_name, framerate, frames_received, s->stat_pkts_received);
  }

  s->stat_pkts_received = 0;
  s->stat_last_time = cur_time_ns;
  if (s->stat_pkts_out_of_order) {
    warn("RX_AUDIO_SESSION(%d): out of order pkts %d (%d:%d)\n", idx,
         s->stat_pkts_out_of_order,
         s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_P],
         s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_R]);
    s->stat_pkts_out_of_order = 0;
    s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_P] = 0;
    s->stat_pkts_out_of_order_per_port[MTL_SESSION_PORT_R] = 0;
  }

  if (s->stat_pkts_dropped) {
    notice("RX_AUDIO_SESSION(%d,%d): dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_dropped);
    s->stat_pkts_dropped = 0;
  }
  if (s->stat_pkts_wrong_pt_dropped) {
    notice("RX_AUDIO_SESSION(%d,%d): wrong hdr payload_type dropped pkts %d\n", m_idx,
           idx, s->stat_pkts_wrong_pt_dropped);
    s->stat_pkts_wrong_pt_dropped = 0;
  }
  if (s->stat_pkts_wrong_ssrc_dropped) {
    notice("RX_AUDIO_SESSION(%d,%d): wrong hdr ssrc dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_ssrc_dropped);
    s->stat_pkts_wrong_ssrc_dropped = 0;
  }
  if (s->stat_pkts_len_mismatch_dropped) {
    notice("RX_AUDIO_SESSION(%d,%d): pkt len mismatch dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_len_mismatch_dropped);
    s->stat_pkts_len_mismatch_dropped = 0;
  }
  if (s->stat_slot_get_frame_fail) {
    notice("RX_AUDIO_SESSION(%d,%d): slot get frame fail %u\n", m_idx, idx,
           s->stat_slot_get_frame_fail);
    s->stat_slot_get_frame_fail = 0;
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("RX_AUDIO_SESSION(%d,%d): tasklet time avg %.2fus max %.2fus min %.2fus\n",
           m_idx, idx, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_notify_frame_us > 8) {
    notice("RX_AUDIO_SESSION(%d,%d): notify frame max %uus\n", m_idx, idx,
           s->stat_max_notify_frame_us);
  }
  s->stat_max_notify_frame_us = 0;

  if (s->enable_timing_parser_stat) ra_tp_stat(s);

  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    struct mt_rx_pcap* pcap = &s->pcap[s_port];
    if (pcap->pcap) {
      MT_USDT_ST30_RX_PCAP_DUMP(m_idx, idx, s_port, pcap->file_name, pcap->dumped_pkts);
    }
  }
}

static int rx_audio_session_detach(struct mtl_main_impl* impl,
                                   struct st_rx_audio_sessions_mgr* mgr,
                                   struct st_rx_audio_session_impl* s) {
  s->attached = false;
  rx_audio_session_stat(mgr, s);
  rx_audio_session_uinit(impl, s);
  return 0;
}

static int rx_audio_session_update_src(struct mtl_main_impl* impl,
                                       struct st_rx_audio_session_impl* s,
                                       struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st30_rx_ops* ops = &s->ops;

  rx_audio_session_uinit_mcast(impl, s);
  rx_audio_session_uinit_hw(s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->ip_addr[i], src->ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops->mcast_sip_addr[i], src->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st30_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (20000 + idx * 2);
  }
  /* reset seq id */

  s->session_seq_id = -1;
  s->latest_seq_id[MTL_SESSION_PORT_P] = -1;
  s->latest_seq_id[MTL_SESSION_PORT_R] = -1;
  s->tmstamp = -1;

  ret = rx_audio_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rx_audio_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_audio_sessions_mgr_update_src(struct st_rx_audio_sessions_mgr* mgr,
                                            struct st_rx_audio_session_impl* s,
                                            struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rx_audio_session_update_src(mgr->parent, s, src);
  rx_audio_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int st_rx_audio_sessions_stat(void* priv) {
  struct st_rx_audio_sessions_mgr* mgr = priv;
  struct st_rx_audio_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_audio_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    rx_audio_session_stat(mgr, s);
    rx_audio_session_put(mgr, j);
  }

  return 0;
}

static int rx_audio_sessions_mgr_init(struct mtl_main_impl* impl,
                                      struct mtl_sch_impl* sch,
                                      struct st_rx_audio_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  mgr->parent = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_SCH_MAX_RX_AUDIO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_audio_sessions_mgr";
  ops.start = rx_audio_sessions_tasklet_start;
  ops.stop = rx_audio_sessions_tasklet_stop;
  ops.handler = rx_audio_sessions_tasklet_handler;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_rx_audio_sessions_stat, mgr, "rx_audio");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_audio_session_init(struct st_rx_audio_sessions_mgr* mgr,
                                 struct st_rx_audio_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
  s->idx = idx;
  return 0;
}

static struct st_rx_audio_session_impl* rx_audio_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st30_rx_ops* ops) {
  struct st_rx_audio_sessions_mgr* mgr = &sch->rx_a_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_rx_audio_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_RX_AUDIO_SESSIONS; i++) {
    if (!rx_audio_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = rx_audio_session_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rx_audio_session_attach(mgr->parent, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_audio_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_audio_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail to find free slot\n", __func__, midx);
  return NULL;
}

static int rx_audio_sessions_mgr_detach(struct st_rx_audio_sessions_mgr* mgr,
                                        struct st_rx_audio_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_audio_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rx_audio_session_detach(mgr->parent, mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  rx_audio_session_put(mgr, idx);

  return 0;
}

static int rx_audio_sessions_mgr_update(struct st_rx_audio_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_SCH_MAX_RX_AUDIO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

static int rx_audio_sessions_mgr_uinit(struct st_rx_audio_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_audio_session_impl* s;

  mt_stat_unregister(mgr->parent, st_rx_audio_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_RX_AUDIO_SESSIONS; i++) {
    s = rx_audio_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rx_audio_sessions_mgr_detach(mgr, s);
    rx_audio_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int rx_audio_ops_check(struct st30_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip = NULL;

  if ((num_ports > MTL_SESSION_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->ip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->ip_addr[0], ops->ip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST30_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
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
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  /* Zero means disable the payload_type check */
  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_audio_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->rx_a_init) return 0;

  /* create rx audio context */
  ret = rx_audio_sessions_mgr_init(impl, sch, &sch->rx_a_mgr);
  if (ret < 0) {
    err("%s, rx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  sch->rx_a_init = true;
  return 0;
}

int st_rx_audio_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->rx_a_init) return 0;

  rx_audio_sessions_mgr_uinit(&sch->rx_a_mgr);

  sch->rx_a_init = false;
  return 0;
}

st30_rx_handle st30_rx_create(mtl_handle mt, struct st30_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st_rx_audio_session_handle_impl* s_impl;
  struct st_rx_audio_session_impl* s;
  int quota_mbs, ret;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST30_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST30_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  quota_mbs = impl->main_sch->data_quota_mbs_limit / impl->rx_audio_sessions_max_per_sch;
  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_a_mgr_mutex);
  ret = st_rx_audio_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_a_mgr_mutex);
  s = rx_audio_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->rx_a_mgr_mutex);
  if (!s) {
    err("%s(%d), rx_audio_sessions_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_RX_AUDIO;
  s_impl->impl = s;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;
  s->st30_handle = s_impl;

  rte_atomic32_inc(&impl->st30_rx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st30_rx_update_source(st30_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rx_audio_sessions_mgr_update_src(&sch->rx_a_mgr, s, src);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st30_rx_free(st30_rx_handle handle) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct mtl_sch_impl* sch;
  struct st_rx_audio_session_impl* s;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->rx_a_mgr_mutex);
  ret = rx_audio_sessions_mgr_detach(&sch->rx_a_mgr, s);
  mt_pthread_mutex_unlock(&sch->rx_a_mgr_mutex);
  if (ret < 0) err("%s(%d, %d), mgr detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->rx_a_mgr_mutex);
  rx_audio_sessions_mgr_update(&sch->rx_a_mgr);
  mt_pthread_mutex_unlock(&sch->rx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_rx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st30_rx_put_framebuff(st30_rx_handle handle, void* frame) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct st_frame_trans* st30_frame;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    st30_frame = &s->st30_frames[i];
    if (st30_frame->addr == frame) {
      dbg("%s(%d), put frame at %d\n", __func__, s->idx, i);
      return rx_audio_session_put_frame(s, st30_frame);
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, s->idx, frame);
  return -EIO;
}

void* st30_rx_get_mbuf(st30_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct rte_mbuf* pkt;
  struct rte_ring* rtps_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->st30_rtps_ring;
  if (!rtps_ring) {
    err("%s(%d), rtp ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(rtps_ring, (void**)&pkt);
  if (ret < 0) {
    dbg("%s(%d), rtp ring is empty\n", __func__, idx);
    return NULL;
  }

  size_t hdr_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                   sizeof(struct rte_udp_hdr);
  *len = pkt->data_len - hdr_len;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  return pkt;
}

void st30_rx_put_mbuf(st30_rx_handle handle, void* mbuf) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_HANDLE_RX_AUDIO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st30_rx_get_queue_meta(st30_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    meta->queue_id[i] = rx_audio_queue_id(s, i);
  }

  return 0;
}

int st30_rx_get_session_stats(st30_rx_handle handle, struct st30_rx_user_stats* stats) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_audio_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st30_rx_reset_session_stats(st30_rx_handle handle) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_audio_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}
