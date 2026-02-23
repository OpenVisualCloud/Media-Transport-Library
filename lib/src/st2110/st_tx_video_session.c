/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_video_session.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_rtcp.h"
#include "../mt_stat.h"
#include "../mt_util.h"
#include "st_err.h"
#include "st_video_transmitter.h"

#ifdef MTL_SIMULATE_PACKET_DROPS
static inline void tv_simulate_packet_loss(struct st_tx_video_session_impl* s,
                                           struct rte_ipv4_hdr* ipv4,
                                           enum mtl_session_port session_port) {
  uint port = s->port_maps[session_port];
  struct mtl_main_impl* impl;

  if (!s || !ipv4 || session_port > MTL_SESSION_PORT_MAX) return;

  impl = s->impl;
  port = s->port_maps[session_port];
  if (!mt_if_has_packet_loss_simulation(s->impl)) return;

  uint num_port = impl->user_para.port_packet_loss[port].tx_stream_loss_divider
                      ? impl->user_para.port_packet_loss[port].tx_stream_loss_divider
                      : s->ops.num_port;
  uint loss_id = impl->user_para.port_packet_loss[port].tx_stream_loss_id
                     ? impl->user_para.port_packet_loss[port].tx_stream_loss_id
                     : port;

  if (!num_port || loss_id >= num_port) return;

  uint16_t pkt_idx = s->st20_seq_id + 1;
  if ((pkt_idx % num_port) == loss_id) {
    ipv4->src_addr = rte_cpu_to_be_32(0);
    ipv4->dst_addr = rte_cpu_to_be_32(0);
    ipv4->hdr_checksum = 0;
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }
}
#endif /* MTL_SIMULATE_PACKET_DROPS */

static inline uint64_t tai_from_frame_count(struct st_tx_video_pacing* pacing,
                                            uint64_t frame_count) {
  /* Doubles lose integer precision beyond 2^53 (~9e15), so a plain cast to uint64_t
   * may truncate to a smaller value. Using nextafter(val, INFINITY) ensures we round
   * up to the next representable double before casting, avoiding jumping between tai in
   * neighboring frames. This caused problems when tai was again changed to frame count */
  return nextafter(frame_count * pacing->frame_time, INFINITY);
}

/* transmission start time of the frame */
static inline uint64_t transmission_start_time(struct st_tx_video_pacing* pacing,
                                               uint64_t frame_count) {
  return tai_from_frame_count(pacing, frame_count) + pacing->tr_offset -
         (pacing->vrx * pacing->trs);
}

static inline void pacing_set_mbuf_time_stamp(struct rte_mbuf* mbuf,
                                              struct st_tx_video_pacing* pacing) {
  st_tx_mbuf_set_tsc(mbuf, pacing->tsc_time_cursor);
  st_tx_mbuf_set_ptp(mbuf, pacing->ptp_time_cursor);
}

static inline void pacing_forward_cursor(struct st_tx_video_pacing* pacing) {
  /* pkt forward */
  pacing->tsc_time_cursor += pacing->trs;
  pacing->ptp_time_cursor += pacing->trs;
}

static inline uint64_t tv_rl_bps(struct st_tx_video_session_impl* s) {
  double reactive = 1.0;
  if (s->ops.interlaced && s->ops.height <= 576) {
    reactive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  return (uint64_t)(s->st20_pkt_size * s->st20_total_pkts * 1.0 * s->fps_tm.mul /
                    s->fps_tm.den / reactive);
}

static void tv_notify_frame_done(struct st_tx_video_session_impl* s, uint16_t frame_idx) {
  uint64_t tsc_start = 0;
  struct mtl_main_impl* impl = s->impl;
  bool time_measure = mt_sessions_time_measure(impl);
  if (time_measure) tsc_start = mt_get_tsc(impl);
  if (s->st22_info) {
    struct st22_tx_frame_meta* tx_st22_meta = &s->st20_frames[frame_idx].tx_st22_meta;
    if (s->st22_info->notify_frame_done)
      s->st22_info->notify_frame_done(s->ops.priv, frame_idx, tx_st22_meta);
    MT_USDT_ST22_TX_FRAME_DONE(s->mgr->idx, s->idx, frame_idx,
                               tx_st22_meta->rtp_timestamp);
  } else {
    struct st20_tx_frame_meta* tv_meta = &s->st20_frames[frame_idx].tv_meta;
    if (s->ops.notify_frame_done)
      s->ops.notify_frame_done(s->ops.priv, frame_idx, tv_meta);
    MT_USDT_ST20_TX_FRAME_DONE(s->mgr->idx, s->idx, frame_idx, tv_meta->rtp_timestamp);
  }
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
  }
}

static void tv_frame_free_cb(void* addr, void* opaque) {
  struct st_frame_trans* frame_info = opaque;
  struct st_tx_video_session_impl* s = frame_info->priv;
  int s_idx = s->idx, frame_idx = frame_info->idx;

  if ((addr < frame_info->addr) || (addr >= (frame_info->addr + s->st20_fb_size))) {
    err("%s(%d), addr %p does not belong to frame %d\n", __func__, s_idx, addr,
        frame_idx);
    return;
  }

  int refcnt = rte_atomic32_read(&frame_info->refcnt);
  if (refcnt != 1) {
    warn("%s(%d), frame %d err refcnt %d addr %p\n", __func__, s_idx, frame_idx, refcnt,
         addr);
    return;
  }

  tv_notify_frame_done(s, frame_idx);
  rte_atomic32_dec(&frame_info->refcnt);
  /* clear ext frame info */
  if (frame_info->flags & ST_FT_FLAG_EXT) {
    frame_info->addr = NULL;
    frame_info->iova = 0;
  }

  dbg("%s(%d), succ frame_idx %d\n", __func__, s_idx, frame_idx);
}

static rte_iova_t tv_frame_get_offset_iova(struct st_tx_video_session_impl* s,
                                           struct st_frame_trans* frame_info,
                                           size_t offset) {
  if (frame_info->page_table_len == 0) return frame_info->iova + offset;
  void* addr = RTE_PTR_ADD(frame_info->addr, offset);
  struct st_page_info* page;
  for (uint16_t i = 0; i < frame_info->page_table_len; i++) {
    page = &frame_info->page_table[i];
    if (addr >= page->addr && addr < RTE_PTR_ADD(page->addr, page->len))
      return page->iova + RTE_PTR_DIFF(addr, page->addr);
  }

  err("%s(%d,%d), offset %" PRIu64 " get iova fail\n", __func__, s->idx, frame_info->idx,
      offset);
  return MTL_BAD_IOVA;
}

static int tv_frame_create_page_table(struct st_tx_video_session_impl* s,
                                      struct st_frame_trans* frame_info) {
  struct rte_memseg* mseg = rte_mem_virt2memseg(frame_info->addr, NULL);
  if (mseg == NULL) {
    err("%s(%d,%d), get mseg fail\n", __func__, s->idx, frame_info->idx);
    return -EIO;
  }
  size_t hugepage_sz = mseg->hugepage_sz;
  info("%s(%d,%d), hugepage size %" PRIu64 "\n", __func__, s->idx, frame_info->idx,
       hugepage_sz);

  /* calculate num hugepages */
  uint16_t num_pages =
      RTE_PTR_DIFF(RTE_PTR_ALIGN(frame_info->addr + s->st20_fb_size, hugepage_sz),
                   RTE_PTR_ALIGN_FLOOR(frame_info->addr, hugepage_sz)) /
      hugepage_sz;
  int soc_id = s->socket_id;
  struct st_page_info* pages = mt_rte_zmalloc_socket(sizeof(*pages) * num_pages, soc_id);
  if (pages == NULL) {
    err("%s(%d,%d), pages info malloc fail\n", __func__, s->idx, frame_info->idx);
    return -ENOMEM;
  }

  /* get IOVA start of each page */
  void* addr = frame_info->addr;
  for (uint16_t i = 0; i < num_pages; i++) {
    /* touch the page before getting its IOVA */
    *(volatile char*)addr = 0;
    pages[i].iova = rte_mem_virt2iova(addr);
    pages[i].addr = addr;
    void* next_addr = RTE_PTR_ALIGN(RTE_PTR_ADD(addr, 1), hugepage_sz);
    pages[i].len = RTE_PTR_DIFF(next_addr, addr);
    addr = next_addr;
    info("%s(%d,%d), seg %u, va %p, iova 0x%" PRIx64 ", len %" PRIu64 "\n", __func__,
         s->idx, frame_info->idx, i, pages[i].addr, pages[i].iova, pages[i].len);
  }
  frame_info->page_table = pages;
  frame_info->page_table_len = num_pages;

  return 0;
}

static inline bool tv_frame_payload_cross_page(struct st_tx_video_session_impl* s,
                                               struct st_frame_trans* frame_info,
                                               size_t offset, size_t len) {
  if (frame_info->page_table_len == 0) return false;
  return ((tv_frame_get_offset_iova(s, frame_info, offset + len - 1) -
           tv_frame_get_offset_iova(s, frame_info, offset)) != len - 1);
}

static int tv_alloc_frames(struct mtl_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  struct st_frame_trans* frame_info;
  struct st22_tx_video_info* st22_info = s->st22_info;

  s->st20_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st20_frames) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames) {
    err("%s(%d), st20_frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame_info = &s->st20_frames[i];
    rte_atomic32_set(&frame_info->refcnt, 0);
    frame_info->idx = i;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame_info = &s->st20_frames[i];

    frame_info->sh_info.free_cb = tv_frame_free_cb;
    frame_info->sh_info.fcb_opaque = frame_info;
    rte_mbuf_ext_refcnt_set(&frame_info->sh_info, 0);

    if (s->ops.flags & ST20_TX_FLAG_EXT_FRAME) {
      frame_info->iova = 0;
      frame_info->addr = NULL;
      frame_info->flags = ST_FT_FLAG_EXT;
      info("%s(%d), use external framebuffer, skip allocation\n", __func__, idx);
    } else {
      void* frame = mt_rte_zmalloc_socket(s->st20_fb_size, soc_id);
      if (!frame) {
        err("%s(%d), rte_malloc %" PRIu64 " fail at %d\n", __func__, idx, s->st20_fb_size,
            i);
        return -ENOMEM;
      }
      if (st22_info && s->st22_box_hdr_length) { /* copy boxes */
        /* Validate bounds to prevent buffer overrun */
        size_t max_copy_len =
            RTE_MIN(s->st22_box_hdr_length, sizeof(st22_info->st22_boxes));
        max_copy_len = RTE_MIN(max_copy_len, s->st20_fb_size);
        if (max_copy_len != s->st22_box_hdr_length) {
          warn("%s(%d), st22_box_hdr_length %u exceeds bounds, clamping to %zu\n",
               __func__, idx, s->st22_box_hdr_length, max_copy_len);
        }
        mtl_memcpy(frame, &st22_info->st22_boxes, max_copy_len);
      }
      frame_info->iova = rte_mem_virt2iova(frame);
      frame_info->addr = frame;
      frame_info->flags = ST_FT_FLAG_RTE_MALLOC;
      if (impl->iova_mode == RTE_IOVA_PA && !s->tx_no_chain)
        tv_frame_create_page_table(s, frame_info);
    }
    frame_info->priv = s;

    /* init user meta */
    frame_info->user_meta_buffer_size =
        impl->pkt_udp_suggest_max_size - sizeof(struct st20_rfc4175_rtp_hdr);
    frame_info->user_meta =
        mt_rte_zmalloc_socket(frame_info->user_meta_buffer_size, soc_id);
    if (!frame_info->user_meta) {
      err("%s(%d), user_meta malloc %" PRIu64 " fail at %d\n", __func__, idx,
          frame_info->user_meta_buffer_size, i);
      return -ENOMEM;
    }
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tv_free_frames(struct st_tx_video_session_impl* s) {
  if (s->st20_frames) {
    struct st_frame_trans* frame;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      st_frame_trans_uinit(frame, NULL);
    }

    mt_rte_free(s->st20_frames);
    s->st20_frames = NULL;
    s->st20_frames_cnt = 0; /* mark frames unavailable after free */
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tv_poll_vsync(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s) {
  struct st_vsync_info* vsync = &s->vsync;
  uint64_t cur_tsc = mt_get_tsc(impl);

  if (cur_tsc > vsync->next_epoch_tsc) {
    uint64_t tsc_delta = cur_tsc - vsync->next_epoch_tsc;
    dbg("%s(%d), vsync with epochs %" PRIu64 "\n", __func__, s->idx, vsync->meta.epoch);
    s->ops.notify_event(s->ops.priv, ST_EVENT_VSYNC, &vsync->meta);
    st_vsync_calculate(impl, vsync); /* set next vsync */
    /* check tsc delta for status */
    if (tsc_delta > NS_PER_MS) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_vsync_mismatch);
    }
  }

  return 0;
}

static int uint64_t_cmp(const void* a, const void* b) {
  const uint64_t* ai = a;
  const uint64_t* bi = b;

  if (*ai < *bi) {
    return -1;
  } else if (*ai > *bi) {
    return 1;
  }
  return 0;
}

static int tv_train_pacing(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                           enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct rte_mbuf* pad;

  int idx = s->idx;
  struct mt_txq_entry* queue = s->queue[s_port];
  int pad_pkts, ret;
  int up_trim = 5;
  int low_trim = up_trim + 1;
  int loop_frame = 60 * 1 + up_trim + low_trim; /* the frames to be trained */
  uint64_t frame_times_ns[loop_frame];
  float pad_interval;
  uint64_t rl_bps = tv_rl_bps(s);
  uint64_t train_start_time, train_end_time;
  double measured_bps;
  uint64_t bps_to_set;

  uint16_t resolved = s->ops.pad_interval;
  if (resolved) {
    s->pacing.pad_interval = resolved;
    info("%s(%d), user customized pad_interval %u\n", __func__, idx, resolved);
    return 0;
  }
  if ((s->ops.flags & ST20_TX_FLAG_ENABLE_STATIC_PAD_P)) {
    resolved = st20_pacing_static_profiling(impl, s, s_port);
    if (resolved) {
      s->pacing.pad_interval = resolved;
      info("%s(%d), user static pad_interval %u\n", __func__, idx, resolved);
      return 0;
    }
  }

  ret = mt_pacing_train_pad_result_search(impl, port, rl_bps, &pad_interval);
  if (ret >= 0) {
    s->pacing.pad_interval = pad_interval;
    info("%s(%d), use pre-train pad_interval %f\n", __func__, idx, pad_interval);
    return 0;
  }

  /* wait ptp and tsc calibrate done */
  ret = mt_ptp_wait_stable(impl, MTL_PORT_P, 60 * 3 * MS_PER_S);
  if (ret < 0) return ret;
  mt_wait_tsc_stable(impl);

  train_start_time = mt_get_tsc(impl);

  /* warm-up stage to consume all nix tx buf */
  pad_pkts = mt_if_nb_tx_desc(impl, port) * 1;
  pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  for (int i = 0; i < pad_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    mt_txq_burst_busy(queue, &pad, 1, 10);
  }

  int total = s->st20_total_pkts;
  int remain = 32 - (total % 32);

  /* training stage */
  for (int loop = 0; loop < loop_frame; loop++) {
    uint64_t start = mt_get_ptp_time(impl, MTL_PORT_P);
    for (int i = 0; i < total; i++) {
      enum st20_packet_type type;

      if ((s->ops.type == ST20_TYPE_RTP_LEVEL) ||
          (s->s_type == MT_ST22_HANDLE_TX_VIDEO) ||
          (s->ops.packing == ST20_PACKING_GPM_SL)) {
        type = ST20_PKT_TYPE_NORMAL;
      } else { /* frame type */
        uint32_t offset = s->st20_pkt_len * i;
        uint16_t line1_number = offset / s->st20_bytes_in_line;
        /* last pkt should be treated as normal pkt also */
        if ((offset + s->st20_pkt_len) < (line1_number + 1) * s->st20_bytes_in_line) {
          type = ST20_PKT_TYPE_NORMAL;
        } else {
          type = ST20_PKT_TYPE_EXTRA;
        }
      }

      pad = s->pad[s_port][type];
      rte_mbuf_refcnt_update(pad, 1);
      mt_txq_burst_busy(queue, &pad, 1, 10);
    }
    pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
    for (int i = 0; i < remain; i++) {
      rte_mbuf_refcnt_update(pad, 1);
      mt_txq_burst_busy(queue, &pad, 1, 10);
    }
    uint64_t end = mt_get_ptp_time(impl, MTL_PORT_P);
    double time = ((double)end - start) * total / (total + remain);
    frame_times_ns[loop] = time;
  }

  for (int loop = 0; loop < loop_frame; loop++) {
    dbg("%s(%d), frame_time_ns %" PRIu64 "\n", __func__, idx, frame_times_ns[loop]);
  }
  qsort(frame_times_ns, loop_frame, sizeof(uint64_t), uint64_t_cmp);
  for (int loop = 0; loop < loop_frame; loop++) {
    dbg("%s(%d), sorted frame_time_ns %" PRIu64 "\n", __func__, idx,
        frame_times_ns[loop]);
  }
  uint64_t frame_times_ns_sum = 0;
  int entry_in_sum = 0;
  for (int i = low_trim; i < (loop_frame - up_trim); i++) {
    frame_times_ns_sum += frame_times_ns[i];
    entry_in_sum++;
  }
  double frame_avg_time_sec = (double)frame_times_ns_sum / entry_in_sum / NS_PER_S;
  double pkts_per_sec = s->st20_total_pkts / frame_avg_time_sec;

  /* parse the pad interval */
  double pkts_per_frame = pkts_per_sec * s->fps_tm.den / s->fps_tm.mul;
  /* adjust as tr offset */
  double reactive = (1080.0 / 1125.0);
  if (s->ops.interlaced && s->ops.height <= 576) {
    reactive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  pkts_per_frame = pkts_per_frame * reactive;
  measured_bps = s->st20_pkt_size * pkts_per_sec * reactive;
  pad_interval = (float)s->st20_total_pkts / (pkts_per_frame - s->st20_total_pkts);

  /* Padding is effective only when the actual throughput slightly exceeds the expected
   * value. The pad interval decreases as the measured throughput surpasses the expected
   * rate. If the difference is too significant, it indicates an issue. A minimum padding
   * value of 32 is chosen as a reasonable threshold. */
  if (measured_bps > rl_bps && pad_interval > 32) {
    s->pacing.pad_interval = pad_interval;
    mt_pacing_train_pad_result_add(impl, port, rl_bps, pad_interval);
    train_end_time = mt_get_tsc(impl);
    info("%s(%d,%d), trained pad_interval %f pkts_per_frame %f with time %fs\n", __func__,
         idx, s_port, pad_interval, pkts_per_frame,
         (double)(train_end_time - train_start_time) / NS_PER_S);
    return 0;
  }
  if (measured_bps < rl_bps) {
    info("%s(%d), measured bps %" PRIu64 " is lower than set bps %" PRIu64 "\n", __func__,
         idx, (uint64_t)measured_bps, rl_bps);
  } else {
    info("%s(%d), too small pad_interval %f pkts_per_frame %f, st20_total_pkts %d\n",
         __func__, idx, pad_interval, pkts_per_frame, s->st20_total_pkts);
  }

  if (!mt_pacing_train_bps_result_search(impl, port, rl_bps, &bps_to_set)) {
    err("%s(%d), measured speed is out of range on already trained bps\n", __func__, idx);
    return -EINVAL;
  }

/* Slightly increase the target bitrate to compensate for measurement inaccuracies,
 * rounding errors, and system overhead. This helps ensure the actual transmission bitrate
 * meets or exceeds the required rate
 */
#define INCREASE_BPS_FACTOR 1.005
  bps_to_set = INCREASE_BPS_FACTOR * (rl_bps * rl_bps) / measured_bps;
  info("%s(%d), Retrain pacing with bps changed to %" PRIu64 "\n", __func__, idx,
       bps_to_set);
  mt_pacing_train_bps_result_add(impl, port, rl_bps, bps_to_set);
  mt_txq_set_tx_bps(queue, bps_to_set);
  ret = tv_train_pacing(impl, s, s_port);
  return ret;
}

static int tv_init_pacing(struct mtl_main_impl* impl,
                          struct st_tx_video_session_impl* s) {
  int idx = s->idx;
  struct st_tx_video_pacing* pacing = &s->pacing;

  double frame_time = (double)1000000000.0 * s->fps_tm.den / s->fps_tm.mul;
  pacing->frame_time = frame_time;
  pacing->frame_time_sampling =
      (double)(s->fps_tm.sampling_clock_rate) * s->fps_tm.den / s->fps_tm.mul;
  pacing->reactive = 1080.0 / 1125.0;

  /* calculate tr offset */
  pacing->tr_offset =
      s->ops.height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 750.0);
  if (s->ops.interlaced) {
    if (s->ops.height <= 576)
      pacing->reactive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
    if (s->ops.height == 480) {
      pacing->tr_offset = frame_time * (20.0 / 525.0) * 2;
    } else if (s->ops.height == 576) {
      pacing->tr_offset = frame_time * (26.0 / 625.0) * 2;
    } else {
      pacing->tr_offset = frame_time * (22.0 / 1125.0) * 2;
    }
  }
  pacing->trs = frame_time * pacing->reactive / s->st20_total_pkts;
  pacing->frame_idle_time =
      frame_time - pacing->tr_offset - frame_time * pacing->reactive;
  dbg("%s[%02d], frame_idle_time %f\n", __func__, idx, pacing->frame_idle_time);
  if (pacing->frame_idle_time < 0) {
    warn("%s[%02d], error frame_idle_time %f\n", __func__, idx, pacing->frame_idle_time);
    pacing->frame_idle_time = 0;
  }
  pacing->max_onward_epochs = (double)NS_PER_S / frame_time; /* 1s */
  dbg("%s[%02d], max_onward_epochs %u\n", __func__, idx, pacing->max_onward_epochs);
  /* default VRX compensate as rl accuracy, update later in tv_train_pacing */
  pacing->pad_interval = s->st20_total_pkts;

  int num_port = s->ops.num_port;
  int ret;

  for (int i = 0; i < num_port; i++) {
    if (s->pacing_way[i] == ST21_TX_PACING_WAY_RL) {
      ret = tv_train_pacing(impl, s, i);
      if (ret < 0) {
        /* fallback to tsc pacing */
        s->pacing_way[i] = ST21_TX_PACING_WAY_TSC;
      }
    }
  }

  if (num_port > 1) {
    if (s->pacing_way[MTL_SESSION_PORT_P] != s->pacing_way[MTL_SESSION_PORT_R]) {
      /* currently not support two different pacing? */
      warn("%s(%d), different pacing detected, all set to tsc\n", __func__, idx);
      s->pacing_way[MTL_SESSION_PORT_P] = ST21_TX_PACING_WAY_TSC;
      s->pacing_way[MTL_SESSION_PORT_R] = ST21_TX_PACING_WAY_TSC;
    }
  }

  uint32_t pkts_in_tr_offset = pacing->tr_offset / pacing->trs;
  /* calculate warmup pkts for rl */
  uint32_t warm_pkts = 0;
  if (s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_RL) {
    /* 80 percent tr offset time as warmup pkts for rl */
    warm_pkts = pkts_in_tr_offset;
    warm_pkts = warm_pkts * 8 / 10;
    warm_pkts = RTE_MIN(warm_pkts, 128); /* limit to 128 pkts */
  }
  pacing->warm_pkts = warm_pkts;

  /* calculate vrx pkts */
  pacing->vrx = s->st21_vrx_narrow;
  if (s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_RL) {
    pacing->vrx -= 2; /* VRX compensate to rl burst(max_burst_size=2048) */
    pacing->vrx -= 2; /* leave VRX space for deviation */
    if (s->ops.height <= 576) {
      pacing->warm_pkts = 8; /* fix me */
      pacing->vrx = s->st21_vrx_narrow;
    }
  } else if (s->pacing_way[MTL_SESSION_PORT_P] == ST21_TX_PACING_WAY_TSC_NARROW) {
    /* tsc narrow use single bulk for better accuracy */
    s->bulk = 1;
  } else {
    pacing->vrx -= (s->bulk - 1); /* compensate for bulk */
  }

  if (s->s_type == MT_ST22_HANDLE_TX_VIDEO) {
    /* not sure the pacing for st22, none now */
    pacing->vrx = 0;
    pacing->warm_pkts = 0;
  }
  if (s->ops.start_vrx) {
    if (s->ops.start_vrx >= pkts_in_tr_offset) {
      err("%s[%02d], use start_vrx %u larger than pkts in tr offset %u\n", __func__, idx,
          s->ops.start_vrx, pkts_in_tr_offset);
    } else {
      info("%s[%02d], use start_vrx %u from user\n", __func__, idx, s->ops.start_vrx);
      pacing->vrx = s->ops.start_vrx;
    }
  } else if (s->ops.pacing == ST21_PACING_WIDE) {
    uint32_t wide_vrx = pkts_in_tr_offset * 8 / 10;
    uint32_t max_vrx = s->st21_vrx_wide * 8 / 10;
    pacing->vrx = RTE_MIN(max_vrx, wide_vrx);
    pacing->warm_pkts = 0; /* no need warmup for wide */
    info("%s[%02d], wide pacing\n", __func__, idx);
  }
  info("%s[%02d], trs %f trOffset %f vrx %u warm_pkts %u frame time %fms fps %f\n",
       __func__, idx, pacing->trs, pacing->tr_offset, pacing->vrx, pacing->warm_pkts,
       pacing->frame_time / NS_PER_MS, st_frame_rate(s->ops.fps));

  /* resolve pacing tasklet */
  for (int i = 0; i < num_port; i++) {
    ret = st_video_resolve_pacing_tasklet(s, i);
    if (ret < 0) return ret;
  }

  return 0;
}

static int tv_init_pacing_epoch(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  struct st_tx_video_pacing* pacing = &s->pacing;
  pacing->cur_epochs = ptp_time / pacing->frame_time;
  return 0;
}

static void validate_user_timestamp(struct st_tx_video_session_impl* s,
                                    uint64_t requested_frame_count,
                                    uint64_t current_frame_count) {
  if (requested_frame_count < current_frame_count) {
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
    dbg("%s(%d), user requested transmission time in the past, required_tai %" PRIu64
        ", cur_tai %" PRIu64 "\n",
        __func__, s->idx, requested_frame_count, current_frame_count);
  } else if (requested_frame_count >
             current_frame_count + (NS_PER_S / s->pacing.frame_time)) {
    dbg("%s(%d), requested frame count %" PRIu64
        " too far in the future, current frame count %" PRIu64 "\n",
        __func__, s->idx, requested_frame_count, current_frame_count);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_error_user_timestamp);
  }
}

static inline uint64_t calc_frame_count_since_epoch(struct st_tx_video_session_impl* s,
                                                    uint64_t cur_tai,
                                                    uint64_t required_tai) {
  uint64_t frame_count_tai = cur_tai / s->pacing.frame_time;
  uint64_t next_free_frame_slot = s->pacing.cur_epochs + 1;
  uint64_t frame_count;

  if (required_tai) {
    frame_count = (required_tai + s->pacing.frame_time / 2) / s->pacing.frame_time;
    validate_user_timestamp(s, frame_count, frame_count_tai);
  }

  if (frame_count_tai <= next_free_frame_slot) {
    /* There is time buffer until the next available frame time window */
    if (next_free_frame_slot - frame_count_tai > s->pacing.max_onward_epochs) {
      /* current time is out of onward range, just note this and still move to next free
       * slot */
      dbg("%s(%d), onward range exceeded, next_free_frame_slot %" PRIu64
          ", frame_count_tai %" PRIu64 "\n",
          __func__, s->idx, next_free_frame_slot, frame_count_tai);
      ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_onward,
                          (next_free_frame_slot - frame_count_tai));
    }

    if (!required_tai) {
      frame_count = next_free_frame_slot;
    }

  } else {
    dbg("%s(%d), frame is late, frame_count_tai %" PRIu64 " next_free_frame_slot %" PRIu64
        "\n",
        __func__, s->idx, frame_count_tai, next_free_frame_slot);
    ST_SESSION_STAT_ADD(s, port_user_stats.common, stat_epoch_drop,
                        (frame_count_tai - next_free_frame_slot));

    if (s->ops.notify_frame_late) {
      s->ops.notify_frame_late(s->ops.priv, frame_count_tai - next_free_frame_slot);
    }

    frame_count = frame_count_tai;
  }

  return frame_count;
}

static int tv_sync_pacing(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                          uint64_t required_tai) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t cur_tai = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t cur_tsc = mt_get_tsc(impl);
  uint64_t start_time_tai;
  int64_t time_to_tx_ns;

  pacing->cur_epochs = calc_frame_count_since_epoch(s, cur_tai, required_tai);

  if (s->ops.flags & ST20_TX_FLAG_EXACT_USER_PACING) {
    start_time_tai = required_tai;
  } else {
    start_time_tai = transmission_start_time(pacing, pacing->cur_epochs);
  }
  time_to_tx_ns = start_time_tai - cur_tai;

  if (time_to_tx_ns < 0) {
    /* should never happen, but it does. TODO: check why */
    dbg("%s(%d), negative time_to_tx_ns detected: %ld ns. Current PTP time: %" PRIu64
        "\n",
        __func__, s->idx, time_to_tx_ns, cur_tai);
    time_to_tx_ns = 0;
  }

  /* tsc_time_cursor is important as it determines when first packet of the frame will be
   * send */
  pacing->tsc_time_cursor = cur_tsc + time_to_tx_ns;

  pacing->tsc_time_frame_start = pacing->tsc_time_cursor;
  pacing->ptp_time_cursor = start_time_tai;

  return 0;
}

static int tv_sync_pacing_st22(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s, uint64_t required_tai,
                               int pkts_in_frame) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  /* reset trs */
  pacing->trs = pacing->frame_time * pacing->reactive / pkts_in_frame;
  dbg("%s(%d), trs %f\n", __func__, s->idx, pacing->trs);
  return tv_sync_pacing(impl, s, required_tai);
}

static void tv_update_rtp_time_stamp(struct st_tx_video_session_impl* s,
                                     enum st10_timestamp_fmt tfmt, uint64_t timestamp) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t delta_ns = (uint64_t)s->ops.rtp_timestamp_delta_us * NS_PER_US;

  if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
    enum st10_timestamp_fmt tfmt_for_clk = tfmt;
    timestamp = timestamp + delta_ns;
    pacing->rtp_time_stamp =
        st10_get_media_clk(tfmt_for_clk, timestamp, s->fps_tm.sampling_clock_rate);
  } else {
    uint64_t tai_for_rtp_ts;
    if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
      tai_for_rtp_ts = tai_from_frame_count(pacing, pacing->cur_epochs);
    } else {
      tai_for_rtp_ts = pacing->ptp_time_cursor;
    }
    tai_for_rtp_ts += delta_ns;
    pacing->rtp_time_stamp =
        st10_tai_to_media_clk(tai_for_rtp_ts, s->fps_tm.sampling_clock_rate);
  }
  dbg("%s(%d), rtp time stamp %u\n", __func__, s->idx, pacing->rtp_time_stamp);
}

static int tv_init_next_meta(struct st_tx_video_session_impl* s,
                             struct st20_tx_frame_meta* meta) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  struct st20_tx_ops* ops = &s->ops;

  memset(meta, 0, sizeof(*meta));
  meta->width = ops->width;
  meta->height = ops->height;
  meta->fps = ops->fps;
  meta->fmt = ops->fmt;
  if (ops->interlaced) { /* init second_field but user still can customize also */
    meta->second_field = s->second_field;
  }
  /* point to next epoch */
  meta->epoch = pacing->cur_epochs + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai_from_frame_count(pacing, meta->epoch);
  return 0;
}

static int tv_init_st22_next_meta(struct st_tx_video_session_impl* s,
                                  struct st22_tx_frame_meta* meta) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  struct st20_tx_ops* ops = &s->ops;

  memset(meta, 0, sizeof(*meta));
  meta->width = ops->width;
  meta->height = ops->height;
  meta->fps = ops->fps;
  meta->codestream_size = s->st22_codestream_size;
  if (ops->interlaced) { /* init second_field but user still can customize also */
    meta->second_field = s->second_field;
  }
  /* point to next epoch */
  meta->epoch = pacing->cur_epochs + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai_from_frame_count(pacing, meta->epoch);
  return 0;
}

static int tv_init_st22_boxes(struct st_tx_video_session_impl* s) {
  struct st22_tx_video_info* st22_info = s->st22_info;
  struct st22_jpvs* jpvs = &st22_info->st22_boxes.jpvs;
  uint32_t lbox = sizeof(*jpvs);
  jpvs->lbox = htonl(lbox);
  jpvs->tbox[0] = 'j';
  jpvs->tbox[1] = 'p';
  jpvs->tbox[2] = 'v';
  jpvs->tbox[3] = 's';

  struct st22_jpvi* jpvi = &jpvs->jpvi;
  lbox = sizeof(*jpvi);
  jpvi->lbox = htonl(lbox);
  jpvi->tbox[0] = 'j';
  jpvi->tbox[1] = 'p';
  jpvi->tbox[2] = 'v';
  jpvi->tbox[3] = 'i';
  uint32_t brat_m =
      8 * s->st22_codestream_size * s->fps_tm.mul / s->fps_tm.den / 1024 / 1024;
  jpvi->brat = htonl(brat_m);
  /* hardcode to 59.94 now */
  uint32_t frat = (1 << 24) | 60;
  jpvi->frat = htonl(frat);
  /* hardcode to 10bit ycbcr 422 */
  uint16_t schar = (0x1 << 15) | ((10 - 1) << 4);
  jpvi->schar = htons(schar);
  /* zero now */
  jpvi->tcod = htonl(0x0);

  struct st22_jxpl* jxpl = &jpvs->jxpl;
  lbox = sizeof(*jxpl);
  jxpl->lbox = htonl(lbox);
  jxpl->tbox[0] = 'j';
  jxpl->tbox[1] = 'x';
  jxpl->tbox[2] = 'p';
  jxpl->tbox[3] = 'l';
  /* Main 422.10 */
  jxpl->ppih = htons(0x3540);
  /* 4k-1 full */
  jxpl->plev = htons(0x2080);

  struct st22_colr* colr = &st22_info->st22_boxes.colr;
  lbox = sizeof(*colr);
  colr->lbox = htonl(lbox);
  colr->tbox[0] = 'c';
  colr->tbox[1] = 'o';
  colr->tbox[2] = 'l';
  colr->tbox[3] = 'r';
  colr->meth = 0x05; /* must 5 */
  /*  ITU-R BT.709-6 */
  colr->methdat[1] = 0x01;
  colr->methdat[3] = 0x01;
  colr->methdat[5] = 0x01;
  colr->methdat[6] = 0x80;

  return 0;
}

static int tv_init_hdr(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                       enum mtl_session_port s_port) {
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  int ret;
  struct st_rfc4175_video_hdr* hdr = &s->s_hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st20_rfc4175_rtp_hdr* rtp = &hdr->rtp;
  struct st20_tx_ops* ops = &s->ops;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = mt_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);

  /* ether hdr */
  if ((s_port == MTL_SESSION_PORT_P) && (ops->flags & ST20_TX_FLAG_USER_P_MAC)) {
    rte_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == MTL_SESSION_PORT_R) && (ops->flags & ST20_TX_FLAG_USER_R_MAC)) {
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
    err("%s(%d), macaddr get fail %d for port %d\n", __func__, idx, ret, s_port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ipv4 hdr */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0;
  ipv4->packet_id = 0; /* always 0 when DONT_FRAGMENT set */
  ipv4->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ipv4->next_proto_id = IPPROTO_UDP;
  mtl_memcpy(&ipv4->src_addr, sip, MTL_IP_ADDR_LEN);
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st20_src_port[s_port]);
  udp->dst_port = htons(s->st20_dst_port[s_port]);
  udp->dgram_cksum = 0;

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = ST_RVRTP_VERSION_2;
  rtp->base.marker = 0;
  rtp->base.payload_type =
      ops->payload_type ? ops->payload_type : ST_RVRTP_PAYLOAD_TYPE_RAW_VIDEO;
  uint32_t ssrc = ops->ssrc ? ops->ssrc : s->idx + 0x123450;
  rtp->base.ssrc = htonl(ssrc);
  rtp->row_length = htons(s->st20_pkt_len);
  rtp->row_number = 0;
  rtp->row_offset = 0;

  /* st22_rfc9134_rtp_hdr if st22 frame mode */
  if (s->st22_info) {
    struct st22_rfc9134_rtp_hdr* st22_hdr = &s->st22_info->rtp_hdr[s_port];
    /* copy base */
    mtl_memcpy(&st22_hdr->base, &rtp->base, sizeof(st22_hdr->base));
    st22_hdr->trans_order = 1; /* packets sent sequentially */
    st22_hdr->kmode = 0;       /* codestream packetization mode */
    st22_hdr->f_counter_hi = 0;
    st22_hdr->f_counter_lo = 0;
  }

  info("%s(%d,%d), ip %u.%u.%u.%u port %u:%u\n", __func__, idx, s_port, dip[0], dip[1],
       dip[2], dip[3], s->st20_src_port[s_port], s->st20_dst_port[s_port]);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx, ssrc %u\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5], ssrc);
  return 0;
}

static int tv_uinit_rtcp(struct st_tx_video_session_impl* s) {
  for (int i = 0; i < s->ops.num_port; i++) {
    if (s->rtcp_tx[i]) {
      mt_rtcp_tx_free(s->rtcp_tx[i]);
      s->rtcp_tx[i] = NULL;
    }
    if (s->rtcp_q[i]) {
      mt_rxq_put(s->rtcp_q[i]);
      s->rtcp_q[i] = NULL;
    }
  }

  return 0;
}

static int tv_init_rtcp(struct mtl_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                        struct st_tx_video_session_impl* s) {
  int idx = s->idx;
  int mgr_idx = mgr->idx;
  struct st20_tx_ops* ops = &s->ops;
  int num_port = ops->num_port;
  struct mt_rxq_flow flow;

  for (int i = 0; i < num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    struct mt_rtcp_tx_ops rtcp_ops;
    memset(&rtcp_ops, 0, sizeof(rtcp_ops));
    rtcp_ops.port = port;
    char name[MT_RTCP_MAX_NAME_LEN];
    snprintf(name, sizeof(name), ST_TX_VIDEO_PREFIX "M%dS%dP%d", mgr_idx, idx, i);
    rtcp_ops.name = name;
    struct mt_udp_hdr hdr;
    mtl_memcpy(&hdr, &s->s_hdr[i], sizeof(hdr));
    hdr.udp.dst_port++;
    rtcp_ops.udp_hdr = &hdr;
    if (!ops->rtcp.buffer_size) ops->rtcp.buffer_size = ST_TX_VIDEO_RTCP_RING_SIZE;
    rtcp_ops.buffer_size = ops->rtcp.buffer_size;
    if (s->st22_info)
      rtcp_ops.payload_format = MT_RTP_PAYLOAD_FORMAT_RFC9134;
    else
      rtcp_ops.payload_format = MT_RTP_PAYLOAD_FORMAT_RFC4175;
    s->rtcp_tx[i] = mt_rtcp_tx_create(impl, &rtcp_ops);
    if (!s->rtcp_tx[i]) {
      err("%s(%d,%d), mt_rtcp_tx_create fail on port %d\n", __func__, mgr_idx, idx, i);
      tv_uinit_rtcp(s);
      return -EIO;
    }
    /* create flow to receive rtcp nack */
    memset(&flow, 0, sizeof(flow));
    flow.flags = MT_RXQ_FLOW_F_NO_IP | MT_RXQ_FLOW_F_FORCE_CNI;
    flow.dst_port = s->st20_dst_port[i] + 1;
    s->rtcp_q[i] = mt_rxq_get(impl, port, &flow);
    if (!s->rtcp_q[i]) {
      err("%s(%d,%d), mt_rxq_get fail on port %d\n", __func__, mgr_idx, idx, i);
      tv_uinit_rtcp(s);
      return -EIO;
    }
  }

  return 0;
}

static int tv_build_st20_redundant(struct st_tx_video_session_impl* s,
                                   struct rte_mbuf* pkt_r,
                                   const struct rte_mbuf* pkt_base) {
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt_r, struct mt_udp_hdr*);
  struct mt_udp_hdr* hdr_base = rte_pktmbuf_mtod(pkt_base, struct mt_udp_hdr*);
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;

  /* update the hdr: eth, ip, udp */
  rte_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));
  mt_mbuf_init_ipv4(pkt_r);

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_r->data_len;
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  udp->dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  /* copy rtp and payload, assume it's only one segment  */
  size_t hdr_sz = sizeof(*hdr_base);
  void* pd_base = rte_pktmbuf_mtod_offset(pkt_base, void*, hdr_sz);
  void* pd_r = rte_pktmbuf_mtod_offset(pkt_r, void*, hdr_sz);
  size_t pd_len = pkt_base->pkt_len - hdr_sz;
  rte_memcpy(pd_r, pd_base, pd_len);

  return 0;
}

static int tv_build_st20(struct st_tx_video_session_impl* s, struct rte_mbuf* pkt) {
  struct st_rfc4175_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  struct st20_tx_ops* ops = &s->ops;
  uint32_t offset;
  uint16_t line1_number, line1_offset;
  uint16_t line1_length = 0, line2_length = 0;
  bool single_line = (ops->packing == ST20_PACKING_GPM_SL);
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the basic hdrs: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_P], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  /* calculate payload header */
  if (single_line) {
    line1_number = s->st20_pkt_idx / s->st20_pkts_in_line;
    int pixel_in_pkt = s->st20_pkt_len / s->st20_pg.size * s->st20_pg.coverage;
    line1_offset = pixel_in_pkt * (s->st20_pkt_idx % s->st20_pkts_in_line);
    offset = line1_number * (uint32_t)s->st20_linesize +
             line1_offset / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->st20_pkt_len * s->st20_pkt_idx;
    line1_number = offset / s->st20_bytes_in_line;
    line1_offset =
        (offset % s->st20_bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->st20_pkt_len > (line1_number + 1) * s->st20_bytes_in_line) &&
        (offset + s->st20_pkt_len < s->st20_frame_size))
      e_rtp =
          rte_pktmbuf_mtod_offset(pkt, struct st20_rfc4175_extra_rtp_hdr*, sizeof(*hdr));
  }

  /* update rtp hdr */
  if (s->st20_pkt_idx >= (s->st20_total_pkts - 1)) rtp->base.marker = 1;
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->st20_seq_id >> 16));
  s->st20_seq_id++;
  uint16_t field = frame_info->tv_meta.second_field ? ST20_SECOND_FIELD : 0x0000;
  rtp->row_number = htons(line1_number | field);
  rtp->row_offset = htons(line1_offset);
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  uint32_t temp =
      single_line ? ((ops->width - line1_offset) / s->st20_pg.coverage * s->st20_pg.size)
                  : (s->st20_frame_size - offset);
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, temp);
  rtp->row_length = htons(left_len);

  if (e_rtp) {
    line1_length = (line1_number + 1) * s->st20_bytes_in_line - offset;
    line2_length = s->st20_pkt_len - line1_length;
    rtp->row_length = htons(line1_length);
    e_rtp->row_length = htons(line2_length);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons((line1_number + 1) | field);
    rtp->row_offset = htons(line1_offset | ST20_SRD_OFFSET_CONTINUATION);
  }

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);

  if (!single_line && s->st20_linesize > s->st20_bytes_in_line)
    /* update offset with line padding for copying */
    offset = offset % s->st20_bytes_in_line + line1_number * s->st20_linesize;
  /* copy payload */
  void* payload = NULL;
  if (e_rtp)
    payload = &e_rtp[1];
  else
    payload = (void*)((uint8_t*)rtp + sizeof(*rtp));
  if (e_rtp && s->st20_linesize > s->st20_bytes_in_line) {
    /* cross lines with padding case */
    mtl_memcpy(payload, frame_info->addr + offset, line1_length);
    mtl_memcpy(payload + line1_length,
               frame_info->addr + s->st20_linesize * (line1_number + 1), line2_length);
  } else {
    mtl_memcpy(payload, frame_info->addr + offset, left_len);
  }
  pkt->data_len = sizeof(struct st_rfc4175_video_hdr) + left_len;
  if (e_rtp) pkt->data_len += sizeof(*e_rtp);
  pkt->pkt_len = pkt->data_len;

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st20_chain(struct st_tx_video_session_impl* s, struct rte_mbuf* pkt,
                               struct rte_mbuf* pkt_chain) {
  struct st_rfc4175_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  struct st20_tx_ops* ops = &s->ops;
  uint32_t offset;
  uint16_t line1_number, line1_offset;
  uint16_t line1_length = 0, line2_length = 0;
  bool single_line = (ops->packing == ST20_PACKING_GPM_SL);
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_P], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  if (single_line) {
    line1_number = s->st20_pkt_idx / s->st20_pkts_in_line;
    int pixel_in_pkt = s->st20_pkt_len / s->st20_pg.size * s->st20_pg.coverage;
    line1_offset = pixel_in_pkt * (s->st20_pkt_idx % s->st20_pkts_in_line);
    offset = line1_number * (uint32_t)s->st20_linesize +
             line1_offset / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->st20_pkt_len * s->st20_pkt_idx;
    line1_number = offset / s->st20_bytes_in_line;
    line1_offset =
        (offset % s->st20_bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->st20_pkt_len > (line1_number + 1) * s->st20_bytes_in_line) &&
        (offset + s->st20_pkt_len < s->st20_frame_size))
      e_rtp =
          rte_pktmbuf_mtod_offset(pkt, struct st20_rfc4175_extra_rtp_hdr*, sizeof(*hdr));
  }

  /* update rtp */
  if (s->st20_pkt_idx >= (s->st20_total_pkts - 1)) rtp->base.marker = 1;
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->st20_seq_id >> 16));
  s->st20_seq_id++;
  uint16_t field = frame_info->tv_meta.second_field ? ST20_SECOND_FIELD : 0x0000;
  rtp->row_number = htons(line1_number | field);
  rtp->row_offset = htons(line1_offset);
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  uint32_t temp =
      single_line ? ((ops->width - line1_offset) / s->st20_pg.coverage * s->st20_pg.size)
                  : (s->st20_frame_size - offset);
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, temp);
  rtp->row_length = htons(left_len);

  if (e_rtp) {
    line1_length = (line1_number + 1) * s->st20_bytes_in_line - offset;
    line2_length = s->st20_pkt_len - line1_length;
    rtp->row_length = htons(line1_length);
    e_rtp->row_length = htons(line2_length);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons((line1_number + 1) | field);
    rtp->row_offset = htons(line1_offset | ST20_SRD_OFFSET_CONTINUATION);
  }

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct st_rfc4175_video_hdr);
  if (e_rtp) pkt->data_len += sizeof(*e_rtp);
  pkt->pkt_len = pkt->data_len;

  if (!single_line && s->st20_linesize > s->st20_bytes_in_line)
    /* update offset with line padding for copying */
    offset = offset % s->st20_bytes_in_line + line1_number * s->st20_linesize;

  if (e_rtp && s->st20_linesize > s->st20_bytes_in_line) {
    /* cross lines with padding case */
    /* re-allocate from copy chain mempool */
    rte_pktmbuf_free(pkt_chain);
    pkt_chain = rte_pktmbuf_alloc(s->mbuf_mempool_copy_chain);
    if (!pkt_chain) {
      dbg("%s(%d), pkts chain realloc fail %d\n", __func__, s->idx, s->st20_pkt_idx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_chain_realloc_fail);
      return -ENOMEM;
    }
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    mtl_memcpy(payload, frame_info->addr + offset, line1_length);
    mtl_memcpy(payload + line1_length,
               frame_info->addr + s->st20_linesize * (line1_number + 1), line2_length);
  } else if (tv_frame_payload_cross_page(s, frame_info, offset, left_len)) {
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    mtl_memcpy(payload, frame_info->addr + offset, left_len);
  } else {
    /* attach payload to chainbuf */
    rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                              tv_frame_get_offset_iova(s, frame_info, offset), left_len,
                              &frame_info->sh_info);
    rte_mbuf_ext_refcnt_update(&frame_info->sh_info, 1);
  }
  pkt_chain->data_len = pkt_chain->pkt_len = left_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st20_redundant_chain(struct st_tx_video_session_impl* s,
                                         struct rte_mbuf* pkt_r,
                                         const struct rte_mbuf* pkt_base) {
  struct st_rfc4175_video_hdr* hdr;
  struct st_rfc4175_video_hdr* hdr_base;
  struct rte_ipv4_hdr* ipv4;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_rtp_hdr* rtp_base;

  hdr = rte_pktmbuf_mtod(pkt_r, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_rfc4175_video_hdr*);
  rtp_base = &hdr_base->rtp;
  rte_memcpy(rtp, rtp_base, sizeof(*rtp));

  /* copy extra if Continuation */
  uint16_t line1_offset = ntohs(rtp->row_offset);
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    rte_memcpy(&rtp[1], &rtp_base[1], sizeof(struct st20_rfc4175_extra_rtp_hdr));
  }

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  struct rte_mbuf* pkt_chain = pkt_base->next;
  pkt_r->next = pkt_chain;

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_rtp(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
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
  rte_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  if (rtp->tmstamp != s->st20_rtp_time) {
    /* start of a new frame */
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st20_rtp_time = rtp->tmstamp;
    if (s->ops.interlaced) {
      struct st20_rfc4175_rtp_hdr* rfc4175 = rte_pktmbuf_mtod_offset(
          pkt, struct st20_rfc4175_rtp_hdr*, sizeof(struct mt_udp_hdr));
      uint16_t line1_number = ntohs(rfc4175->row_number);
      if (line1_number & ST20_SECOND_FIELD) {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
      } else {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
      }
    }
    tv_sync_pacing(impl, s, 0);
    if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    } else {
      uint64_t tai_for_rtp_ts;
      if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
        tai_for_rtp_ts = tai_from_frame_count(&s->pacing, s->pacing.cur_epochs);
      } else {
        tai_for_rtp_ts = s->pacing.ptp_time_cursor;
      }
      tai_for_rtp_ts += (uint64_t)s->ops.rtp_timestamp_delta_us * NS_PER_US;
      s->pacing.rtp_time_stamp =
          st10_tai_to_media_clk(tai_for_rtp_ts, s->fps_tm.sampling_clock_rate);
    }
    dbg("%s(%d), rtp time stamp %u\n", __func__, s->idx, s->pacing.rtp_time_stamp);
  }
  /* update rtp time*/
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

static int tv_build_rtp_chain(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s, struct rte_mbuf* pkt,
                              struct rte_mbuf* pkt_chain) {
  struct mt_udp_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st_rfc3550_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = rte_pktmbuf_mtod(pkt_chain, struct st_rfc3550_rtp_hdr*);

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  if (rtp->tmstamp != s->st20_rtp_time) {
    /* start of a new frame */
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st20_rtp_time = rtp->tmstamp;
    if (s->ops.interlaced) {
      struct st20_rfc4175_rtp_hdr* rfc4175 =
          rte_pktmbuf_mtod(pkt_chain, struct st20_rfc4175_rtp_hdr*);
      uint16_t line1_number = ntohs(rfc4175->row_number);
      if (line1_number & ST20_SECOND_FIELD) {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
      } else {
        ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
      }
    }
    tv_sync_pacing(impl, s, 0);
    if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    } else {
      uint64_t tai_for_rtp_ts;
      if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
        tai_for_rtp_ts = tai_from_frame_count(&s->pacing, s->pacing.cur_epochs);
      } else {
        tai_for_rtp_ts = s->pacing.ptp_time_cursor;
      }
      tai_for_rtp_ts += (uint64_t)s->ops.rtp_timestamp_delta_us * NS_PER_US;
      s->pacing.rtp_time_stamp =
          st10_tai_to_media_clk(tai_for_rtp_ts, s->fps_tm.sampling_clock_rate);
    }
    dbg("%s(%d), rtp time stamp %u\n", __func__, s->idx, s->pacing.rtp_time_stamp);
  }
  /* update rtp time*/
  rtp->tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct mt_udp_hdr);
  pkt->pkt_len = pkt->data_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }
  return 0;
}

static int tv_build_rtp_redundant_chain(struct st_tx_video_session_impl* s,
                                        struct rte_mbuf* pkt_r,
                                        struct rte_mbuf* pkt_base) {
  struct rte_ipv4_hdr* ipv4;
  struct mt_udp_hdr* hdr;

  hdr = rte_pktmbuf_mtod(pkt_r, struct mt_udp_hdr*);
  ipv4 = &hdr->ipv4;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_R].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_R].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(&hdr->udp, &s->s_hdr[MTL_SESSION_PORT_R].udp, sizeof(hdr->udp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  struct rte_mbuf* pkt_chain = pkt_base->next;
  pkt_r->next = pkt_chain;

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st22(struct st_tx_video_session_impl* s, struct rte_mbuf* pkt) {
  struct st22_rfc9134_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_tx_video_info* st22_info = s->st22_info;

  hdr = rte_pktmbuf_mtod(pkt, struct st22_rfc9134_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(*ipv4));
  rte_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(*udp));
  /* copy rtp */
  rte_memcpy(rtp, &st22_info->rtp_hdr[MTL_SESSION_PORT_P], sizeof(*rtp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  /* update rtp */
  if (s->st20_pkt_idx >= (st22_info->st22_total_pkts - 1)) {
    rtp->base.marker = 1;
    rtp->last_packet = 1;
    dbg("%s(%d), maker on pkt %d(total %d)\n", __func__, s->idx, s->st20_pkt_idx,
        s->st20_total_pkts);
  }
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  s->st20_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
  uint16_t f_counter = st22_info->frame_idx % 32;
  uint16_t sep_counter = s->st20_pkt_idx / 2048;
  uint16_t p_counter = s->st20_pkt_idx % 2048;
  rtp->p_counter_lo = p_counter;
  rtp->p_counter_hi = p_counter >> 8;
  rtp->sep_counter_lo = sep_counter;
  rtp->sep_counter_hi = sep_counter >> 5;
  rtp->f_counter_lo = f_counter;
  rtp->f_counter_hi = f_counter >> 2;

  if (s->ops.interlaced) {
    struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
    if (frame_info->tx_st22_meta.second_field)
      rtp->interlaced = 0x3;
    else
      rtp->interlaced = 0x2;
  }

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);

  uint32_t offset = s->st20_pkt_idx * s->st20_pkt_len;
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, st22_info->cur_frame_size - offset);
  dbg("%s(%d), data len %u on pkt %d(total %d)\n", __func__, s->idx, left_len,
      s->st20_pkt_idx, s->st20_total_pkts);

  /* copy payload */
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
  void* payload = &rtp[1];
  mtl_memcpy(payload, frame_info->addr + offset, left_len);

  pkt->data_len = sizeof(*hdr) + left_len;
  pkt->pkt_len = pkt->data_len;

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st22_chain(struct st_tx_video_session_impl* s, struct rte_mbuf* pkt,
                               struct rte_mbuf* pkt_chain) {
  struct st22_rfc9134_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_tx_video_info* st22_info = s->st22_info;

  hdr = rte_pktmbuf_mtod(pkt, struct st22_rfc9134_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(*ipv4));
  rte_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(*udp));
  /* copy rtp */
  rte_memcpy(rtp, &st22_info->rtp_hdr[MTL_SESSION_PORT_P], sizeof(*rtp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  /* update rtp */
  if (s->st20_pkt_idx >= (st22_info->st22_total_pkts - 1)) {
    rtp->base.marker = 1;
    rtp->last_packet = 1;
    dbg("%s(%d), maker on pkt %d(total %d)\n", __func__, s->idx, s->st20_pkt_idx,
        s->st20_total_pkts);
  }
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  s->st20_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
  uint16_t f_counter = st22_info->frame_idx % 32;
  uint16_t sep_counter = s->st20_pkt_idx / 2048;
  uint16_t p_counter = s->st20_pkt_idx % 2048;
  rtp->p_counter_lo = p_counter;
  rtp->p_counter_hi = p_counter >> 8;
  rtp->sep_counter_lo = sep_counter;
  rtp->sep_counter_hi = sep_counter >> 5;
  rtp->f_counter_lo = f_counter;
  rtp->f_counter_hi = f_counter >> 2;

  if (s->ops.interlaced) {
    struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
    if (frame_info->tx_st22_meta.second_field)
      rtp->interlaced = 0x3;
    else
      rtp->interlaced = 0x2;
  }

  /* update mbuf */
  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr);
  pkt->pkt_len = pkt->data_len;

  uint32_t offset = s->st20_pkt_idx * s->st20_pkt_len;
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, st22_info->cur_frame_size - offset);
  dbg("%s(%d), data len %u on pkt %d(total %d)\n", __func__, s->idx, left_len,
      s->st20_pkt_idx, s->st20_total_pkts);

  /* attach payload to chainbuf */
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
  if (tv_frame_payload_cross_page(s, frame_info, offset, left_len)) {
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    mtl_memcpy(payload, frame_info->addr + offset, left_len);
  } else { /* attach payload */
    rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                              tv_frame_get_offset_iova(s, frame_info, offset), left_len,
                              &frame_info->sh_info);
    rte_mbuf_ext_refcnt_update(&frame_info->sh_info, 1);
  }

  pkt_chain->data_len = pkt_chain->pkt_len = left_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st22_redundant_chain(struct st_tx_video_session_impl* s,
                                         struct rte_mbuf* pkt_r,
                                         struct rte_mbuf* pkt_base) {
  struct st22_rfc9134_video_hdr* hdr;
  struct st22_rfc9134_video_hdr* hdr_base;
  struct rte_ipv4_hdr* ipv4;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_rfc9134_rtp_hdr* rtp_base;

  hdr = rte_pktmbuf_mtod(pkt_r, struct st22_rfc9134_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st22_rfc9134_video_hdr*);
  rtp_base = &hdr_base->rtp;
  rte_memcpy(rtp, rtp_base, sizeof(*rtp));

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  struct rte_mbuf* pkt_chain = pkt_base->next;
  pkt_r->next = pkt_chain;

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static uint64_t tv_pacing_required_tai(struct st_tx_video_session_impl* s,
                                       enum st10_timestamp_fmt tfmt, uint64_t timestamp) {
  uint64_t required_tai = 0;

  if (!(s->ops.flags & ST20_TX_FLAG_USER_PACING)) return 0;
  if (!timestamp) {
    if (s->ops.flags & ST20_TX_FLAG_EXACT_USER_PACING) {
      err("%s(%d), EXACT_USER_PACING requires non-zero timestamp\n", __func__, s->idx);
    }
    return 0;
  }

  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    err("%s(%d), Media clock can't be used for user-controlled pacing\n", __func__,
        s->idx);
    return 0;  // Return 0 to indicate an invalid timestamp and fallback to default pacing
  } else {
    required_tai = timestamp;
  }

  return required_tai;
}

static int tv_tasklet_start(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_video_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_get(mgr, sidx);
    if (!s) continue;
    /* re-calculate the vsync */
    if (s->ops.flags & ST20_TX_FLAG_ENABLE_VSYNC) st_vsync_calculate(impl, &s->vsync);
    for (int i = 0; i < s->ops.num_port; i++) {
      s->last_burst_succ_time_tsc[i] = mt_get_tsc(impl);
    }
    /* calculate the pacing epoch */
    tv_init_pacing_epoch(impl, s);
    tx_video_session_put(mgr, sidx);
  }

  return 0;
}

static int tv_usdt_dump_frame(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              struct st_frame_trans* frame) {
  struct st_tx_video_sessions_mgr* mgr = s->mgr;
  int idx = s->idx;
  int fd;
  char usdt_dump_path[64];
  struct st20_tx_ops* ops = &s->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st20tx_m%ds%d_%d_%d_XXXXXX.yuv", mgr->idx, idx, ops->width,
           ops->height);
  fd = mt_mkstemps(usdt_dump_path, strlen(".yuv"));
  if (fd < 0) {
    err("%s(%d), mkstemps %s fail %d\n", __func__, idx, usdt_dump_path, fd);
    return fd;
  }

  /* write frame to dump file */
  ssize_t n = write(fd, frame->addr, s->st20_frame_size);
  if (n != s->st20_frame_size) {
    warn("%s(%d), write fail %" PRIu64 "\n", __func__, idx, n);
  } else {
    MT_USDT_ST20_TX_FRAME_DUMP(mgr->idx, s->idx, usdt_dump_path, frame->addr, n);
  }

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static int tv_tasklet_frame(struct mtl_main_impl* impl,
                            struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
  int idx = s->idx;
  struct st20_tx_ops* ops = &s->ops;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = s->ring[MTL_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;
  int num_port = ops->num_port;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[MTL_SESSION_PORT_P][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_P][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->inflight[MTL_SESSION_PORT_R][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[MTL_SESSION_PORT_R][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_R][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx = 0;
      struct st20_tx_frame_meta meta;
      uint64_t tsc_start = 0;

      tv_init_next_meta(s, &meta);
      /* Query next frame buffer idx */
      bool time_measure = mt_sessions_time_measure(impl);
      if (time_measure) tsc_start = mt_get_tsc(impl);
      ret = ops->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (time_measure) {
        uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
        s->stat_max_next_frame_us = RTE_MAX(s->stat_max_next_frame_us, delta_us);
      }
      if (ret < 0) { /* no frame ready from app */
        if (s->stat_user_busy_first) {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_user_busy);
          s->stat_user_busy_first = false;
          dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        }
        s->stat_build_ret_code = -STI_FRAME_APP_GET_FRAME_BUSY;
        return MTL_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st20_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
            refcnt);
        s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
        return MTL_TASKLET_ALL_DONE;
      }
      frame->tv_meta = meta;

      frame->user_meta_data_size = 0;
      if (meta.user_meta) {
        if (meta.user_meta_size > frame->user_meta_buffer_size) {
          err("%s(%d), frame %u user meta size %" PRId64 " too large\n", __func__, idx,
              next_frame_idx, meta.user_meta_size);
          s->stat_build_ret_code = -STI_FRAME_APP_ERR_USER_META;
          return MTL_TASKLET_ALL_DONE;
        }
        ST_SESSION_STAT_INC(s, port_user_stats, stat_user_meta_cnt);
        /* copy user meta to frame meta */
        rte_memcpy(frame->user_meta, meta.user_meta, meta.user_meta_size);
        frame->user_meta_data_size = meta.user_meta_size;
      }

      s->stat_user_busy_first = true;
      /* all check fine */
      rte_atomic32_inc(&frame->refcnt);
      s->st20_frame_idx = next_frame_idx;
      s->st20_frame_lines_ready = 0;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      /* user timestamp control if any */
      uint64_t required_tai = tv_pacing_required_tai(s, meta.tfmt, meta.timestamp);
      if (s->ops.interlaced) {
        if (frame->tv_meta.second_field) {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
        } else {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
        }
        /* s->second_field is used to init the next frame */
        s->second_field = !frame->tv_meta.second_field;
      }
      tv_sync_pacing(impl, s, required_tai);
      tv_update_rtp_time_stamp(s, meta.tfmt, meta.timestamp);
      frame->tv_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
      frame->tv_meta.timestamp = pacing->ptp_time_cursor;
      frame->tv_meta.rtp_timestamp = pacing->rtp_time_stamp;
      frame->tv_meta.epoch = pacing->cur_epochs;
      /* init to next field */
      MT_USDT_ST20_TX_FRAME_NEXT(s->mgr->idx, s->idx, next_frame_idx, frame->addr,
                                 pacing->rtp_time_stamp);
      /* check if dump USDT enabled */
      if (MT_USDT_ST20_TX_FRAME_DUMP_ENABLED()) {
        int period = st_frame_rate(ops->fps) * 5; /* dump every 5s now */
        if ((s->usdt_frame_cnt % period) == (period / 2)) {
          tv_usdt_dump_frame(impl, s, frame);
        }
        s->usdt_frame_cnt++;
      } else {
        s->usdt_frame_cnt = 0;
      }
    }
  }

  if (ops->type == ST20_TYPE_SLICE_LEVEL) {
    uint16_t line_number = 0;
    if (ops->packing == ST20_PACKING_GPM_SL) {
      line_number = (s->st20_pkt_idx + bulk) / s->st20_pkts_in_line;
    } else {
      uint32_t offset = s->st20_pkt_len * (s->st20_pkt_idx + bulk);
      line_number = offset / s->st20_bytes_in_line + 1;
    }

    uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
    if (line_number >= height) {
      line_number = height - 1;
    }
    if (line_number >= s->st20_frame_lines_ready) {
      struct st20_tx_slice_meta slice_meta;
      memset(&slice_meta, 0, sizeof(slice_meta));
      ret = ops->query_frame_lines_ready(ops->priv, s->st20_frame_idx, &slice_meta);
      if (ret >= 0) s->st20_frame_lines_ready = slice_meta.lines_ready;
      dbg("%s(%d), need line %u, ready lines %u\n", __func__, s->idx, ops->height,
          s->st20_frame_lines_ready);
      if ((ret < 0) || (line_number >= s->st20_frame_lines_ready)) {
        dbg("%s(%d), line %u not ready, ready lines %u\n", __func__, s->idx, line_number,
            s->st20_frame_lines_ready);
        ST_SESSION_STAT_INC(s, port_user_stats, stat_lines_not_ready);
        s->stat_build_ret_code = -STI_FRAME_APP_SLICE_NOT_READY;
        return MTL_TASKLET_ALL_DONE;
      }
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_chain[bulk];

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (!s->tx_no_chain) {
    ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_CHAIN_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      if (!s->tx_no_chain) rte_pktmbuf_free_bulk(pkts_chain, bulk);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_R_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  for (unsigned int i = 0; i < bulk; i++) {
    st_tx_mbuf_set_priv(pkts[i], &s->st20_frames[s->st20_frame_idx]);
    if (s->st20_pkt_idx >= s->st20_total_pkts) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dummy);
      if (!s->tx_no_chain) rte_pktmbuf_free(pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
    } else {
      if (s->tx_no_chain)
        tv_build_st20(s, pkts[i]);
      else
        tv_build_st20_chain(s, pkts[i], pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
      s->stat_pkts_build[MTL_SESSION_PORT_P]++;
      s->port_user_stats.common.port[MTL_SESSION_PORT_P].build++;
    }
    pacing_set_mbuf_time_stamp(pkts[i], pacing);

    if (send_r) {
      st_tx_mbuf_set_priv(pkts_r[i], &s->st20_frames[s->st20_frame_idx]);
      if (s->st20_pkt_idx >= s->st20_total_pkts) {
        st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
      } else {
        if (s->tx_no_chain) {
          tv_build_st20_redundant(s, pkts_r[i], pkts[i]);
        } else
          tv_build_st20_redundant_chain(s, pkts_r[i], pkts[i]);
        st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
        s->stat_pkts_build[MTL_SESSION_PORT_R]++;
        s->port_user_stats.common.port[MTL_SESSION_PORT_R].build++;
      }
      pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
    }

    pacing_forward_cursor(pacing); /* pkt forward */
    s->st20_pkt_idx++;
  }

  bool done = false;

  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[MTL_SESSION_PORT_P][i] = pkts[i];
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[MTL_SESSION_PORT_R][i] = pkts_r[i];
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done with %d pkts\n", __func__, idx, s->st20_frame_idx,
        s->st20_pkt_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (send_r) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    rte_atomic32_inc(&s->stat_frame_cnt);
    if (s->tx_no_chain) {
      /* trigger extbuf free cb since mbuf attach not used */
      struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
      tv_frame_free_cb(frame_info->addr, frame_info);
    }

    uint64_t frame_end_time = mt_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_exceed_frame_time);
      rte_atomic32_inc(&s->cbs_build_timeout);
      dbg("%s(%d), frame %d build time out %ldus\n", __func__, idx, s->st20_frame_idx,
          (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tv_tasklet_rtcp(struct st_tx_video_session_impl* s) {
  struct rte_mbuf* mbuf[ST_TX_VIDEO_RTCP_BURST_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->rtcp_q[s_port]) continue;

    rv = mt_rxq_burst(s->rtcp_q[s_port], &mbuf[0], ST_TX_VIDEO_RTCP_BURST_SIZE);
    if (rv) {
      for (uint16_t i = 0; i < rv; i++) {
        // rte_pktmbuf_dump(stdout, mbuf[i], mbuf[i]->pkt_len);
        struct mt_rtcp_hdr* rtcp = rte_pktmbuf_mtod_offset(mbuf[i], struct mt_rtcp_hdr*,
                                                           sizeof(struct mt_udp_hdr));
        mt_rtcp_tx_parse_rtcp_packet(s->rtcp_tx[s_port], rtcp);
      }
      rte_pktmbuf_free_bulk(&mbuf[0], rv);
    }
  }

  return 0;
}

static int tv_tasklet_rtp(struct mtl_main_impl* impl,
                          struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
#ifdef DEBUG
  int idx = s->idx;
#endif
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_ring* ring_p = s->ring[MTL_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  ret = -1;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_RTP_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[MTL_SESSION_PORT_P][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_P][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->inflight[MTL_SESSION_PORT_R][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[MTL_SESSION_PORT_R][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_R][0] = false;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_rtp[bulk];
  int pkts_remaining = s->st20_total_pkts - s->st20_pkt_idx;
  bool eof = (pkts_remaining > 0) && (pkts_remaining < bulk) ? true : false;
  unsigned int pkts_bulk = eof ? 1 : bulk; /* bulk one only at end of frame */

  if (eof)
    dbg("%s(%d), pkts_bulk %d pkt idx %d\n", __func__, idx, pkts_bulk, s->st20_pkt_idx);

  n = mt_rte_ring_sc_dequeue_bulk(s->packet_ring, (void**)&pkts_rtp, pkts_bulk, NULL);
  if (n == 0) {
    if (s->stat_user_busy_first) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_user_busy);
      s->stat_user_busy_first = false;
      dbg("%s(%d), rtp pkts not ready %d, ring cnt %d\n", __func__, idx, ret,
          rte_ring_count(s->packet_ring));
    }
    s->stat_build_ret_code = -STI_RTP_APP_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }
  s->stat_user_busy_first = true;
  s->ops.notify_rtp_done(s->ops.priv);

  uint16_t alloc_begin = s->tx_no_chain ? pkts_bulk : 0;
  uint16_t alloc_bulk = s->tx_no_chain ? bulk - pkts_bulk : bulk;
  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, &pkts[alloc_begin], alloc_bulk);
  if (ret < 0) {
    dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_rtp, pkts_bulk);
    s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }
  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, &pkts_r[alloc_begin], alloc_bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(&pkts[alloc_begin], alloc_bulk);
      rte_pktmbuf_free_bulk(pkts_rtp, pkts_bulk);
      s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  for (unsigned int i = 0; i < pkts_bulk; i++) {
    if (s->tx_no_chain) {
      pkts[i] = pkts_rtp[i];
      tv_build_rtp(impl, s, pkts[i]);
    } else {
      tv_build_rtp_chain(impl, s, pkts[i], pkts_rtp[i]);
    }
    st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    pacing_set_mbuf_time_stamp(pkts[i], pacing);
    s->stat_pkts_build[MTL_SESSION_PORT_P]++;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].build++;

    if (send_r) {
      if (s->tx_no_chain) {
        pkts_r[i] = rte_pktmbuf_alloc(hdr_pool_r);
        if (pkts_r[i] == NULL) {
          dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
          rte_pktmbuf_free_bulk(pkts, bulk);
          rte_pktmbuf_free_bulk(pkts_r, bulk);
          s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
          s->st20_pkt_idx -= i; /* todo: revert all status */
          return MTL_TASKLET_ALL_DONE;
        }
        tv_build_st20_redundant(s, pkts_r[i], pkts[i]);
      } else
        tv_build_rtp_redundant_chain(s, pkts_r[i], pkts[i]);
      st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
      s->stat_pkts_build[MTL_SESSION_PORT_R]++;
      s->port_user_stats.common.port[MTL_SESSION_PORT_R].build++;
    }

    pacing_forward_cursor(pacing); /* pkt forward */
    s->st20_pkt_idx++;
  }

  /* build dummy bulk pkts to satisfy video transmitter which is bulk based */
  if (eof) {
    for (unsigned int i = pkts_bulk; i < bulk; i++) {
      st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
      pacing_set_mbuf_time_stamp(pkts[i], pacing);
      if (send_r) {
        st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
        pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
      }
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dummy);
    }
  }

  bool done = false;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[MTL_SESSION_PORT_P][i] = pkts[i];
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[MTL_SESSION_PORT_R][i] = pkts_r[i];
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tv_st22_usdt_dump_codestream(struct mtl_main_impl* impl,
                                        struct st_tx_video_session_impl* s,
                                        struct st_frame_trans* frame, size_t size) {
  struct st_tx_video_sessions_mgr* mgr = s->mgr;
  int idx = s->idx;
  int fd;
  char usdt_dump_path[64];
  struct st20_tx_ops* ops = &s->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st22tx_m%ds%d_%d_%d_XXXXXX.raw", mgr->idx, idx, ops->width,
           ops->height);
  fd = mt_mkstemps(usdt_dump_path, strlen(".raw"));
  if (fd < 0) {
    err("%s(%d), mkstemps %s fail %d\n", __func__, idx, usdt_dump_path, fd);
    return fd;
  }

  /* write frame to dump file */
  ssize_t n = write(fd, frame->addr, size);
  if (n != size) {
    warn("%s(%d), write fail %" PRIu64 "\n", __func__, idx, n);
  } else {
    MT_USDT_ST22_TX_FRAME_DUMP(mgr->idx, s->idx, usdt_dump_path, frame->addr, n);
  }

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static int tv_tasklet_st22(struct mtl_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
  int idx = s->idx;
  struct st20_tx_ops* ops = &s->ops;
  struct st22_tx_video_info* st22_info = s->st22_info;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[MTL_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = s->ring[MTL_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_ST22_RING_FULL;
    return MTL_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[MTL_SESSION_PORT_R];
    ring_r = s->ring[MTL_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->inflight[MTL_SESSION_PORT_P][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[MTL_SESSION_PORT_P][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_P][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_ST22_INFLIGHT_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->inflight[MTL_SESSION_PORT_R][0]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[MTL_SESSION_PORT_R][0],
                                 bulk, NULL);
    if (n > 0) {
      s->inflight[MTL_SESSION_PORT_R][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_ST22_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx;
      struct st22_tx_frame_meta meta;
      uint64_t tsc_start = 0;

      tv_init_st22_next_meta(s, &meta);
      /* Query next frame buffer idx */
      bool time_measure = mt_sessions_time_measure(impl);
      if (time_measure) tsc_start = mt_get_tsc(impl);
      ret = st22_info->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (time_measure) {
        uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
        s->stat_max_next_frame_us = RTE_MAX(s->stat_max_next_frame_us, delta_us);
      }
      if (ret < 0) { /* no frame ready from app */
        if (s->stat_user_busy_first) {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_user_busy);
          s->stat_user_busy_first = false;
          dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        }
        s->stat_build_ret_code = -STI_ST22_APP_GET_FRAME_BUSY;
        return MTL_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st20_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        err("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
            refcnt);
        s->stat_build_ret_code = -STI_ST22_APP_ERR_TX_FRAME;
        return MTL_TASKLET_ALL_DONE;
      }
      /* check code stream size */
      size_t codestream_size = meta.codestream_size;
      if ((codestream_size > s->st22_codestream_size) || !codestream_size) {
        err("%s(%d), invalid codestream size %" PRIu64 ", allowed %" PRIu64 "\n",
            __func__, idx, codestream_size, s->st22_codestream_size);
        tv_notify_frame_done(s, next_frame_idx);
        s->stat_build_ret_code = -STI_ST22_APP_GET_FRAME_ERR_SIZE;
        return MTL_TASKLET_ALL_DONE;
      }

      s->stat_user_busy_first = true;
      /* all check fine */
      frame->tx_st22_meta = meta;
      rte_atomic32_inc(&frame->refcnt);
      size_t frame_size = codestream_size + s->st22_box_hdr_length;
      st22_info->st22_total_pkts = frame_size / s->st20_pkt_len;
      if (frame_size % s->st20_pkt_len) st22_info->st22_total_pkts++;
      s->st20_total_pkts = st22_info->st22_total_pkts;
      st22_info->cur_frame_size = frame_size;
      s->st20_frame_idx = next_frame_idx;
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      /* user timestamp control if any */
      uint64_t required_tai = tv_pacing_required_tai(s, meta.tfmt, meta.timestamp);
      if (s->ops.interlaced) {
        if (frame->tx_st22_meta.second_field) {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
        } else {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
        }
        /* s->second_field is used to init the next frame */
        s->second_field = !frame->tx_st22_meta.second_field;
      }
      tv_sync_pacing_st22(impl, s, required_tai, st22_info->st22_total_pkts);
      tv_update_rtp_time_stamp(s, meta.tfmt, meta.timestamp);
      frame->tx_st22_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
      frame->tx_st22_meta.timestamp = pacing->ptp_time_cursor;
      frame->tx_st22_meta.epoch = pacing->cur_epochs;
      frame->tx_st22_meta.rtp_timestamp = pacing->rtp_time_stamp;
      MT_USDT_ST22_TX_FRAME_NEXT(s->mgr->idx, s->idx, next_frame_idx, frame->addr,
                                 pacing->rtp_time_stamp, codestream_size);
      /* check if dump USDT enabled */
      if (MT_USDT_ST22_TX_FRAME_DUMP_ENABLED()) {
        int period = st_frame_rate(ops->fps) * 5; /* dump every 5s now */
        if ((s->usdt_frame_cnt % period) == (period / 2)) {
          tv_st22_usdt_dump_codestream(impl, s, frame, frame_size);
        }
        s->usdt_frame_cnt++;
      } else {
        s->usdt_frame_cnt = 0;
      }
      dbg("%s(%d), next_frame_idx %d(%d pkts) start\n", __func__, idx, next_frame_idx,
          s->st20_total_pkts);
      dbg("%s(%d), codestream_size %" PRId64 "(%d st22 pkts) time_stamp %u\n", __func__,
          idx, codestream_size, st22_info->st22_total_pkts, pacing->rtp_time_stamp);
      return MTL_TASKLET_HAS_PENDING;
    } else if (ST21_TX_STAT_SENDING_PKTS == s->st20_frame_stat) {
      uint64_t tsc_time_frame_start = s->pacing.tsc_time_frame_start;
      if (tsc_time_frame_start) {
        if (mt_get_tsc(impl) < tsc_time_frame_start) {
          return MTL_TASKLET_ALL_DONE;
        }
        s->pacing.tsc_time_frame_start = 0; /* time reach, clear now */
      }
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];

  if (s->st20_pkt_idx >= st22_info->st22_total_pkts) { /* build pad */
    struct rte_mbuf* pad = s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL];
    struct rte_mbuf* pad_r = s->pad[MTL_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL];

    for (unsigned int i = 0; i < bulk; i++) {
      dbg("%s(%d), pad on pkt %d\n", __func__, s->idx, s->st20_pkt_idx);
      pkts[i] = pad;
      rte_mbuf_refcnt_update(pad, 1);
      st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
      pacing_set_mbuf_time_stamp(pkts[i], pacing);

      if (send_r) {
        pkts_r[i] = pad_r;
        rte_mbuf_refcnt_update(pad_r, 1);
        st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      }

      pacing_forward_cursor(pacing); /* pkt forward */
      s->st20_pkt_idx++;
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dummy);
    }
  } else {
    struct rte_mbuf* pkts_chain[bulk];

    ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }

    if (!s->tx_no_chain) {
      ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
      if (ret < 0) {
        dbg("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
        rte_pktmbuf_free_bulk(pkts, bulk);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_CHAIN_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
    }

    if (send_r) {
      ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
      if (ret < 0) {
        dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
        rte_pktmbuf_free_bulk(pkts, bulk);
        if (!s->tx_no_chain) rte_pktmbuf_free_bulk(pkts_chain, bulk);
        s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_R_FAIL;
        return MTL_TASKLET_ALL_DONE;
      }
    }

    for (unsigned int i = 0; i < bulk; i++) {
      if (s->st20_pkt_idx >= st22_info->st22_total_pkts) {
        dbg("%s(%d), pad on pkt %d\n", __func__, s->idx, s->st20_pkt_idx);
        ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dummy);
        if (!s->tx_no_chain) rte_pktmbuf_free(pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
      } else {
        if (s->tx_no_chain)
          tv_build_st22(s, pkts[i]);
        else
          tv_build_st22_chain(s, pkts[i], pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
        s->stat_pkts_build[MTL_SESSION_PORT_P]++;
        s->port_user_stats.common.port[MTL_SESSION_PORT_P].build++;
      }
      pacing_set_mbuf_time_stamp(pkts[i], pacing);

      if (send_r) {
        if (s->st20_pkt_idx >= st22_info->st22_total_pkts) {
          st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
        } else {
          if (s->tx_no_chain)
            tv_build_st20_redundant(s, pkts_r[i], pkts[i]);
          else
            tv_build_st22_redundant_chain(s, pkts_r[i], pkts[i]);
          st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
          s->port_user_stats.common.port[MTL_SESSION_PORT_R].build++;
          s->stat_pkts_build[MTL_SESSION_PORT_R]++;
        }
        pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
      }

      pacing_forward_cursor(pacing); /* pkt forward */
      s->st20_pkt_idx++;
    }
  }

  bool done = false;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[MTL_SESSION_PORT_P][i] = pkts[i];
    s->inflight_cnt[MTL_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_ST22_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[MTL_SESSION_PORT_R][i] = pkts_r[i];
      s->inflight_cnt[MTL_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_ST22_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done with %d pkts\n", __func__, idx, s->st20_frame_idx,
        s->st20_pkt_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (send_r) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    rte_atomic32_inc(&s->stat_frame_cnt);
    st22_info->frame_idx++;
    if (s->tx_no_chain) {
      /* trigger extbuf free cb since mbuf attach not used */
      struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
      tv_frame_free_cb(frame_info->addr, frame_info);
    }

    uint64_t frame_end_time = mt_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_exceed_frame_time);
      rte_atomic32_inc(&s->cbs_build_timeout);
      dbg("%s(%d), frame %d build time out %ldus\n", __func__, idx, s->st20_frame_idx,
          (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int tvs_tasklet_handler(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_video_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;
    if (!s->active) goto exit;

    if (time_measure) tsc_s = mt_get_tsc(impl);

    if (s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP) tv_tasklet_rtcp(s);
    /* check vsync if it has vsync enabled */
    if (s->ops.flags & ST20_TX_FLAG_ENABLE_VSYNC) tv_poll_vsync(impl, s);

    s->stat_build_ret_code = 0;
    if (s->st22_info)
      pending = tv_tasklet_st22(impl, s);
    else if (st20_is_frame_type(s->ops.type))
      pending = tv_tasklet_frame(impl, s);
    else
      pending = tv_tasklet_rtp(impl, s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }

  exit:
    tx_video_session_put(mgr, sidx);
  }

  return pending;
}

static int tv_uinit_hw(struct st_tx_video_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->ring[i]) {
      mt_ring_dequeue_clean(s->ring[i]);
      rte_ring_free(s->ring[i]);
      s->ring[i] = NULL;
    }

    if (s->queue[i]) {
      struct rte_mbuf* pad = s->pad[i][ST20_PKT_TYPE_NORMAL];
      /* free completed mbufs from NIC tx ring before flushing */
      mt_txq_done_cleanup(s->queue[i]);
      /* flush all the pkts in the tx ring desc */
      if (pad) mt_txq_flush(s->queue[i], pad);
      /* clean any remaining mbufs after flush */
      mt_txq_done_cleanup(s->queue[i]);
      mt_txq_put(s->queue[i]);
      s->queue[i] = NULL;
    }

    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (s->pad[i][j]) {
        rte_pktmbuf_free(s->pad[i][j]);
        s->pad[i][j] = NULL;
      }
    }
  }

  return 0;
}

static int tv_init_hw(struct mtl_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                      struct st_tx_video_session_impl* s) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx, idx = s->idx, num_port = s->ops.num_port;
  struct rte_mempool* pad_mempool;
  struct rte_mbuf* pad;
  enum mtl_port port;
  uint16_t queue_id;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    struct mt_txq_flow flow;
    memset(&flow, 0, sizeof(flow));
    flow.bytes_per_sec = tv_rl_bps(s);
    mt_pacing_train_bps_result_search(impl, i, flow.bytes_per_sec, &flow.bytes_per_sec);
    mtl_memcpy(&flow.dip_addr, &s->ops.dip_addr[i], MTL_IP_ADDR_LEN);
    flow.dst_port = s->ops.udp_port[i];
    if (ST21_TX_PACING_WAY_TSN == s->pacing_way[i])
      flow.flags |= MT_TXQ_FLOW_F_LAUNCH_TIME;
    flow.gso_sz = s->st20_pkt_size - sizeof(struct mt_udp_hdr);
    s->queue[i] = mt_txq_get(impl, port, &flow);
    if (!s->queue[i]) {
      tv_uinit_hw(s);
      return -EIO;
    }
    queue_id = mt_txq_queue_id(s->queue[i]);

    snprintf(ring_name, 32, "%sM%dS%dP%d", ST_TX_VIDEO_PREFIX, mgr_idx, idx, i);
    flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
    count = s->ring_count;
    ring = rte_ring_create(ring_name, count, s->socket_id, flags);
    if (!ring) {
      err("%s(%d,%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, idx, i);
      tv_uinit_hw(s);
      return -ENOMEM;
    }
    s->ring[i] = ring;
    info("%s(%d,%d), port(l:%d,p:%d), queue %d, count %u\n", __func__, mgr_idx, idx, i,
         port, queue_id, count);

    if (mt_pmd_is_dpdk_af_xdp(impl, port) && s->mbuf_mempool_reuse_rx[i]) {
      if (s->mbuf_mempool_hdr[i]) {
        err("%s(%d,%d), fail to reuse rx, has mempool_hdr for port %d\n", __func__,
            mgr_idx, idx, i);
      } else {
        /* reuse rx mempool for zero copy */
        if (mt_user_rx_mono_pool(impl))
          s->mbuf_mempool_hdr[i] = mt_sys_rx_mempool(impl, port);
        else
          s->mbuf_mempool_hdr[i] = mt_if(impl, port)->rx_queues[queue_id].mbuf_pool;
        info("%s(%d,%d), reuse rx mempool(%p) for port %d\n", __func__, mgr_idx, idx,
             s->mbuf_mempool_hdr[i], i);
      }
    }

    if (false && mt_pmd_is_dpdk_af_xdp(impl, port)) {
      /* disable now, always use no zc mempool for the flush pad */
      pad_mempool = s->mbuf_mempool_hdr[i];
    } else {
      pad_mempool = mt_sys_tx_mempool(impl, port);
    }
    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (!s->st20_pkt_info[j].number) continue;
      info("%s(%d), type %d number %u size %u\n", __func__, idx, j,
           s->st20_pkt_info[j].number, s->st20_pkt_info[j].size);
      pad = mt_build_pad(impl, pad_mempool, port, RTE_ETHER_TYPE_IPV4,
                         s->st20_pkt_info[j].size);
      if (!pad) {
        tv_uinit_hw(s);
        return -ENOMEM;
      }
      s->pad[i][j] = pad;
    }
  }

  return 0;
}

static int tv_mempool_free(struct st_tx_video_session_impl* s) {
  int ret;
  int retry;
  int max_retry = 10;

  if (s->mbuf_mempool_chain && !s->tx_mono_pool) {
    for (retry = 0; retry < max_retry; retry++) {
      ret = mt_mempool_free(s->mbuf_mempool_chain);
      if (ret >= 0) break;
      mt_sleep_ms(1); /* wait for NIC to complete DMA and free mbufs */
    }
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }
  if (s->mbuf_mempool_copy_chain && !s->tx_mono_pool) {
    for (retry = 0; retry < max_retry; retry++) {
      ret = mt_mempool_free(s->mbuf_mempool_copy_chain);
      if (ret >= 0) break;
      mt_sleep_ms(1);
    }
    if (ret >= 0) s->mbuf_mempool_copy_chain = NULL;
  }

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i]) {
      if (!s->mbuf_mempool_reuse_rx[i] && !s->tx_mono_pool) {
        for (retry = 0; retry < max_retry; retry++) {
          ret = mt_mempool_free(s->mbuf_mempool_hdr[i]);
          if (ret >= 0) break;
          mt_sleep_ms(1);
        }
      } else {
        ret = 0;
      }
      if (ret >= 0) s->mbuf_mempool_hdr[i] = NULL;
    }
  }

  return 0;
}

static bool tv_has_chain_buf(struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  for (int port = 0; port < num_ports; port++) {
    if (!s->eth_has_chain[port]) return false;
  }

  /* all ports capable chain */
  return true;
}

static bool tv_pkts_capable_chain(struct mtl_main_impl* impl,
                                  struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  /* true for rtp type */
  if (!st20_is_frame_type(ops->type)) return true;

  for (int port = 0; port < num_ports; port++) {
    enum mtl_port s_port = mt_port_logic2phy(s->port_maps, port);
    uint16_t max_buffer_nb = mt_if_nb_tx_desc(impl, s_port);
    // max_buffer_nb += s->ring_count;
    /* at least two swap buffer */
    if ((s->st20_total_pkts * (s->st20_frames_cnt - 1)) < max_buffer_nb) {
      warn("%s(%d), max_buffer_nb %u on s_port %d too large, st20_total_pkts %d\n",
           __func__, s->idx, max_buffer_nb, s_port, s->st20_total_pkts);
      return false;
    }
  }

  /* all ports capable chain */
  return true;
}

static int tv_mempool_init(struct mtl_main_impl* impl,
                           struct st_tx_video_sessions_mgr* mgr,
                           struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum mtl_port port;
  unsigned int n;
  uint16_t hdr_room_size = 0;
  uint16_t chain_room_size = 0;

  if (s->tx_no_chain) {
    /* do not use mbuf chain, use same mbuf for hdr+payload */
    hdr_room_size = s->st20_pkt_size;
  } else if (s->st22_info) {
    hdr_room_size = sizeof(struct st22_rfc9134_video_hdr);
    /* attach extbuf used, only placeholder mbuf */
    chain_room_size = 0;
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    hdr_room_size = sizeof(struct mt_udp_hdr);
    chain_room_size = s->rtp_pkt_max_size;
  } else { /* frame level */
    hdr_room_size = sizeof(struct st_rfc4175_video_hdr);
    if (ops->packing != ST20_PACKING_GPM_SL)
      hdr_room_size += sizeof(struct st20_rfc4175_extra_rtp_hdr);
    /* attach extbuf used, only placeholder mbuf */
    chain_room_size = 0;
    if (impl->iova_mode == RTE_IOVA_PA) /* need copy for cross page pkts*/
      chain_room_size = s->st20_pkt_len;
  }

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    /* allocate header mbuf pool */
    if (s->mbuf_mempool_reuse_rx[i]) {
      s->mbuf_mempool_hdr[i] = NULL; /* reuse rx mempool for zero copy */
    } else if (s->tx_mono_pool) {
      s->mbuf_mempool_hdr[i] = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx,
           s->mbuf_mempool_hdr[i], i);
    } else {
      n = mt_if_nb_tx_desc(impl, port) + s->ring_count;
      if (ops->flags & ST20_TX_FLAG_ENABLE_RTCP) n += ops->rtcp.buffer_size;
      if (ops->type == ST20_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
      if (s->mbuf_mempool_hdr[i]) {
        warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
      } else {
        char pool_name[32];
        snprintf(pool_name, 32, "%sM%dS%dP%d_HDR_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
                 i, s->recovery_idx);
        struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
            impl, pool_name, n, MT_MBUF_CACHE_SIZE, sizeof(struct mt_muf_priv_data),
            hdr_room_size, s->socket_id);
        if (!mbuf_pool) {
          tv_mempool_free(s);
          return -ENOMEM;
        }
        s->mbuf_mempool_hdr[i] = mbuf_pool;
      }
    }
  }

  /* allocate payload(chain) mbuf pool on primary port */
  if (!s->tx_no_chain) {
    port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    n = mt_if_nb_tx_desc(impl, port) + s->ring_count;
    if (ops->flags & ST20_TX_FLAG_ENABLE_RTCP) n += ops->rtcp.buffer_size;
    if (ops->type == ST20_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;

    if (s->tx_mono_pool) {
      s->mbuf_mempool_chain = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx,
           s->mbuf_mempool_chain);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%d_CHAIN_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
               s->recovery_idx);
      struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
          impl, pool_name, n, MT_MBUF_CACHE_SIZE, 0, chain_room_size, s->socket_id);
      if (!mbuf_pool) {
        tv_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_chain = mbuf_pool;

      /* has copy (not attach extbuf) and chain mbuf, create a special mempool */
      if (s->st20_linesize > s->st20_bytes_in_line &&
          s->ops.packing != ST20_PACKING_GPM_SL) {
        chain_room_size = s->st20_pkt_len;
        n /= s->st20_total_pkts / s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
        char pool_name[32];
        snprintf(pool_name, 32, "%sM%dS%d_COPY_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
                 s->recovery_idx);
        struct rte_mempool* mbuf_pool = mt_mempool_create_by_socket(
            impl, pool_name, n, MT_MBUF_CACHE_SIZE, 0, chain_room_size, s->socket_id);
        if (!mbuf_pool) {
          tv_mempool_free(s);
          return -ENOMEM;
        }
        s->mbuf_mempool_copy_chain = mbuf_pool;
      }
    }
  }

  return 0;
}

static int tv_init_packet_ring(struct st_tx_video_sessions_mgr* mgr,
                               struct st_tx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_TX_VIDEO_PREFIX, mgr_idx, idx);
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

static int tv_uinit_sw(struct st_tx_video_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    /* free all inflight */
    if (s->inflight[i][0]) {
      rte_pktmbuf_free_bulk(&s->inflight[i][0], s->bulk);
      s->inflight[i][0] = NULL;
    }
    if (s->trs_inflight_num[i]) {
      rte_pktmbuf_free_bulk(&s->trs_inflight[i][s->trs_inflight_idx[i]],
                            s->trs_inflight_num[i]);
      s->trs_inflight_num[i] = 0;
    }
    if (s->trs_inflight_num2[i]) {
      rte_pktmbuf_free_bulk(&s->trs_inflight2[i][s->trs_inflight_idx2[i]],
                            s->trs_inflight_num2[i]);
      s->trs_inflight_num2[i] = 0;
    }
  }

  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tv_mempool_free(s);

  tv_free_frames(s);

  if (s->st22_info) {
    mt_rte_free(s->st22_info);
    s->st22_info = NULL;
  }

  return 0;
}

static int tv_init_st22_frame(struct st_tx_video_session_impl* s,
                              struct st22_tx_ops* st22_frame_ops) {
  struct st22_tx_video_info* st22_info;

  st22_info = mt_rte_zmalloc_socket(sizeof(*st22_info), s->socket_id);
  if (!st22_info) return -ENOMEM;

  st22_info->get_next_frame = st22_frame_ops->get_next_frame;
  st22_info->notify_frame_done = st22_frame_ops->notify_frame_done;

  s->st22_info = st22_info;

  return 0;
}

static int tv_init_sw(struct mtl_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                      struct st_tx_video_session_impl* s,
                      struct st22_tx_ops* st22_frame_ops) {
  int idx = s->idx, ret;
  enum st20_type type = s->ops.type;

  if (st22_frame_ops) {
    ret = tv_init_st22_frame(s, st22_frame_ops);
    if (ret < 0) {
      err("%s(%d), tv_init_sw fail %d\n", __func__, idx, ret);
      tv_uinit_sw(s);
      return -EIO;
    }
    tv_init_st22_boxes(s);
  }

  /* free the pool if any in previous session */
  tv_mempool_free(s);
  ret = tv_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tv_uinit_sw(s);
    return ret;
  }

  if (type == ST20_TYPE_RTP_LEVEL)
    ret = tv_init_packet_ring(mgr, s);
  else
    ret = tv_alloc_frames(impl, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tv_uinit_sw(s);
    return ret;
  }

  return 0;
}

static int tv_init_pkt(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                       struct st20_tx_ops* ops, struct st22_tx_ops* st22_frame_ops) {
  int idx = s->idx;
  uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
  enum st20_type type = ops->type;

  /* clear pkt info */
  memset(&s->st20_pkt_info[0], 0,
         sizeof(struct st20_packet_group_info) * ST20_PKT_TYPE_MAX);

  /* 4800 if 1080p yuv422 */
  /* Calculate bytes per line, rounding up if there's a remainder */
  size_t raw_bytes_size = (size_t)ops->width * s->st20_pg.size;
  s->st20_bytes_in_line =
      (raw_bytes_size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
  /* rtp mode only  */
  s->rtp_pkt_max_size = ops->rtp_pkt_size;

  if (st22_frame_ops) { /* st22 frame mode */
    int max_data_len =
        impl->pkt_udp_suggest_max_size - sizeof(struct st22_rfc9134_rtp_hdr);
    uint32_t align = 128;
    max_data_len = max_data_len / align * align;
    s->st20_total_pkts = s->st20_frame_size / max_data_len;
    if (s->st20_frame_size % max_data_len) s->st20_total_pkts++;
    s->st20_pkt_len = max_data_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st22_rfc9134_rtp_hdr);
    /* assume all are normal */
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = s->st20_total_pkts;
  } else if (type == ST20_TYPE_RTP_LEVEL) { /* rtp path */
    s->st20_total_pkts = ops->rtp_frame_total_pkts;
    s->st20_pkt_size = ops->rtp_pkt_size + sizeof(struct mt_udp_hdr);
    s->st20_pkt_len = ops->rtp_pkt_size; /* not used in rtp, just set a value */
    /* assume all are normal */
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = s->st20_total_pkts;
  } else if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line */
    int bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc4175_video_hdr);
    s->st20_pkts_in_line = (s->st20_bytes_in_line / bytes_in_pkt) + 1;

    int pixel_in_pkt = (ops->width + s->st20_pkts_in_line - 1) / s->st20_pkts_in_line;
    s->st20_pkt_len =
        (pixel_in_pkt + s->st20_pg.coverage - 1) / s->st20_pg.coverage * s->st20_pg.size;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    s->st20_total_pkts = height * s->st20_pkts_in_line;

    int line_last_len = s->st20_bytes_in_line % s->st20_pkt_len;
    if (line_last_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number = height;
      s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].size =
          line_last_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number;
    dbg("%s(%d),  line_last_len: %d\n", __func__, idx, line_last_len);
  } else if (ops->packing == ST20_PACKING_BPM) {
    if (ST_VIDEO_BPM_SIZE % s->st20_pg.size) {
      err("%s(%d), bpm size 1260 can not be divide by pg size %u\n", __func__, idx,
          s->st20_pg.size);
      return -EIO;
    }
    s->st20_pkt_len = ST_VIDEO_BPM_SIZE;
    int last_pkt_len = s->st20_frame_size % s->st20_pkt_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    s->st20_total_pkts = ceil((double)s->st20_frame_size / s->st20_pkt_len);
    int bytes_per_pkt = s->st20_pkt_len;
    int temp = s->st20_bytes_in_line;
    while (temp % bytes_per_pkt != 0 && temp <= s->st20_frame_size) {
      temp += s->st20_bytes_in_line;
    }
    int none_extra_lines = ceil((double)s->st20_frame_size / temp);
    int extra_pkts = height - none_extra_lines;
    if (extra_pkts) {
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = extra_pkts;
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
          s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
    }
    if (last_pkt_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number = 1;
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].size =
          last_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number -
        s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
    dbg("%s(%d),  extra_pkts: %d\n", __func__, idx, extra_pkts);
  } else if (ops->packing == ST20_PACKING_GPM) {
    int max_data_len = impl->pkt_udp_suggest_max_size -
                       sizeof(struct st20_rfc4175_rtp_hdr) -
                       sizeof(struct st20_rfc4175_extra_rtp_hdr);
    uint32_t align = s->st20_pg.size * 2;
    max_data_len = max_data_len / align * align;
    int pg_per_pkt = max_data_len / s->st20_pg.size;
    s->st20_total_pkts =
        (ceil)((double)ops->width * height / (s->st20_pg.coverage * pg_per_pkt));
    s->st20_pkt_len = pg_per_pkt * s->st20_pg.size;
    int last_pkt_len = s->st20_frame_size % s->st20_pkt_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    int bytes_per_pkt = s->st20_pkt_len;
    int temp = s->st20_bytes_in_line;
    while (temp % bytes_per_pkt != 0 && temp <= s->st20_frame_size) {
      temp += s->st20_bytes_in_line;
    }
    int none_extra_lines = ceil((double)s->st20_frame_size / temp);
    int extra_pkts = height - none_extra_lines;
    if (extra_pkts) {
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = extra_pkts;
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
          s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
    }
    if (last_pkt_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number = 1;
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].size =
          last_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number -
        s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
    dbg("%s(%d),  extra_pkts: %d\n", __func__, idx, extra_pkts);
  } else {
    err("%s(%d), invalid packing mode %d\n", __func__, idx, ops->packing);
    return -EIO;
  }

  if (s->st20_pkt_size > ST_PKT_MAX_ETHER_BYTES) {
    err("%s(%d), invalid st20 pkt size %d\n", __func__, idx, s->st20_pkt_size);
    return -EIO;
  }

  return 0;
}

static int tv_uinit(struct st_tx_video_session_impl* s) {
  tv_uinit_rtcp(s);
  /* must uinit hw firstly as frame use shared external buffer */
  tv_uinit_hw(s);
  tv_uinit_sw(s);
  return 0;
}

static int tv_attach(struct mtl_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                     struct st_tx_video_session_impl* s, struct st20_tx_ops* ops,
                     enum mt_handle_type s_type, struct st22_tx_ops* st22_frame_ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  ret = st20_get_pgroup(ops->fmt, &s->st20_pg);
  if (ret < 0) {
    err("%s(%d), st20_get_pgroup fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_get_fps_timing(ops->fps, &s->fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  s->impl = impl;
  s->mgr = mgr;
  /* mark the queue to fatal error if burst fail exceed tx_hang_detect_time_thresh */
  if (ops->tx_hang_detect_ms)
    s->tx_hang_detect_time_thresh = ops->tx_hang_detect_ms * NS_PER_MS;
  else
    s->tx_hang_detect_time_thresh = NS_PER_S;

  /* Calculate bytes per line, rounding up if there's a remainder */
  size_t raw_bytes_size = (size_t)ops->width * s->st20_pg.size;
  s->st20_linesize = (raw_bytes_size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
  if (ops->linesize > s->st20_linesize)
    s->st20_linesize = ops->linesize;
  else if (ops->linesize) {
    err("%s(%d), invalid linesize %u\n", __func__, idx, ops->linesize);
    return -EINVAL;
  }

  uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
  if (st22_frame_ops) {
    if (st22_frame_ops->flags & ST22_TX_FLAG_DISABLE_BOXES)
      s->st22_box_hdr_length = 0;
    else
      s->st22_box_hdr_length = sizeof(struct st22_boxes);
    s->st22_codestream_size = st22_frame_ops->framebuff_max_size;
    s->st20_frame_size = s->st22_codestream_size + s->st22_box_hdr_length;
    s->st20_fb_size = s->st20_frame_size;
    info("%s(%d), st22 max codestream size %" PRId64 ", box len %u\n", __func__, idx,
         s->st22_codestream_size, s->st22_box_hdr_length);
  } else {
    s->st20_frame_size = ops->width * height * s->st20_pg.size / s->st20_pg.coverage;
    s->st20_fb_size = s->st20_linesize * height;
  }
  s->st20_frames_cnt = ops->framebuff_cnt;

  ret = tv_init_pkt(impl, s, ops, st22_frame_ops);
  if (ret < 0) {
    err("%s(%d), pkt init fail %d\n", __func__, idx, ret);
    return ret;
  }

  double frame_time = (double)s->fps_tm.den / s->fps_tm.mul;
  s->st21_vrx_narrow = RTE_MAX(8, s->st20_total_pkts / (27000 * frame_time));
  s->st21_vrx_wide = RTE_MAX(720, s->st20_total_pkts / (300 * frame_time));

  info("%s(%d), st21_vrx_narrow: %d, st21_vrx_wide: %d\n", __func__, idx,
       s->st21_vrx_narrow, s->st21_vrx_wide);

  s->st20_pkt_idx = 0;
  s->st20_seq_id = 0;
  s->st20_rtp_time = UINT32_MAX;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  if (ops->flags & ST20_TX_FLAG_DISABLE_BULK) {
    s->bulk = 1;
    info("%s(%d), bulk is disabled\n", __func__, idx);
  } else {
    s->bulk = RTE_MIN(4, ST_SESSION_MAX_BULK);
  }

  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "TX_VIDEO_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  s->s_type = s_type;
  for (int i = 0; i < num_port; i++) {
    s->st20_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx * 2);
    if (mt_user_random_src_port(impl))
      s->st20_src_port[i] = mt_random_port(s->st20_dst_port[i]);
    else
      s->st20_src_port[i] =
          (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st20_dst_port[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = mt_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = mt_if_has_multi_seg(impl, port);
    if (mt_pmd_is_dpdk_af_xdp(impl, port) && mt_user_af_xdp_zc(impl)) {
      /* enable zero copy for tx */
      s->mbuf_mempool_reuse_rx[i] = true;
    } else {
      s->mbuf_mempool_reuse_rx[i] = false;
    }
  }
  s->tx_mono_pool = mt_user_tx_mono_pool(impl);
  s->multi_src_port = mt_user_multi_src_port(impl);
  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  /* make sure the ring is smaller than total pkts */
  while (s->ring_count > s->st20_total_pkts) {
    s->ring_count /= 2;
  }

  if (st22_frame_ops) {
    /* no chain support for st22 since the pkts for each frame may be very small */
    s->tx_no_chain = true;
  } else {
    /* manually disable chain or any port can't support chain */
    s->tx_no_chain = mt_user_tx_no_chain(impl) || !tv_has_chain_buf(s) ||
                     !tv_pkts_capable_chain(impl, s);
  }
  if (s->tx_no_chain) {
    info("%s(%d), no chain mbuf support\n", __func__, idx);
  }

  enum mtl_port port;
  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    /* use system pacing way now */
    s->pacing_way[i] = st_tx_pacing_way(impl, port);
    /* use tsc for st22 since pkts for each frame is vary */
    if (st22_frame_ops && s->pacing_way[i] == ST21_TX_PACING_WAY_RL) {
      s->pacing_way[i] = ST21_TX_PACING_WAY_TSC;
    }
  }

  ret = tv_init_sw(impl, mgr, s, st22_frame_ops);
  if (ret < 0) {
    err("%s(%d), tv_init_sw fail %d\n", __func__, idx, ret);
    tv_uinit(s);
    return ret;
  }

  ret = tv_init_hw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_hw fail %d\n", __func__, idx, ret);
    tv_uinit(s);
    return ret;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tv_init_hdr(impl, s, i);
    if (ret < 0) {
      err("%s(%d), tx_session_init_hdr fail %d port %d\n", __func__, idx, ret, i);
      tv_uinit(s);
      return ret;
    }
  }

  if (ops->flags & ST20_TX_FLAG_ENABLE_RTCP) {
    ret = tv_init_rtcp(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), tx_session_init_rtcp fail %d\n", __func__, idx, ret);
      tv_uinit(s);
      return ret;
    }
  }

  ret = tv_init_pacing(impl, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_pacing fail %d\n", __func__, idx, ret);
    tv_uinit(s);
    return ret;
  }

  /* init vsync */
  s->vsync.meta.frame_time = s->pacing.frame_time;
  st_vsync_calculate(impl, &s->vsync);
  s->vsync.init = true;
  /* init advice sleep us */
  double sleep_ns = s->pacing.trs * 128;
  s->advice_sleep_us = sleep_ns / NS_PER_US;
  if (mt_user_tasklet_sleep(impl)) {
    info("%s(%d), advice sleep us %" PRIu64 "\n", __func__, idx, s->advice_sleep_us);
  }

  s->stat_lines_not_ready = 0;
  s->stat_user_busy = 0;
  s->stat_user_busy_first = true;
  s->stat_epoch_troffset_mismatch = 0;
  s->stat_trans_troffset_mismatch = 0;
  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = mt_get_monotonic_time();
  mt_stat_u64_init(&s->stat_time);

  for (int i = 0; i < num_port; i++) {
    s->inflight[i][0] = NULL;
    s->inflight_cnt[i] = 0;
    s->trs_inflight_num[i] = 0;
    s->trs_inflight_num2[i] = 0;
    s->trs_pad_inflight_num[i] = 0;
    s->trs_target_tsc[i] = 0;
    s->last_burst_succ_time_tsc[i] = mt_get_tsc(impl);
  }

  tv_init_pacing_epoch(impl, s);
  s->active = true;

  info("%s(%d), len %d(%d) total %d each line %d type %d flags 0x%x, %s\n", __func__, idx,
       s->st20_pkt_len, s->st20_pkt_size, s->st20_total_pkts, s->st20_pkts_in_line,
       ops->type, ops->flags, ops->interlaced ? "interlace" : "progressive");
  info("%s(%d), w %u h %u fmt %s packing %d pt %d, pacing way: %s\n", __func__, idx,
       ops->width, ops->height, st20_fmt_name(ops->fmt), ops->packing, ops->payload_type,
       st_tx_pacing_way_name(s->pacing_way[MTL_SESSION_PORT_P]));
  return 0;
}

void tx_video_session_clear_cpu_busy(struct st_tx_video_session_impl* s) {
  s->cpu_busy_score = 0;
  rte_atomic32_set(&s->cbs_build_timeout, 0);
}

void tx_video_session_cal_cpu_busy(struct mtl_sch_impl* sch,
                                   struct st_tx_video_session_impl* s) {
  uint64_t avg_ns_per_loop = mt_sch_avg_ns_loop(sch);
  int cbs_build_timeout;

  s->cpu_busy_score = (double)avg_ns_per_loop / s->bulk / s->pacing.trs * 100.0;

  /* build timeout check */
  cbs_build_timeout = rte_atomic32_read(&s->cbs_build_timeout);
  rte_atomic32_set(&s->cbs_build_timeout, 0);
  if (cbs_build_timeout > 10) {
    s->cpu_busy_score = 100.0; /* mark as busy */
    notice("%s(%d), mask as busy as build time out %d\n", __func__, s->idx,
           cbs_build_timeout);
  }

  s->stat_cpu_busy_score = s->cpu_busy_score;
}

static void tv_stat(struct st_tx_video_sessions_mgr* mgr,
                    struct st_tx_video_session_impl* s) {
  int m_idx = mgr->idx, idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frame_cnt = rte_atomic32_read(&s->stat_frame_cnt);
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->stat_frame_cnt, 0);

  notice("TX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d:%d inflight %d:%d\n",
         m_idx, idx, s->ops_name, framerate, frame_cnt,
         s->stat_pkts_build[MTL_SESSION_PORT_P], s->stat_pkts_build[MTL_SESSION_PORT_R],
         s->trs_inflight_cnt[0], s->inflight_cnt[0]);
  notice("TX_VIDEO_SESSION(%d,%d): throughput %f Mb/s: %f Mb/s, cpu busy %f\n", m_idx,
         idx,
         (double)s->stat_bytes_tx[MTL_SESSION_PORT_P] * 8 / time_sec / MTL_STAT_M_UNIT,
         (double)s->stat_bytes_tx[MTL_SESSION_PORT_R] * 8 / time_sec / MTL_STAT_M_UNIT,
         s->stat_cpu_busy_score);
  s->stat_last_time = cur_time_ns;
  s->stat_pkts_build[MTL_SESSION_PORT_P] = 0;
  s->stat_pkts_build[MTL_SESSION_PORT_R] = 0;
  s->stat_pkts_burst = 0;
  s->trs_inflight_cnt[0] = 0;
  s->inflight_cnt[0] = 0;
  s->stat_bytes_tx[MTL_SESSION_PORT_P] = 0;
  s->stat_bytes_tx[MTL_SESSION_PORT_R] = 0;

  if (s->stat_pkts_dummy) {
    dbg("TX_VIDEO_SESSION(%d,%d): dummy pkts %u, burst %u\n", m_idx, idx,
        s->stat_pkts_dummy, s->stat_pkts_burst_dummy);
    s->stat_pkts_dummy = 0;
    s->stat_pkts_burst_dummy = 0;
  }

  if (s->stat_epoch_troffset_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): mismatch epoch troffset %u\n", m_idx, idx,
           s->stat_epoch_troffset_mismatch);
    s->stat_epoch_troffset_mismatch = 0;
  }
  if (s->stat_trans_troffset_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): transmitter mismatch troffset %u\n", m_idx, idx,
           s->stat_trans_troffset_mismatch);
    s->stat_trans_troffset_mismatch = 0;
  }
  if (s->stat_trans_recalculate_warmup) {
    notice("TX_VIDEO_SESSION(%d,%d): transmitter recalculate warmup %u\n", m_idx, idx,
           s->stat_trans_recalculate_warmup);
    s->stat_trans_recalculate_warmup = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_VIDEO_SESSION(%d,%d): epoch drop %u\n", m_idx, idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_epoch_onward) {
    notice("TX_VIDEO_SESSION(%d,%d): epoch onward %u\n", m_idx, idx,
           s->stat_epoch_onward);
    s->stat_epoch_onward = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_VIDEO_SESSION(%d,%d): build timeout frames %u\n", m_idx, idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (s->stat_error_user_timestamp) {
    notice("TX_VIDEO_SESSION(%d,%d): error user timestamp %u\n", m_idx, idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }
  if (s->stat_user_busy) {
    notice("TX_VIDEO_SESSION(%d,%d): busy as no ready frame from user %u\n", m_idx, idx,
           s->stat_user_busy);
    s->stat_user_busy = 0;
  }
  if (s->stat_lines_not_ready) {
    notice("TX_VIDEO_SESSION(%d,%d): query new lines but app not ready %u\n", m_idx, idx,
           s->stat_lines_not_ready);
    s->stat_lines_not_ready = 0;
  }
  if (s->stat_vsync_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): vsync mismatch cnt %u\n", m_idx, idx,
           s->stat_vsync_mismatch);
    s->stat_vsync_mismatch = 0;
  }
  if (s->stat_pkts_chain_realloc_fail) {
    notice("TX_VIDEO_SESSION(%d,%d): chain pkt realloc fail cnt %u\n", m_idx, idx,
           s->stat_pkts_chain_realloc_fail);
    notice("TX_VIDEO_SESSION(%d,%d): SERIOUS MEMORY ISSUE!\n", m_idx, idx);
    s->stat_pkts_chain_realloc_fail = 0;
  }
  if (frame_cnt <= 0) {
    warn("TX_VIDEO_SESSION(%d,%d:%s): build ret %d, trs ret %d:%d\n", m_idx, idx,
         s->ops_name, s->stat_build_ret_code, s->stat_trs_ret_code[MTL_SESSION_PORT_P],
         s->stat_trs_ret_code[MTL_SESSION_PORT_R]);
  }
  if (s->stat_user_meta_cnt || s->stat_user_meta_pkt_cnt) {
    notice("TX_VIDEO_SESSION(%d,%d): user meta %u pkt %u\n", m_idx, idx,
           s->stat_user_meta_cnt, s->stat_user_meta_pkt_cnt);
    s->stat_user_meta_cnt = 0;
    s->stat_user_meta_pkt_cnt = 0;
  }
  if (s->stat_recoverable_error) {
    notice("TX_VIDEO_SESSION(%d,%d): recoverable_error %u \n", m_idx, idx,
           s->stat_recoverable_error);
    s->stat_recoverable_error = 0;
  }
  if (s->stat_unrecoverable_error) {
    err("TX_VIDEO_SESSION(%d,%d): unrecoverable_error %u \n", m_idx, idx,
        s->stat_unrecoverable_error);
    /* not reset unrecoverable_error */
  }
  if (s->ops.interlaced) {
    notice("TX_VIDEO_SESSION(%d,%d): interlace first field %u second field %u\n", m_idx,
           idx, s->stat_interlace_first_field, s->stat_interlace_second_field);
    s->stat_interlace_first_field = 0;
    s->stat_interlace_second_field = 0;
  }

  /* check frame busy stat */
  if (s->st20_frames) {
    struct st_frame_trans* frame_info;
    int frames_in_trans = 0;
    uint16_t framebuff_cnt = s->ops.framebuff_cnt;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame_info = &s->st20_frames[i];
      if (rte_atomic32_read(&frame_info->refcnt)) frames_in_trans++;
    }
    if ((frames_in_trans > 2) || (frames_in_trans >= framebuff_cnt)) {
      notice("TX_VIDEO_SESSION(%d,%d): %d frames are in trans, total %u\n", m_idx, idx,
             frames_in_trans, framebuff_cnt);
    }
  }

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("TX_VIDEO_SESSION(%d,%d): tasklet time avg %.2fus max %.2fus min %.2fus\n",
           m_idx, idx, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_next_frame_us > 8 || s->stat_max_notify_frame_us > 8) {
    notice("TX_VIDEO_SESSION(%d,%d): get next frame max %uus, notify done max %uus\n",
           m_idx, idx, s->stat_max_next_frame_us, s->stat_max_notify_frame_us);
  }
  s->stat_max_next_frame_us = 0;
  s->stat_max_notify_frame_us = 0;
}

static int tv_detach(struct st_tx_video_sessions_mgr* mgr,
                     struct st_tx_video_session_impl* s) {
  tv_stat(mgr, s);
  tv_uinit(s);
  return 0;
}

static int tv_init(struct st_tx_video_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static struct st_tx_video_session_impl* tv_mgr_attach(
    struct mtl_sch_impl* sch, struct st20_tx_ops* ops, enum mt_handle_type s_type,
    struct st22_tx_ops* st22_frame_ops) {
  struct st_tx_video_sessions_mgr* mgr = &sch->tx_video_mgr;
  int midx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  int ret;
  struct st_tx_video_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    if (!tx_video_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = tv_init(s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = tv_attach(impl, mgr, s, ops, s_type, st22_frame_ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    tx_video_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int tv_mgr_detach(struct st_tx_video_sessions_mgr* mgr,
                         struct st_tx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tv_detach(mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  tx_video_session_put(mgr, idx);

  return 0;
}

static int tv_update_dst(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                         struct st_tx_dest_info* dst) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st20_tx_ops* ops = &s->ops;

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->dip_addr[i], dst->dip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = dst->udp_port[i];
    s->st20_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx * 2);
    s->st20_dst_port[i] =
        (ops->udp_src_port[i]) ? (ops->udp_src_port[i]) : s->st20_dst_port[i];

    /* update hdr */
    ret = tv_init_hdr(impl, s, i);
    if (ret < 0) {
      err("%s(%d), init hdr fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  return 0;
}

static int tv_mgr_update_dst(struct st_tx_video_sessions_mgr* mgr,
                             struct st_tx_video_session_impl* s,
                             struct st_tx_dest_info* dst) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = tx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = tv_update_dst(mgr->parent, s, dst);
  tx_video_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int tv_mgr_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                       struct st_tx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;
  int i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc4175_video_hdr) != 62);
  RTE_BUILD_BUG_ON(sizeof(struct st_rfc3550_hdr) != 54);
  RTE_BUILD_BUG_ON(sizeof(struct st22_rfc9134_video_hdr) != 58);
  RTE_BUILD_BUG_ON(sizeof(struct st22_boxes) != 60);

  mgr->parent = impl;
  mgr->idx = idx;

  for (i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_video_sessions_mgr";
  ops.start = tv_tasklet_start;
  ops.handler = tvs_tasklet_handler;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tv_mgr_uinit(struct st_tx_video_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_tx_video_session_impl* s;

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    s = tx_video_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tv_mgr_detach(mgr, s);
    tx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int tv_mgr_update(struct st_tx_video_sessions_mgr* mgr) {
  int max_idx = 0;
  struct mtl_main_impl* impl = mgr->parent;
  uint64_t sleep_us = mt_sch_default_sleep_us(impl);
  struct st_tx_video_session_impl* s;

  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    s = mgr->sessions[i];
    if (!s) continue;
    max_idx = i + 1;
    sleep_us = RTE_MIN(s->advice_sleep_us, sleep_us);
  }
  dbg("%s(%d), sleep us %" PRIu64 ", max_idx %d\n", __func__, mgr->idx, sleep_us,
      max_idx);
  mgr->max_idx = max_idx;
  if (mgr->tasklet) mt_tasklet_set_sleep(mgr->tasklet, sleep_us);
  return 0;
}

static int tv_sessions_stat(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct st_tx_video_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_video_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    tv_stat(mgr, s);
    tx_video_session_put(mgr, j);
  }

  return 0;
}

int st_tx_video_sessions_sch_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->tx_video_init) return 0;

  /* create tx video context */
  struct st_tx_video_sessions_mgr* tx_video_mgr = &sch->tx_video_mgr;
  ret = tv_mgr_init(impl, sch, tx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_tv_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_video_transmitter_init(impl, sch, tx_video_mgr, &sch->video_transmitter);
  if (ret < 0) {
    tv_mgr_uinit(tx_video_mgr);
    err("%s(%d), st_video_transmitter_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  mt_stat_register(impl, tv_sessions_stat, tx_video_mgr, "tx_video");
  sch->tx_video_init = true;
  return 0;
}

int st_tx_video_sessions_sch_uinit(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  if (!sch->tx_video_init) return 0;

  struct st_tx_video_sessions_mgr* tx_video_mgr = &sch->tx_video_mgr;

  mt_stat_unregister(impl, tv_sessions_stat, tx_video_mgr);
  st_video_transmitter_uinit(&sch->video_transmitter);
  tv_mgr_uinit(tx_video_mgr);
  sch->tx_video_init = false;

  return 0;
}

int st_tx_video_session_migrate(struct st_tx_video_sessions_mgr* mgr,
                                struct st_tx_video_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
  tv_init(s, idx);
  return 0;
}

static int tv_ops_check(struct st20_tx_ops* ops) {
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

  if (st20_is_frame_type(ops->type)) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
    if (ops->type == ST20_TYPE_SLICE_LEVEL) {
      if (!ops->query_frame_lines_ready) {
        err("%s, pls set query_frame_lines_ready\n", __func__);
        return -EINVAL;
      }
    }
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (ops->rtp_frame_total_pkts <= 0) {
      err("%s, invalid rtp_frame_total_pkts %d\n", __func__, ops->rtp_frame_total_pkts);
      return -EINVAL;
    }
    if (!mt_rtp_len_valid(ops->rtp_pkt_size)) {
      err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
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

  if (ops->flags & ST20_TX_FLAG_EXACT_USER_PACING &&
      !(ops->flags & ST20_TX_FLAG_USER_PACING)) {
    err("%s, invalid flags 0x%x, need set USER_PACING with EXACT_USER_PACING\n", __func__,
        ops->flags);
    return -EINVAL;
  }

  return 0;
}

static int tv_st22_ops_check(struct st22_tx_ops* ops) {
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

  if (ops->type == ST22_TYPE_FRAME_LEVEL) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST22_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST22_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (ops->pack_type != ST22_PACK_CODESTREAM) {
      err("%s, invalid pack_type %d\n", __func__, ops->pack_type);
      return -EINVAL;
    }
    if (!ops->framebuff_max_size) {
      err("%s, pls set framebuff_max_size\n", __func__);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!mt_rtp_len_valid(ops->rtp_pkt_size)) {
      err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
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

int st20_tx_queue_fatal_error(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  int idx = s->idx;
  int ret;

  if (!mt_pmd_is_dpdk_user(impl, port)) {
    err("%s(%d,%d), not dpdk user pmd, nothing to do\n", __func__, s_port, idx);
    if (s->ops.notify_event) s->ops.notify_event(s->ops.priv, ST_EVENT_FATAL_ERROR, NULL);
    return 0;
  }

  if (!s->queue[s_port]) {
    err("%s(%d,%d), no queue\n", __func__, s_port, idx);
    return -EIO;
  }

  /* clear all tx ring buffer */
  if (s->packet_ring) mt_ring_dequeue_clean(s->packet_ring);
  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    if (s->ring[i]) mt_ring_dequeue_clean(s->ring[i]);
  }
  /* clean the queue done mbuf */
  mt_txq_done_cleanup(s->queue[s_port]);

  mt_txq_fatal_error(s->queue[s_port]);
  mt_txq_put(s->queue[s_port]);
  s->queue[s_port] = NULL;

  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.bytes_per_sec = tv_rl_bps(s);
  mt_pacing_train_bps_result_search(impl, port, flow.bytes_per_sec, &flow.bytes_per_sec);
  mtl_memcpy(&flow.dip_addr, &s->ops.dip_addr[s_port], MTL_IP_ADDR_LEN);
  flow.dst_port = s->ops.udp_port[s_port];
  s->queue[s_port] = mt_txq_get(impl, port, &flow);
  if (!s->queue[s_port]) {
    err("%s(%d,%d), get new txq fail\n", __func__, s_port, idx);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_unrecoverable_error);
    s->active = false; /* mark current session to dead */
    if (s->ops.notify_event) s->ops.notify_event(s->ops.priv, ST_EVENT_FATAL_ERROR, NULL);
    return -EIO;
  }
  uint16_t queue_id = mt_txq_queue_id(s->queue[s_port]);
  info("%s(%d,%d), new queue_id %u\n", __func__, s_port, idx, queue_id);

  /* cleanup frame manager (only valid for frame-type sessions) */
  if (st20_is_frame_type(s->ops.type)) {
    struct st_frame_trans* frame;
    for (uint16_t i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        info("%s(%d,%d), stop frame %u\n", __func__, s_port, idx, i);
        tv_notify_frame_done(s, i);
        rte_atomic32_dec(&frame->refcnt);
        rte_mbuf_ext_refcnt_set(&frame->sh_info, 0);
      }
    }
  }

  /* reset mempool */
  tv_mempool_free(s);
  s->recovery_idx++;
  ret = tv_mempool_init(impl, s->mgr, s);
  if (ret < 0) {
    err("%s(%d,%d), reset mempool fail\n", __func__, s_port, idx);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_unrecoverable_error);
    s->active = false; /* mark current session to dead */
    if (s->ops.notify_event) s->ops.notify_event(s->ops.priv, ST_EVENT_FATAL_ERROR, NULL);
    return ret;
  }

  /* point to next frame */
  s->st20_pkt_idx = 0;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  ST_SESSION_STAT_INC(s, port_user_stats, stat_recoverable_error);
  if (s->ops.notify_event)
    s->ops.notify_event(s->ops.priv, ST_EVENT_RECOVERY_ERROR, NULL);

  return 0;
}

/* only st20 frame mode has this callback */
int st20_frame_tx_start(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                        enum mtl_session_port s_port, struct st_frame_trans* frame) {
  if (!frame->user_meta_data_size) return 0;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  /* tx the user meta */
  struct rte_mbuf* pkt;
  struct st_rfc4175_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  struct rte_mempool* pool = mt_drv_no_sys_txq(impl, port)
                                 ? s->mbuf_mempool_hdr[s_port]
                                 : mt_sys_tx_mempool(impl, port);
  dbg("%s(%d,%d), start trans for frame %p\n", __func__, s->idx, port, frame);
  pkt = rte_pktmbuf_alloc(pool);

  dbg("%s(%d,%d), start trans for frame %p\n", __func__, s->idx, port, frame);
  if (!pkt) {
    err("%s(%d), pkt alloc fail\n", __func__, port);
    return -ENOMEM;
  }

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the basic hdrs: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[s_port], sizeof(*hdr));

  /* set timestamp */
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
  /* indicate it's user meta pkt */
  rtp->row_length = htons(frame->user_meta_data_size | ST20_LEN_USER_META);

  /* init mbuf with ipv4 */
  mt_mbuf_init_ipv4(pkt);

  /* copy user meta */
  void* payload = (uint8_t*)rtp + sizeof(struct st20_rfc4175_rtp_hdr);
  mtl_memcpy(payload, frame->user_meta, frame->user_meta_data_size);

  pkt->data_len = sizeof(struct st_rfc4175_video_hdr) + frame->user_meta_data_size;
  pkt->pkt_len = pkt->data_len;

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[s_port]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  uint16_t send = mt_drv_no_sys_txq(impl, port)
                      ? mt_txq_burst_busy(s->queue[s_port], &pkt, 1, 10)
                      : mt_sys_queue_tx_burst(impl, port, &pkt, 1);
  if (send < 1) {
    err("%s(%d), tx fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }
  ST_SESSION_STAT_INC(s, port_user_stats, stat_user_meta_pkt_cnt);

  return 0;
}

st20_tx_handle st20_tx_create(mtl_handle mt, struct st20_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tv_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }
  int height = ops->interlaced ? (ops->height >> 1) : ops->height;
  ret = st20_get_bandwidth_bps(ops->width, height, ops->fmt, ops->fps, ops->interlaced,
                               &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;
  if (!mt_user_quota_active(impl)) {
    if (ST20_TYPE_RTP_LEVEL == ops->type) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_TX1080P_RTP_PER_SCH;
    }
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST20_TX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST20_TX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, tx video sch init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  s = tv_mgr_attach(sch, ops, MT_HANDLE_TX_VIDEO, NULL);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

  s->st20_handle = s_impl;

  rte_atomic32_inc(&impl->st20_tx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st20_tx_set_ext_frame(st20_tx_handle handle, uint16_t idx,
                          struct st20_ext_frame* ext_frame) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  int s_idx;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  if (!ext_frame) {
    err("%s, NULL ext frame\n", __func__);
    return -EIO;
  }

  s = s_impl->impl;
  s_idx = s->idx;

  if (ext_frame->buf_len < s->st20_fb_size) {
    err("%s(%d), ext framebuffer size %" PRIu64 " can not hold frame, need %" PRIu64 "\n",
        __func__, s_idx, ext_frame->buf_len, s->st20_fb_size);
    return -EIO;
  }
  void* addr = ext_frame->buf_addr;
  if (!addr) {
    err("%s(%d), invalid ext frame address\n", __func__, s_idx);
    return -EIO;
  }
  rte_iova_t iova_addr = ext_frame->buf_iova;
  if (iova_addr == MTL_BAD_IOVA || iova_addr == 0) {
    err("%s(%d), invalid ext frame iova 0x%" PRIx64 "\n", __func__, s_idx, iova_addr);
    return -EIO;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (addr == s->st20_frames[i].addr) {
      warn_once("%s(%d), buffer %p still in tansport!\n", __func__, s_idx, addr);
    }
  }

  if (idx >= s->st20_frames_cnt) {
    err("%s(%d), invalid idx %d, should be in range [0, %d]\n", __func__, s_idx, idx,
        s->st20_frames_cnt);
    return -EIO;
  }
  if (!s->st20_frames) {
    err("%s(%d), st20_frames not valid\n", __func__, s_idx);
    return -EINVAL;
  }
  struct st_frame_trans* frame = &s->st20_frames[idx];
  int refcnt = rte_atomic32_read(&frame->refcnt);
  if (refcnt) {
    err("%s(%d), frame %d are not free, refcnt %d\n", __func__, s_idx, idx, refcnt);
    return -EINVAL;
  }
  if (!(frame->flags & ST_FT_FLAG_EXT)) {
    err("%s(%d), frame %d are not ext enabled\n", __func__, s_idx, idx);
    return -EINVAL;
  }

  frame->addr = addr;
  frame->iova = iova_addr;
  return 0;
}

void* st20_tx_get_framebuffer(st20_tx_handle handle, uint16_t idx) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames || !s->st20_frames[idx].addr) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st20_frames[idx].addr;
}

size_t st20_tx_get_framebuffer_size(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_fb_size;
}

int st20_tx_get_framebuffer_count(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  s = s_impl->impl;
  return s->st20_frames_cnt;
}

void* st20_tx_get_mbuf(st20_tx_handle handle, void** usrptr) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
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

int st20_tx_put_mbuf(st20_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!mt_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
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

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
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

int st20_tx_get_sch_idx(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st20_tx_get_pacing_params(st20_tx_handle handle, double* tr_offset_ns, double* trs_ns,
                              uint32_t* vrx_pkts) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  struct st_tx_video_session_impl* s = s_impl->impl;
  if (tr_offset_ns) *tr_offset_ns = s->pacing.tr_offset;
  if (trs_ns) *trs_ns = s->pacing.trs;
  if (vrx_pkts) *vrx_pkts = s->pacing.vrx;
  return 0;
}

int st20_tx_get_session_stats(st20_tx_handle handle, struct st20_tx_user_stats* stats) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_video_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st20_tx_reset_session_stats(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_tx_video_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}

int st20_tx_free(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct mtl_sch_impl* sch;
  struct st_tx_video_session_impl* s;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = tv_mgr_detach(&sch->tx_video_mgr, s);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) err("%s(%d,%d), st_tx_sessions_mgr_detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_tx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st20_tx_update_destination(st20_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s_impl->sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) return ret;

  ret = tv_mgr_update_dst(&s_impl->sch->tx_video_mgr, s, dst);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

st22_tx_handle st22_tx_create(mtl_handle mt, struct st22_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st22_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_tx_ops st20_ops;
  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tv_st22_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    ret = st22_rtp_bandwidth_bps(ops->rtp_frame_total_pkts, ops->rtp_pkt_size, ops->fps,
                                 &bps);
    if (ret < 0) {
      err("%s, rtp_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
    if (!mt_user_quota_active(impl)) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_TX1080P_RTP_PER_SCH;
    }
  } else {
    ret = st22_frame_bandwidth_bps(ops->framebuff_max_size, ops->fps, &bps);
    if (ret < 0) {
      err("%s, frame_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST22_TX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST22_TX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, tx video sch init fail fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  /* reuse st20 rtp type */
  memset(&st20_ops, 0, sizeof(st20_ops));
  st20_ops.name = ops->name;
  st20_ops.priv = ops->priv;
  st20_ops.num_port = ops->num_port;
  for (int i = 0; i < ops->num_port; i++) {
    memcpy(st20_ops.dip_addr[i], ops->dip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(st20_ops.port[i], MTL_PORT_MAX_LEN, "%s", ops->port[i]);
    st20_ops.udp_src_port[i] = ops->udp_src_port[i];
    st20_ops.udp_port[i] = ops->udp_port[i];
  }
  if (ops->flags & ST22_TX_FLAG_USER_P_MAC) {
    memcpy(&st20_ops.tx_dst_mac[MTL_SESSION_PORT_P][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_P][0], MTL_MAC_ADDR_LEN);
    st20_ops.flags |= ST20_TX_FLAG_USER_P_MAC;
  }
  if (ops->num_port > 1) {
    if (ops->flags & ST22_TX_FLAG_USER_R_MAC) {
      memcpy(&st20_ops.tx_dst_mac[MTL_SESSION_PORT_R][0],
             &ops->tx_dst_mac[MTL_SESSION_PORT_R][0], MTL_MAC_ADDR_LEN);
      st20_ops.flags |= ST20_TX_FLAG_USER_R_MAC;
    }
  }
  if (ops->flags & ST22_TX_FLAG_USER_PACING) st20_ops.flags |= ST20_TX_FLAG_USER_PACING;
  if (ops->flags & ST22_TX_FLAG_USER_TIMESTAMP)
    st20_ops.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  if (ops->flags & ST22_TX_FLAG_ENABLE_VSYNC) st20_ops.flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22_TX_FLAG_ENABLE_RTCP) {
    st20_ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
    st20_ops.rtcp = ops->rtcp;
  }
  if (ops->flags & ST22_TX_FLAG_DISABLE_BULK) st20_ops.flags |= ST20_TX_FLAG_DISABLE_BULK;
  st20_ops.pacing = ops->pacing;
  if (ST22_TYPE_RTP_LEVEL == ops->type)
    st20_ops.type = ST20_TYPE_RTP_LEVEL;
  else
    st20_ops.type = ST20_TYPE_FRAME_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.interlaced = ops->interlaced;
  st20_ops.fmt = ST20_FMT_YUV_422_10BIT;
  st20_ops.framebuff_cnt = ops->framebuff_cnt;
  st20_ops.payload_type = ops->payload_type;
  st20_ops.ssrc = ops->ssrc;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.rtp_frame_total_pkts = ops->rtp_frame_total_pkts;
  st20_ops.rtp_pkt_size = ops->rtp_pkt_size;
  st20_ops.notify_rtp_done = ops->notify_rtp_done;
  st20_ops.notify_event = ops->notify_event;
  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    s = tv_mgr_attach(sch, &st20_ops, MT_ST22_HANDLE_TX_VIDEO, NULL);
  } else {
    s = tv_mgr_attach(sch, &st20_ops, MT_ST22_HANDLE_TX_VIDEO, ops);
  }
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_ST22_HANDLE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st22_handle = s_impl;

  rte_atomic32_inc(&impl->st22_tx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st22_tx_free(st22_tx_handle handle) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct mtl_sch_impl* sch;
  struct st_tx_video_session_impl* s;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = tv_mgr_detach(&sch->tx_video_mgr, s);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) err("%s(%d,%d), st_tx_sessions_mgr_detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st22_tx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st22_tx_update_destination(st22_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s_impl->sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) return ret;

  ret = tv_mgr_update_dst(&s_impl->sch->tx_video_mgr, s, dst);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

void* st22_tx_get_mbuf(st22_tx_handle handle, void** usrptr) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
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

int st22_tx_put_mbuf(st22_tx_handle handle, void* mbuf, uint16_t len) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!mt_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
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

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
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

int st22_tx_get_sch_idx(st22_tx_handle handle) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

void* st22_tx_get_fb_addr(st22_tx_handle handle, uint16_t idx) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != MT_ST22_HANDLE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames || !s->st20_frames[idx].addr) {
    err("%s, st22_frames not allocated\n", __func__);
    return NULL;
  }

  if (s->st22_info) {
    return s->st20_frames[idx].addr + s->st22_box_hdr_length;
  } else {
    return s->st20_frames[idx].addr;
  }
}
