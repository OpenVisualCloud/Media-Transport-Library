/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_video_session.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../dev/mt_dev.h"
#include "../mt_handle_guard.h"
#include "../mt_log.h"
#include "../mt_rtcp.h"
#include "../mt_stat.h"
#include "../mt_util.h"
#include "st_err.h"
#include "st_fmt.h"
#include "st_tx_link.h"
#include "st_video_transmitter.h"

#define TV_RTP_PKT_MAX_SIZE (ST_PKT_MAX_ETHER_BYTES - sizeof(struct mt_udp_hdr))

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

static inline uint64_t tai_from_frame_count(struct st_tx_video_session_impl* s,
                                            uint64_t frame_count) {
  return st_epoch_to_tai(frame_count, s->fps_tm.mul, s->fps_tm.den);
}

/* transmission start time of the frame */
static inline uint64_t transmission_start_time(struct st_tx_video_session_impl* s,
                                               uint64_t frame_count) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  long double start_time = (long double)tai_from_frame_count(s, frame_count) +
                           pacing->tr_offset - (pacing->vrx * pacing->trs);
  if (start_time <= 0) return 0;
  if (start_time >= UINT64_MAX) return UINT64_MAX;
  return start_time;
}

static inline void pacing_set_mbuf_time_stamp(struct rte_mbuf* mbuf,
                                              struct st_tx_video_pacing* pacing) {
  st_tx_mbuf_set_tsc(mbuf, pacing->tsc_time_cursor);
  st_tx_mbuf_set_ptp(mbuf, pacing->ptp_time_cursor);
  st_tx_mbuf_set_vrx_room(mbuf, pacing->vrx_room_ns);
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

static void tv_notify_meta_done(struct st_tx_video_session_impl* s, uint16_t frame_idx,
                                struct st20_tx_frame_meta* tv_meta,
                                struct st22_tx_frame_meta* tx_st22_meta) {
  uint64_t tsc_start = 0;
  struct mtl_main_impl* impl = s->impl;
  bool time_measure = mt_sessions_time_measure(impl);
  if (time_measure) tsc_start = mt_get_tsc(impl);
  if (s->st22_info) {
    if (s->st22_info->notify_frame_done)
      s->st22_info->notify_frame_done(s->ops.priv, frame_idx, tx_st22_meta);
    MT_USDT_ST22_TX_FRAME_DONE(s->mgr->idx, s->idx, frame_idx,
                               tx_st22_meta->rtp_timestamp);
  } else {
    if (s->ops.notify_frame_done)
      s->ops.notify_frame_done(s->ops.priv, frame_idx, tv_meta);
    MT_USDT_ST20_TX_FRAME_DONE(s->mgr->idx, s->idx, frame_idx, tv_meta->rtp_timestamp);
  }
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
  }
}

static void tv_notify_frame_done(struct st_tx_video_session_impl* s, uint16_t frame_idx) {
  struct st_frame_trans* frame = &s->st20_frames[frame_idx];
  tv_notify_meta_done(s, frame_idx, &frame->tv_meta, &frame->tx_st22_meta);
}

/* This empty callback replaces the release callback on retired shared buffer
 * slots. It ignores late mbuf releases from dead or draining transmit queues. */
static void tx_video_frame_retired_release_callback(void* addr, void* opaque) {
  MTL_MAY_UNUSED(addr);
  MTL_MAY_UNUSED(opaque);
}

/* hands the frame back to the app once no pkt and no builder counts on it */
static void tv_frame_put(struct st_frame_trans* frame_info, const char* caller) {
  struct st_tx_video_session_impl* s = frame_info->priv;
  int s_idx = s->idx, frame_idx = frame_info->idx;

  int refcnt = rte_atomic32_read(&frame_info->refcnt);
  if (refcnt != 1) {
    warn("%s(%d), frame %d err refcnt %d sh_cur %u, from %s\n", __func__, s_idx,
         frame_idx, refcnt, rte_mbuf_ext_refcnt_read(frame_info->sh_cur), caller);
    return;
  }

  /* a builder may rewrite the frame's meta once it is released */
  if (s->st22_info)
    frame_info->done_st22_meta = frame_info->tx_st22_meta;
  else
    frame_info->done_tv_meta = frame_info->tv_meta;
  /* clear while still owned: a re-arm can only come after the release below */
  if (frame_info->flags & ST_FT_FLAG_EXT) {
    frame_info->done_addr = frame_info->addr;
    frame_info->addr = NULL;
    frame_info->iova = 0;
  }
  /* x86 rte_atomic32_dec() is no compiler barrier: keep the clear before it */
  rte_atomic_thread_fence(rte_memory_order_release);
  rte_atomic32_dec(&frame_info->refcnt);
  /* the app or st20p may re-arm and re-queue the frame from the callback */
  tv_notify_meta_done(s, frame_idx, &frame_info->done_tv_meta,
                      &frame_info->done_st22_meta);
  /* limit: the frame completing again inside this callback reuses the done_ fields */
  if (frame_info->flags & ST_FT_FLAG_EXT) frame_info->done_addr = NULL;

  dbg("%s(%d), succ frame_idx %d\n", __func__, s_idx, frame_idx);
}

/* DPDK calls this callback when the last chain mbuf drops a shared external
 * buffer reference count to zero. The callback releases the frame for reuse. */
static void tx_video_frame_release_callback(void* addr, void* opaque) {
  struct st_frame_trans* frame_info = opaque;
  struct st_tx_video_session_impl* s = frame_info->priv;

  if ((addr < frame_info->addr) || (addr >= (frame_info->addr + s->st20_fb_size))) {
    err("%s(%d), addr %p does not belong to frame %d\n", __func__, s->idx, addr,
        frame_info->idx);
    return;
  }
  tv_frame_put(frame_info, "the free of its last mbuf");
}

/* the builder counts on the frame it builds: the TX can drop each pkt at once */
static void tv_frame_build_hold(struct st_tx_video_session_impl* s,
                                struct st_frame_trans* frame) {
  if (!s->tx_no_chain) rte_mbuf_ext_refcnt_update(frame->sh_cur, 1);
}

static void tv_frame_build_put(struct st_tx_video_session_impl* s,
                               struct st_frame_trans* frame) {
  if (s->tx_no_chain || !rte_mbuf_ext_refcnt_update(frame->sh_cur, -1))
    tv_frame_put(frame, __func__);
}

/* a frame the builder stops: no callback, the caller hands it back */
static void tv_frame_build_drop(struct st_tx_video_session_impl* s) {
  if (s->st20_frame_stat != ST21_TX_STAT_SENDING_PKTS || s->tx_no_chain ||
      !s->st20_frames)
    return;
  rte_mbuf_ext_refcnt_update(s->st20_frames[s->st20_frame_idx].sh_cur, -1);
}

static rte_iova_t tv_frame_get_page_iova(struct st_tx_video_session_impl* s,
                                         struct st_frame_trans* frame_info,
                                         size_t offset) {
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

static inline rte_iova_t tv_frame_get_offset_iova(struct st_tx_video_session_impl* s,
                                                  struct st_frame_trans* frame_info,
                                                  size_t offset) {
  if (frame_info->page_table_len == 0) return frame_info->iova + offset;
  return tv_frame_get_page_iova(s, frame_info, offset);
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

    for (int j = 0; j < (int)RTE_DIM(frame_info->sh_info); j++) {
      frame_info->sh_info[j].free_cb = tx_video_frame_release_callback;
      frame_info->sh_info[j].fcb_opaque = frame_info;
      rte_mbuf_ext_refcnt_set(&frame_info->sh_info[j], 0);
    }
    frame_info->sh_cur = &frame_info->sh_info[0];

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
        mt_memcpy(frame, &st22_info->st22_boxes, max_copy_len);
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
    bool held = false;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      for (int j = 0; j < ST_FT_SH_INFO_MAX; j++) {
        if (!rte_mbuf_ext_refcnt_read(&frame->sh_info[j])) continue;
        frame->sh_info[j].free_cb = tx_video_frame_retired_release_callback;
        held = true;
      }
      st_frame_trans_uinit(frame, NULL);
    }

    /* a dead TX queue frees its mbufs at the port stop: their sh_info must stay */
    if (held)
      warn("%s(%d), TX queues hold mbufs of the frames, the sh_info stay\n", __func__,
           s->idx);
    else
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
      s->port_user_stats.stat_vsync_mismatch++;
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

/* a BPM or GPM pkt carries a second row header when it crosses a line end, unless it
 * holds the rest of the frame */
static inline bool tv_pkt_has_extra_hdr(struct st_tx_video_session_impl* s,
                                        uint32_t offset) {
  uint32_t end = offset + s->st20_pkt_len;
  return (end > (offset / s->st20_bytes_in_line + 1) * s->st20_bytes_in_line) &&
         (end < s->st20_frame_size);
}

/* the pad tv_train_pacing() sends for pkt i of a frame */
static enum st20_packet_type tv_train_pad_type(struct st_tx_video_session_impl* s,
                                               int i) {
  if ((s->ops.type == ST20_TYPE_RTP_LEVEL) || (s->s_type == MT_ST22_HANDLE_TX_VIDEO))
    return ST20_PKT_TYPE_NORMAL;
  /* a GPM_SL line ends in a shorter pkt unless its bytes split evenly */
  if (s->ops.packing == ST20_PACKING_GPM_SL)
    return (s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number &&
            i % s->st20_pkts_in_line == s->st20_pkts_in_line - 1)
               ? ST20_PKT_TYPE_LINE_TAIL
               : ST20_PKT_TYPE_NORMAL;
  /* frame type, as tv_build_st20() packs it: the last pkt holds the rest of the frame */
  uint32_t offset = s->st20_pkt_len * i;
  if (offset + s->st20_pkt_len > s->st20_frame_size) return ST20_PKT_TYPE_FRAME_TAIL;
  return tv_pkt_has_extra_hdr(s, offset) ? ST20_PKT_TYPE_EXTRA : ST20_PKT_TYPE_NORMAL;
}

/* the pads a tv_train_pacing() burst sends, as one pad a call is CPU bound above ~38
 * Gb/s */
#define TV_TRAIN_BURST (32)

/* the pads of a frame tv_train_pacing() sends, the remain NORMAL ones included */
static void tv_train_mix(struct st_tx_video_session_impl* s,
                         struct mt_pacing_pad_mix* mix) {
  int total = s->st20_total_pkts;
  int remain = TV_TRAIN_BURST - (total % TV_TRAIN_BURST);

  mix->normal = s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
  mix->pkts = total + remain;
  mix->bytes = (uint64_t)remain * mix->normal;
  for (int i = 0; i < total; i++)
    mix->bytes += s->st20_pkt_info[tv_train_pad_type(s, i)].size;
}

/* measured only: the shaper sends pad_interval + 1 pkts in pad_interval trs */
static void tv_set_measured_pad_interval(struct st_tx_video_session_impl* s,
                                         enum mtl_session_port s_port,
                                         float pad_interval) {
  struct mt_pacing_pad_mix mix;

  tv_train_mix(s, &mix);
  s->pacing.pad_interval = pad_interval;
  /* the shaper drains a pad by its bytes: training measured the mean of a frame's pads,
   * this is the NORMAL one's */
  s->rl_plan[s_port].pad_ns = s->pacing.trs * pad_interval / (pad_interval + 1) *
                              mix.normal * mix.pkts / mix.bytes;
}

/* the bytes/s programmed on a queue with its own txq, 0 without */
static uint64_t tv_queue_bps(struct mt_txq_entry* queue) {
  return queue && queue->txq ? queue->txq->bps : 0;
}

/* burst bucket of the port's RL shaper in bytes, 0 when unknown */
static uint32_t tv_rl_burst_size(struct mtl_main_impl* impl, enum mtl_port port) {
  uint32_t size = mt_get_user_params(impl)->port_params[port].rl_burst_size;

  switch (mt_if(impl, port)->drv_info.drv_type) {
    case MT_DRV_IAVF:
      return ST_TX_VIDEO_RL_VF_BURST_SIZE;
    case MT_DRV_ICE: {
      /* what ice_cfg_rl_burst_size() programs, its ice_round_to_num() rounding to the
       * nearest multiple; without the devarg the PMD keeps 15 KB */
      if (!size) return 0;
      uint32_t rounded = RTE_ALIGN_MUL_NEAR(size, 64);
      if (rounded <= 2047 * 64) return rounded;
      return RTE_MIN(RTE_ALIGN_MUL_NEAR(size, 1024), (uint32_t)(2047 * 1024));
    }
    default:
      return 0;
  }
}

/* tv_sync_pacing() snaps the start time to the RTP tick unless USER_TIMESTAMP,
 * RTP_TIMESTAMP_EPOCH or rtp_timestamp_delta_us decouple the RTP timestamp from it */
static bool tv_start_snaps(struct st_tx_video_session_impl* s) {
  return !(s->ops.flags &
           (ST20_TX_FLAG_USER_TIMESTAMP | ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH)) &&
         !s->ops.rtp_timestamp_delta_us;
}

/* the most tv_sync_pacing()'s 90 kHz snap moves the target past epoch + tr_offset -
 * vrx * trs, over the phases the epochs take on the tick grid; an exact user start time
 * is not snapped, the frames without one are */
static long double tv_snap_loss_ns(struct st_tx_video_session_impl* s) {
  uint32_t rate = s->fps_tm.sampling_clock_rate;
  uint64_t a = (uint64_t)rate * s->fps_tm.den, b = s->fps_tm.mul;
  long double loss = 0;

  if (!tv_start_snaps(s) || !rate || !b) return 0;
  while (a % b) {
    uint64_t r = a % b;
    a = b;
    b = r;
  }
  uint64_t phases = s->fps_tm.mul / b;
  if (phases > 64) return (long double)NS_PER_S / rate / 2;
  for (uint64_t k = phases; k < 2 * phases; k++) {
    uint64_t start = transmission_start_time(s, k);
    loss = RTE_MAX(loss, (long double)st_tai_round_to_media_clk_ns(start, rate) - start);
  }
  return loss;
}

/* how late of the target grid a pkt behind packet 0 leaves at most: the pad sawtooth,
 * (pad_interval / 2 + bulk) / (pad_interval + 1) trs, and one pad more where a frame's
 * whole pads round up */
static long double tv_rl_sawtooth_ns(struct st_tx_video_session_impl* s,
                                     long double pad) {
  struct st_tx_video_pacing* pacing = &s->pacing;

  return (pacing->pad_interval / 2 + (float)s->bulk) / (pacing->pad_interval + 1) *
             pacing->trs +
         pad;
}

/* Plans the RL pre-arm of each port from the pad_ns training measured and the rate on its
 * queue. Runs after tv_init_hw() built the trim pads, once warm_pkts and the pacing ways
 * are final. A port without a plan holds packet 0 until its target. */
static void tv_rl_plan_init(struct mtl_main_impl* impl,
                            struct st_tx_video_session_impl* s) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint32_t normal = s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
  uint16_t pad_size = st_tx_video_rl_pad_size(normal);
  int idx = s->idx;
  long double snap_loss = tv_snap_loss_ns(s);
  /* Packet j must reach the receiver by its drain slot, vrx trs after its place on the
   * target grid (ST 2110-21 6.6.2). The 90 kHz snap moves that grid later by up to
   * snap_loss, and the NIC and the wire take their share after the shaper. */
  long double slot = pacing->vrx * pacing->trs - snap_loss - ST_TX_VIDEO_RL_WIRE_NS;

  pacing->snap_loss_ns = (int32_t)snap_loss;
  for (int i = 0; i < s->ops.num_port; i++) {
    struct st_tx_video_rl_plan* plan = &s->rl_plan[i];
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    uint32_t burst_size = tv_rl_burst_size(impl, port);
    uint64_t bps = tv_queue_bps(s->queue[i]);
    bool armed = pacing->warm_pkts && s->pacing_way[i] == ST21_TX_PACING_WAY_RL &&
                 plan->pad_ns && burst_size && bps &&
                 s->trim_pad[i][ST_TX_VIDEO_RL_TRIM_PAD_COARSE];
    /* the in-frame pad is NORMAL, the pre-arm's drains by its own bytes */
    long double normal_ns = plan->pad_ns;

    if (armed) {
      plan->pad_ns = st_tx_video_rl_pad_ns(normal_ns, normal, pad_size);
      /* the timeslot grid releases packet 0 up to a timeslot early: aim one late */
      long double aim = st_tx_video_rl_tokens(bps) * plan->pad_ns / pad_size;
      long double timeslot =
          st_tx_video_rl_timeslot_tokens(bps) * plan->pad_ns / pad_size;
      /* the bucket fills at the programmed rate, not the trained one */
      long double refill = (long double)burst_size * NS_PER_S / bps;
      plan->bucket_ns = refill + aim;
      /* Behind packet 0 the pkts run late of the grid by up to the sawtooth. Check 1
       * takes packet 0 a fine trim and two aims late, its pads' and the timeslot's. The
       * rest of the slot may go to a stalled burst, or to what a late entry's tail cannot
       * cut, within what the grid and the cut leave of the launch budget. */
      long double late = tv_rl_sawtooth_ns(s, normal_ns);
      long double fine = plan->pad_ns * ST_TX_VIDEO_RL_TRIM_FINE / pad_size;
      long double lead = slot - late - 2 * aim - fine;
      plan->absorb_ns =
          RTE_MIN(ST_TX_VIDEO_RL_GRID_LATE_MAX_NS - aim - timeslot - fine, lead);
      plan->refill_ns = refill;
      /* the fewest pads that keep the shaper draining, past the bucket it sends at once,
       * through a tick in the next burst and that burst's own time to its doorbell */
      plan->anchor_pads = RTE_MIN(
          ceill((plan->bucket_ns + ST_TX_VIDEO_RL_TICK_NS + ST_TX_VIDEO_RL_STALL_NS) /
                plan->pad_ns),
          ST_TX_VIDEO_RL_MAX_PADS_PER_BURST);
      long double window = pacing->warm_pkts * pacing->trs + plan->bucket_ns;
      /* from the previous frame's last pkt to the warm-up open */
      long double idle =
          pacing->frame_time * (1 - pacing->reactive) - pacing->warm_pkts * pacing->trs;
      /* the window fits half the ring, clear of the PMD's lazy cleanup */
      armed = window / plan->pad_ns + 1 <= mt_if_nb_tx_desc(impl, port) / 2 &&
              /* it holds the trim_pads; video_trs_rl_pre_arm() checks a late entry */
              window > (st_tx_video_rl_trim_pads(pad_size) - 1) * plan->pad_ns &&
              /* the bucket refills before the window opens */
              refill <= idle &&
              /* the grid releases packet 0 within a timeslot of target + aim */
              aim + timeslot <= ST_TX_VIDEO_RL_GRID_LATE_MAX_NS;
    }
    /* A pkt that leaves the shaper up to this budget and its vrx_room_ns past its target
     * still reaches the receiver by its slot, the sawtooth taken too; EBU LIST reads VRX
     * 0 one trs later. Without a plan a pad drains within a trs. st22 sets its trs per
     * frame, and a budget below 0 would flag every frame: neither is judged. A port with
     * no budget warns, as does an armed one with no stall limit. */
    if (s->pacing_way[i] == ST21_TX_PACING_WAY_RL && pacing->vrx &&
        s->s_type != MT_ST22_HANDLE_TX_VIDEO) {
      plan->vrx_budget_ns = RTE_MAX(
          0, (int64_t)(slot - tv_rl_sawtooth_ns(s, armed ? normal_ns : pacing->trs)));
      if (!plan->vrx_budget_ns)
        warn("%s[%02d], port %d vrx %u leaves no VRX budget, VRX not judged\n", __func__,
             idx, i, pacing->vrx);
      else if (armed && plan->absorb_ns <= 0)
        warn("%s[%02d], port %d vrx %u leaves no stall limit, frames may be flagged\n",
             __func__, idx, i, pacing->vrx);
    }
    if (!armed) {
      plan->pad_ns = 0;
      plan->bucket_ns = 0;
      plan->absorb_ns = 0;
      plan->refill_ns = 0;
      plan->armed = false;
      if (s->pacing_way[i] == ST21_TX_PACING_WAY_RL)
        warn("%s[%02d], port %d no rl plan, holds packet 0, VRX at risk if late\n",
             __func__, idx, i);
      continue;
    }
    plan->pad_size = pad_size;
    plan->trim_pads = st_tx_video_rl_trim_pads(plan->pad_size);
    plan->armed = true;
    info("%s[%02d], port %d rl plan, pad_ns %Lf bucket_ns %Lf burst size %u bps %" PRIu64
         " pad_size %u\n",
         __func__, idx, i, plan->pad_ns, plan->bucket_ns, burst_size, bps,
         plan->pad_size);
  }
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
  struct mt_pacing_pad_mix mix;

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

  tv_train_mix(s, &mix);
  ret = mt_pacing_train_pad_result_search(impl, port, rl_bps, tv_queue_bps(queue), &mix,
                                          &pad_interval);
  if (ret >= 0) {
    tv_set_measured_pad_interval(s, s_port, pad_interval);
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
  int remain = TV_TRAIN_BURST - (total % TV_TRAIN_BURST);
  struct rte_mbuf* pads[TV_TRAIN_BURST];

  /* training stage */
  for (int loop = 0; loop < loop_frame; loop++) {
    uint64_t start = mt_get_ptp_time(impl, MTL_PORT_P);
    for (int i = 0; i < total + remain; i += TV_TRAIN_BURST) {
      for (int j = 0; j < TV_TRAIN_BURST; j++) {
        enum st20_packet_type type =
            i + j < total ? tv_train_pad_type(s, i + j) : ST20_PKT_TYPE_NORMAL;
        pads[j] = s->pad[s_port][type];
        rte_mbuf_refcnt_update(pads[j], 1);
      }
      mt_txq_burst_busy(queue, pads, TV_TRAIN_BURST, 10);
    }
    uint64_t end = mt_get_ptp_time(impl, MTL_PORT_P);
    double time = (double)(end - start) * total / (total + remain);
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
  /* the ring is still full of pads, the session paces as if its queue were idle */
  mt_sleep_us((unsigned int)((double)pad_pkts / pkts_per_sec * US_PER_S + 1));

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
    tv_set_measured_pad_interval(s, s_port, pad_interval);
    mt_pacing_train_pad_result_add(impl, port, rl_bps, tv_queue_bps(queue), &mix,
                                   pad_interval);
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
  bps_to_set = INCREASE_BPS_FACTOR * (double)rl_bps * (double)rl_bps / measured_bps;
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
  dbg("%s[%02d], frame_idle_time %Lf\n", __func__, idx, pacing->frame_idle_time);
  if (pacing->frame_idle_time < 0) {
    warn("%s[%02d], error frame_idle_time %Lf\n", __func__, idx, pacing->frame_idle_time);
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
        err("%s(%d), port %d rl training fail %d, fallback to tsc pacing\n", __func__,
            idx, i, ret);
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
    warm_pkts = RTE_MIN(warm_pkts, (uint32_t)128); /* limit to 128 pkts */
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
  tv_rl_plan_init(impl, s);
  info("%s[%02d], trs %Lf trOffset %Lf vrx %u warm_pkts %u frame time %Lfms fps %f\n",
       __func__, idx, pacing->trs, pacing->tr_offset, pacing->vrx, pacing->warm_pkts,
       pacing->frame_time / NS_PER_MS, st_frame_rate(s->ops.fps));
  /* resolve pacing tasklet */
  for (int i = 0; i < num_port; i++) {
    ret = st_video_resolve_pacing_tasklet(s, i);
    if (ret < 0) return ret;
  }

  return 0;
}

static int tv_reset_pacing_epoch(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s) {
  uint64_t ptp_time = mt_get_ptp_time(impl, MTL_PORT_P);
  struct st_tx_video_pacing* pacing = &s->pacing;
  pacing->cur_epochs = st_tai_to_epoch(ptp_time, s->fps_tm.mul, s->fps_tm.den);
  pacing->exact_start_tai = 0;
  pacing->user_start_tai = 0;
  return 0;
}

/* The epoch the next frame follows. After tv_drop_sw_pkts() and over a link drain, at
 * least the one of the time now: no frame takes an epoch again or starts late. */
static uint64_t tv_prev_epoch(struct st_tx_video_session_impl* s) {
  uint64_t cur = s->pacing.cur_epochs;

  if (likely(!s->pacing.epoch_resync && !s->tx_link_drain)) return cur;
  return RTE_MAX(cur, st_tai_to_epoch(mt_get_ptp_time(s->impl, MTL_PORT_P), s->fps_tm.mul,
                                      s->fps_tm.den));
}

static void validate_user_timestamp(struct st_tx_video_session_impl* s,
                                    uint64_t requested_time, uint64_t current_time,
                                    uint64_t max_future) {
  if (requested_time < current_time) {
    s->port_user_stats.common.stat_error_user_timestamp++;
    dbg("%s(%d), user requested transmission time in the past, required_tai %" PRIu64
        ", cur_tai %" PRIu64 "\n",
        __func__, s->idx, requested_time, current_time);
  } else if (requested_time - current_time > max_future) {
    dbg("%s(%d), requested time %" PRIu64 " too far in the future, current time %" PRIu64
        "\n",
        __func__, s->idx, requested_time, current_time);
    s->port_user_stats.common.stat_error_user_timestamp++;
  }
}

static inline uint64_t calc_frame_count_since_epoch(struct st_tx_video_session_impl* s,
                                                    uint64_t cur_tai,
                                                    uint64_t required_tai) {
  uint64_t frame_count_tai = st_tai_to_epoch(cur_tai, s->fps_tm.mul, s->fps_tm.den);
  uint64_t next_free_frame_slot;
  uint64_t frame_count;

  if (required_tai) {
    frame_count = st_tai_to_nearest_epoch(required_tai, s->fps_tm.mul, s->fps_tm.den);
    if (s->ops.flags & ST20_TX_FLAG_EXACT_USER_PACING) {
      validate_user_timestamp(s, required_tai, cur_tai, NS_PER_S);
    } else {
      /* Without EXACT only an epoch already in the past is counted here. Whether a
       * start is too far ahead depends on the interlace parity bump and the 90 kHz
       * rounding tv_sync_pacing() applies later, so tv_sync_pacing() checks 1 s. */
      if (frame_count < frame_count_tai)
        s->port_user_stats.common.stat_error_user_timestamp++;
    }
    /* frame_count 0 collides with the "unset" sentinel used elsewhere; fall back to
     * real time rather than honor a rounded-to-zero required_tai */
    if (!frame_count) frame_count = frame_count_tai;
    return frame_count;
  }

  if (s->pacing.cur_epochs == UINT64_MAX) return frame_count_tai;
  next_free_frame_slot = s->pacing.cur_epochs + 1;
  /* a user start PTP time has reached holds nothing back any more; clearing it also
   * keeps user_start_tai - cur_tai below from wrapping */
  if (s->pacing.user_start_tai <= cur_tai) s->pacing.user_start_tai = 0;

  if (frame_count_tai <= next_free_frame_slot) {
    /* There is time buffer until the next available frame time window */
    uint64_t onward = next_free_frame_slot - frame_count_tai;
    /* A user-paced start up to 1 s ahead can put the next free slot past the onward
     * range. Resyncing to now would schedule this frame about 1 s before the held
     * frame, yet the FIFO ring still sends it after that frame: unpaced, with its RTP
     * timestamp going back. So keep following the held frame. A PTP step back that
     * leaves the held start more than 1 s ahead still resyncs. A late frame (the
     * branch below) always resyncs, as nothing is held then. */
    bool user_start_held =
        s->pacing.user_start_tai && s->pacing.user_start_tai - cur_tai <= NS_PER_S;
    if (onward > s->pacing.max_onward_epochs && !user_start_held) {
      /* current time is out of onward range, resync to real time instead of
       * advancing by one epoch forever */
      dbg("%s(%d), onward range exceeded, next_free_frame_slot %" PRIu64
          ", frame_count_tai %" PRIu64 "\n",
          __func__, s->idx, next_free_frame_slot, frame_count_tai);
      s->port_user_stats.common.stat_epoch_onward += onward;
      frame_count = frame_count_tai;
    } else {
      frame_count = next_free_frame_slot;
      /* An EXACT frame sets cur_epochs to the epoch nearest its TAI. When that TAI is
       * more than TR_offset but less than half a frame past epoch N, the default start
       * of N + 1 comes less than one frame time after it, while the EXACT frame is
       * still on the wire. If this frame's start, after the 90 kHz rounding, is that
       * close, move it one epoch later; one epoch is enough, as an epoch is one frame
       * time (one field when interlaced). */
      if (s->pacing.exact_start_tai) {
        uint64_t start = transmission_start_time(s, frame_count);
        if (tv_start_snaps(s))
          start = st_tai_round_to_media_clk_ns(start, s->fps_tm.sampling_clock_rate);
        if (start < s->pacing.exact_start_tai + s->pacing.frame_time) frame_count++;
      }
    }

  } else {
    dbg("%s(%d), frame is late, frame_count_tai %" PRIu64 " next_free_frame_slot %" PRIu64
        "\n",
        __func__, s->idx, frame_count_tai, next_free_frame_slot);
    s->port_user_stats.common.stat_epoch_drop += (frame_count_tai - next_free_frame_slot);

    if (s->ops.notify_frame_late) {
      s->ops.notify_frame_late(s->ops.priv, frame_count_tai - next_free_frame_slot);
    }

    frame_count = frame_count_tai;
  }

  return frame_count;
}

/* the tightest VRX budget of the RL ports that judge one, 0 for none */
static int64_t tv_vrx_budget_ns(struct st_tx_video_session_impl* s) {
  int64_t budget_ns = 0;

  for (int i = 0; i < s->ops.num_port; i++) {
    int64_t b = s->rl_plan[i].vrx_budget_ns;
    if (s->pacing_way[i] == ST21_TX_PACING_WAY_RL && b && (!budget_ns || b < budget_ns))
      budget_ns = b;
  }
  return budget_ns;
}

static int tv_sync_pacing(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
                          uint64_t required_tai, bool second_field) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t cur_tai = mt_get_ptp_time(impl, MTL_PORT_P);
  uint64_t cur_tsc = mt_get_tsc(impl);
  uint64_t prev_epochs = pacing->cur_epochs;
  bool prev_epoch_resync = pacing->epoch_resync;
  uint64_t start_time_tai;
  uint64_t time_to_tx_ns;

  pacing->cur_epochs = tv_prev_epoch(s);
  pacing->epoch_resync = false;
  pacing->cur_epochs = calc_frame_count_since_epoch(s, cur_tai, required_tai);

  /* the app asked for an exact start time and actually supplied one */
  bool exact_user_pacing =
      (s->ops.flags & ST20_TX_FLAG_EXACT_USER_PACING) && required_tai;
  /* an ST22 RTP-level session would read its F bit off the first codestream byte: its
   * 16-byte rfc9134 header is not the struct st20_rfc4175_rtp_hdr this offset assumes */
  bool field_parity_known = !(s->s_type == MT_ST22_HANDLE_TX_VIDEO && !s->st22_info);
  /* ST 2110-21 6.2 frame grid: frame_time is one field, so even slot = first field.
   * Applied here; calc_frame_count_since_epoch() stays parity-blind. doc/design.md 6.6 */
  if (s->ops.interlaced && !exact_user_pacing && field_parity_known &&
      ((pacing->cur_epochs & 0x1) != second_field)) {
    pacing->cur_epochs++;
  }

  if (exact_user_pacing) {
    start_time_tai = required_tai;
  } else {
    start_time_tai = transmission_start_time(s, pacing->cur_epochs);
  }
  /* an exact user start is not snapped: its drain slot keeps the snap loss */
  pacing->vrx_room_ns = exact_user_pacing ? pacing->snap_loss_ns : 0;
  if (!exact_user_pacing && tv_start_snaps(s)) {
    uint64_t snapped =
        st_tai_round_to_media_clk_ns(start_time_tai, s->fps_tm.sampling_clock_rate);
    pacing->vrx_room_ns = pacing->snap_loss_ns - (int32_t)(snapped - start_time_tai);
    start_time_tai = snapped;
  }
  /* The transmitter does not wait more than 1 s for a target and would send the whole
   * frame unpaced at once, so a non-EXACT start more than 1 s ahead is refused like an
   * out-of-range EXACT TAI and counted. It is judged on the final start, after the
   * parity bump and the 90 kHz rounding. calc_frame_count_since_epoch() replaced
   * cur_epochs with the user's epoch, so put the previous one back; the frame then
   * gets the default pacing from the previous frame on. The inner call has
   * required_tai 0 and cannot take this branch: at most one recursion. */
  if (required_tai && !exact_user_pacing && start_time_tai > cur_tai &&
      start_time_tai - cur_tai > NS_PER_S) {
    dbg("%s(%d), start %" PRIu64 " more than 1s ahead of %" PRIu64 "\n", __func__, s->idx,
        start_time_tai, cur_tai);
    s->port_user_stats.common.stat_error_user_timestamp++;
    pacing->cur_epochs = prev_epochs;
    pacing->epoch_resync = prev_epoch_resync;
    return tv_sync_pacing(impl, s, 0, second_field);
  }
  /* see calc_frame_count_since_epoch(): exact_start_tai guards the next frame only,
   * user_start_tai stays until PTP time reaches it */
  pacing->exact_start_tai = exact_user_pacing ? required_tai : 0;
  if (required_tai) pacing->user_start_tai = start_time_tai;
  if (start_time_tai < cur_tai) {
    time_to_tx_ns = 0;
    uint64_t late = cur_tai - start_time_tai;
    int64_t budget_ns = tv_vrx_budget_ns(s);
    if (budget_ns && (int64_t)late > budget_ns + pacing->vrx_room_ns) {
      s->stat_rl.start_late++;
      if (late > s->stat_rl.start_late_max_ns) s->stat_rl.start_late_max_ns = late;
      MT_USDT_ST20_TX_RL_EVENT(s->mgr->idx, s->idx, ST_TX_VIDEO_RL_CAUSE_START_LATE, late,
                               0);
      pacing->vrx_room_ns = ST_TX_VIDEO_RL_ROOM_START_LATE;
    } else if (budget_ns) {
      /* check 1 judges the re-timed pkts of each port against the rest of its budget */
      pacing->vrx_room_ns -= (int32_t)late;
    }
  } else {
    time_to_tx_ns = start_time_tai - cur_tai;
  }

  /* tsc_time_cursor is important as it determines when first packet of the frame will be
   * send */
  if (time_to_tx_ns > UINT64_MAX - cur_tsc)
    pacing->tsc_time_cursor = UINT64_MAX;
  else
    pacing->tsc_time_cursor = cur_tsc + time_to_tx_ns;

  pacing->tsc_time_frame_start = pacing->tsc_time_cursor;
  pacing->ptp_time_cursor = start_time_tai;

  return 0;
}

static int tv_sync_pacing_st22(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s, uint64_t required_tai,
                               int pkts_in_frame, bool second_field) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  /* reset trs */
  pacing->trs = pacing->frame_time * pacing->reactive / pkts_in_frame;
  dbg("%s(%d), trs %Lf\n", __func__, s->idx, pacing->trs);
  return tv_sync_pacing(impl, s, required_tai, second_field);
}

/* Returns the TAI instant frame->rtp_timestamp was derived from, for the caller
 * to report as frame->timestamp -- see the per-branch comments below for why. */
static uint64_t tv_update_rtp_time_stamp(struct st_tx_video_session_impl* s,
                                         enum st10_timestamp_fmt tfmt,
                                         uint64_t timestamp) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t delta_ns = (uint64_t)s->ops.rtp_timestamp_delta_us * NS_PER_US;
  uint64_t tai_for_rtp_ts;

  if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
    /* Contract of ST20_TX_FLAG_USER_TIMESTAMP (st20_api.h): the RTP timestamp
     * is assigned verbatim from the app-supplied timestamp, independent of
     * how tv_sync_pacing() scheduled the frame (ptp_time_cursor may be
     * epoch-rounded and tr_offset/vrx adjusted). A MEDIA_CLK timestamp is
     * unwrapped against pacing->ptp_time_cursor first so it resolves to a
     * real TAI instant rather than one expanded from a zero-based clock;
     * delta_ns is then applied in the TAI/ns domain regardless of tfmt. */
    uint64_t tai_base =
        (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK)
            ? st10_media_clk_to_tai((uint64_t)pacing->ptp_time_cursor,
                                    (uint32_t)timestamp, s->fps_tm.sampling_clock_rate)
            : timestamp;
    tai_for_rtp_ts = tai_base + delta_ns;
    pacing->rtp_time_stamp =
        st10_tai_to_media_clk(tai_for_rtp_ts, s->fps_tm.sampling_clock_rate);
  } else {
    /* Not user-controlled: the RTP timestamp is the pacing-derived instant --
     * either the bare epoch (RTP_TIMESTAMP_EPOCH, omitting tr_offset) or the
     * scheduled TX cursor. rtp_timestamp_delta_us shifts only this value,
     * never the real TX schedule. */
    if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
      tai_for_rtp_ts = tai_from_frame_count(s, pacing->cur_epochs);
    } else {
      tai_for_rtp_ts = pacing->ptp_time_cursor;
    }
    tai_for_rtp_ts += delta_ns;
    pacing->rtp_time_stamp =
        st10_tai_to_media_clk(tai_for_rtp_ts, s->fps_tm.sampling_clock_rate);
  }
  dbg("%s(%d), rtp time stamp %u\n", __func__, s->idx, pacing->rtp_time_stamp);
  return tai_for_rtp_ts;
}

static int tv_init_next_meta(struct st_tx_video_session_impl* s,
                             struct st20_tx_frame_meta* meta) {
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
  meta->epoch = tv_prev_epoch(s) + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai_from_frame_count(s, meta->epoch);
  return 0;
}

static int tv_init_st22_next_meta(struct st_tx_video_session_impl* s,
                                  struct st22_tx_frame_meta* meta) {
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
  meta->epoch = tv_prev_epoch(s) + 1;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai_from_frame_count(s, meta->epoch);
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
  const uint32_t den = s->fps_tm.den == 1001 ? 2 : 1;
  const uint32_t num = s->fps_tm.den == 1001 ? s->fps_tm.mul / 1000 : s->fps_tm.mul;
  uint32_t frat = (s->ops.interlaced << 30) | ((den & 0x3f) << 24) |
                  ((num >> s->ops.interlaced) & 0xffff);
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
    mt_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == MTL_SESSION_PORT_R) && (ops->flags & ST20_TX_FLAG_USER_R_MAC)) {
    mt_memcpy(d_addr->addr_bytes, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
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
  mt_memcpy(&ipv4->src_addr, sip, MTL_IP_ADDR_LEN);
  mt_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

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
  uint32_t ssrc = ops->ssrc ? ops->ssrc : (uint32_t)s->idx + 0x123450;
  rtp->base.ssrc = htonl(ssrc);
  rtp->row_length = htons(s->st20_pkt_len);
  rtp->row_number = 0;
  rtp->row_offset = 0;

  /* st22_rfc9134_rtp_hdr if st22 frame mode */
  if (s->st22_info) {
    struct st22_rfc9134_rtp_hdr* st22_hdr = &s->st22_info->rtp_hdr[s_port];
    /* copy base */
    mt_memcpy(&st22_hdr->base, &rtp->base, sizeof(st22_hdr->base));
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

/* the retransmit ring mt_rtcp_tx_create() keeps: never smaller than the TX queue */
static uint16_t tv_rtcp_buffer_size(struct mtl_main_impl* impl,
                                    struct st_tx_video_session_impl* s,
                                    enum mtl_port port) {
  uint16_t size =
      s->ops.rtcp.buffer_size ? s->ops.rtcp.buffer_size : ST_TX_VIDEO_RTCP_RING_SIZE;
  return RTE_MAX(size, mt_if_nb_tx_desc(impl, port));
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
    mt_memcpy(&hdr, &s->s_hdr[i], sizeof(hdr));
    hdr.udp.dst_port++;
    rtcp_ops.udp_hdr = &hdr;
    if (!ops->rtcp.buffer_size) ops->rtcp.buffer_size = ST_TX_VIDEO_RTCP_RING_SIZE;
    rtcp_ops.buffer_size = ops->rtcp.buffer_size;
    /* the ring mt_rtcp_tx_create() keeps, which it would raise with a warning */
    if (mt_if_tx_deep(impl, port))
      rtcp_ops.buffer_size = tv_rtcp_buffer_size(impl, s, port);
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
  mt_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));
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
  mt_memcpy(pd_r, pd_base, pd_len);

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
  mt_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_P], sizeof(*hdr));

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
    if (tv_pkt_has_extra_hdr(s, offset))
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

  if (!single_line && s->st20_linesize > (size_t)s->st20_bytes_in_line)
    /* update offset with line padding for copying */
    offset = offset % s->st20_bytes_in_line + line1_number * s->st20_linesize;
  /* copy payload */
  void* payload = NULL;
  if (e_rtp)
    payload = &e_rtp[1];
  else
    payload = (void*)((uint8_t*)rtp + sizeof(*rtp));
  if (e_rtp && s->st20_linesize > (size_t)s->st20_bytes_in_line) {
    /* cross lines with padding case */
    mt_memcpy(payload, frame_info->addr + offset, line1_length);
    mt_memcpy(payload + line1_length,
              frame_info->addr + s->st20_linesize * (line1_number + 1), line2_length);
  } else {
    mt_memcpy(payload, frame_info->addr + offset, left_len);
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
  mt_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_P], sizeof(*hdr));

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
    if (tv_pkt_has_extra_hdr(s, offset))
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

  if (!single_line && s->st20_linesize > (size_t)s->st20_bytes_in_line)
    /* update offset with line padding for copying */
    offset = offset % s->st20_bytes_in_line + line1_number * s->st20_linesize;

  if (e_rtp && s->st20_linesize > (size_t)s->st20_bytes_in_line) {
    /* cross lines with padding case */
    /* re-allocate from copy chain mempool */
    rte_pktmbuf_free(pkt_chain);
    pkt_chain = rte_pktmbuf_alloc(s->mbuf_mempool_copy_chain);
    if (!pkt_chain) {
      dbg("%s(%d), pkts chain realloc fail %d\n", __func__, s->idx, s->st20_pkt_idx);
      s->port_user_stats.stat_pkts_chain_realloc_fail++;
      return -ENOMEM;
    }
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    mt_memcpy(payload, frame_info->addr + offset, line1_length);
    mt_memcpy(payload + line1_length,
              frame_info->addr + s->st20_linesize * (line1_number + 1), line2_length);
  } else if (tv_frame_payload_cross_page(s, frame_info, offset, left_len)) {
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    mt_memcpy(payload, frame_info->addr + offset, left_len);
  } else if (s->tx_direct_chain && s->st20_pkt_idx < s->st20_total_pkts - 1) {
    /* only the last pkt holds the frame: the PMD frees it after the rest completed */
    pkt_chain->buf_addr = frame_info->addr + offset;
    rte_mbuf_iova_set(pkt_chain, tv_frame_get_offset_iova(s, frame_info, offset));
    pkt_chain->buf_len = left_len;
    pkt_chain->data_off = 0;
  } else {
    /* attach payload to chainbuf */
    rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                              tv_frame_get_offset_iova(s, frame_info, offset), left_len,
                              frame_info->sh_cur);
    rte_mbuf_ext_refcnt_update(frame_info->sh_cur, 1);
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
  mt_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_rfc4175_video_hdr*);
  rtp_base = &hdr_base->rtp;
  mt_memcpy(rtp, rtp_base, sizeof(*rtp));

  /* copy extra if Continuation */
  uint16_t line1_offset = ntohs(rtp->row_offset);
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    mt_memcpy(&rtp[1], &rtp_base[1], sizeof(struct st20_rfc4175_extra_rtp_hdr));
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

/* at a builder visit inside a frame, the lead of the next pkt it builds; compared as a
 * long double, as a cursor saturated to UINT64_MAX overflows int64_t */
static inline void tv_lead_visit(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s) {
  uint64_t cursor = (uint64_t)s->pacing.tsc_time_cursor, now = mt_get_tsc(impl);
  /* a saturated cursor, UINT64_MAX, is no lead below 0 */
  int64_t lead_ns = cursor > now ? (int64_t)RTE_MIN(cursor - now, (uint64_t)INT64_MAX)
                                 : -(int64_t)(now - cursor);

  if (lead_ns < s->lead_min_ns) s->lead_min_ns = lead_ns;
}

static uint64_t tv_pacing_required_tai(struct st_tx_video_session_impl* s,
                                       enum st10_timestamp_fmt tfmt, uint64_t timestamp);

static uint64_t tv_rtp_required_tai(struct st_tx_video_session_impl* s,
                                    struct rte_mbuf* app_pkt) {
  if (!(s->ops.flags & ST20_TX_FLAG_USER_PACING)) return 0;

  uint64_t tai = *RTE_MBUF_DYNFIELD(app_pkt, s->user_tai_offset, uint64_t*);
  if (!tai) {
    /* builder tasklet: no warn here, the stat dump reports the count */
    s->port_user_stats.common.stat_error_user_timestamp++;
    dbg("%s(%d), USER_PACING frame has no TAI, use default pacing\n", __func__, s->idx);
    return 0;
  }
  return tv_pacing_required_tai(s, ST10_TIMESTAMP_FMT_TAI, tai);
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
  mt_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  mt_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  mt_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  if (rtp->tmstamp != s->st20_rtp_time) {
    /* start of a new frame */
    s->st20_pkt_idx = 0;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st20_rtp_time = rtp->tmstamp;
    bool second_field = false;
    if (s->ops.interlaced) {
      struct st20_rfc4175_rtp_hdr* rfc4175 = rte_pktmbuf_mtod_offset(
          pkt, struct st20_rfc4175_rtp_hdr*, sizeof(struct mt_udp_hdr));
      uint16_t line1_number = ntohs(rfc4175->row_number);
      second_field = !!(line1_number & ST20_SECOND_FIELD);
      if (second_field) {
        s->port_user_stats.stat_interlace_second_field++;
      } else {
        s->port_user_stats.stat_interlace_first_field++;
      }
    }
    tv_sync_pacing(impl, s, tv_rtp_required_tai(s, pkt), second_field);
    if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    } else {
      uint64_t tai_for_rtp_ts;
      if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
        tai_for_rtp_ts = tai_from_frame_count(s, s->pacing.cur_epochs);
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
  mt_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  mt_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  mt_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(hdr->udp));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_P);
#endif

  if (s->multi_src_port) udp->src_port += (s->st20_pkt_idx / 128) % 8;

  if (rtp->tmstamp != s->st20_rtp_time) {
    /* start of a new frame */
    s->st20_pkt_idx = 0;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (s->ops.num_port > 1) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    s->st20_rtp_time = rtp->tmstamp;
    bool second_field = false;
    if (s->ops.interlaced) {
      struct st20_rfc4175_rtp_hdr* rfc4175 =
          rte_pktmbuf_mtod(pkt_chain, struct st20_rfc4175_rtp_hdr*);
      uint16_t line1_number = ntohs(rfc4175->row_number);
      second_field = !!(line1_number & ST20_SECOND_FIELD);
      if (second_field) {
        s->port_user_stats.stat_interlace_second_field++;
      } else {
        s->port_user_stats.stat_interlace_first_field++;
      }
    }
    tv_sync_pacing(impl, s, tv_rtp_required_tai(s, pkt_chain), second_field);
    if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    } else {
      uint64_t tai_for_rtp_ts;
      if (s->ops.flags & ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH) {
        tai_for_rtp_ts = tai_from_frame_count(s, s->pacing.cur_epochs);
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
  mt_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_R].eth, sizeof(hdr->eth));
  mt_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_R].ipv4, sizeof(hdr->ipv4));
  mt_memcpy(&hdr->udp, &s->s_hdr[MTL_SESSION_PORT_R].udp, sizeof(hdr->udp));

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
  mt_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  mt_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(*ipv4));
  mt_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(*udp));
  /* copy rtp */
  mt_memcpy(rtp, &st22_info->rtp_hdr[MTL_SESSION_PORT_P], sizeof(*rtp));

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
  mt_memcpy(payload, frame_info->addr + offset, left_len);

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
  mt_memcpy(&hdr->eth, &s->s_hdr[MTL_SESSION_PORT_P].eth, sizeof(hdr->eth));
  mt_memcpy(ipv4, &s->s_hdr[MTL_SESSION_PORT_P].ipv4, sizeof(*ipv4));
  mt_memcpy(udp, &s->s_hdr[MTL_SESSION_PORT_P].udp, sizeof(*udp));
  /* copy rtp */
  mt_memcpy(rtp, &st22_info->rtp_hdr[MTL_SESSION_PORT_P], sizeof(*rtp));

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
    mt_memcpy(payload, frame_info->addr + offset, left_len);
  } else { /* attach payload */
    rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                              tv_frame_get_offset_iova(s, frame_info, offset), left_len,
                              frame_info->sh_cur);
    rte_mbuf_ext_refcnt_update(frame_info->sh_cur, 1);
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
  mt_memcpy(hdr, &s->s_hdr[MTL_SESSION_PORT_R], sizeof(*hdr));

#ifdef MTL_SIMULATE_PACKET_DROPS
  tv_simulate_packet_loss(s, ipv4, MTL_SESSION_PORT_R);
#endif

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st22_rfc9134_video_hdr*);
  rtp_base = &hdr_base->rtp;
  mt_memcpy(rtp, rtp_base, sizeof(*rtp));

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
      s->port_user_stats.common.stat_error_user_timestamp++;
      err("%s(%d), EXACT_USER_PACING requires non-zero timestamp\n", __func__, s->idx);
    }
    return 0;
  }

  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    s->port_user_stats.common.stat_error_user_timestamp++;
    err("%s(%d), Media clock can't be used for user-controlled pacing\n", __func__,
        s->idx);
    return 0;  // Return 0 to indicate an invalid timestamp and fallback to default pacing
  } else {
    required_tai = timestamp;
  }

  if (s->ops.flags & ST20_TX_FLAG_EXACT_USER_PACING) {
    uint64_t cur_tai = mt_get_ptp_time(s->impl, MTL_PORT_P);
    long double warm_up_lead_ns = s->pacing.warm_pkts * s->pacing.trs;
    if (required_tai < cur_tai || required_tai - cur_tai > NS_PER_S ||
        required_tai - cur_tai < warm_up_lead_ns) {
      s->port_user_stats.common.stat_error_user_timestamp++;
      return 0;
    }
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
    tv_reset_pacing_epoch(impl, s);
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
  if ((size_t)n != s->st20_frame_size) {
    warn("%s(%d), write fail %" PRIu64 "\n", __func__, idx, n);
  } else {
    MT_USDT_ST20_TX_FRAME_DUMP(mgr->idx, s->idx, usdt_dump_path, frame->addr, n);
  }

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

/* a frame time from build to send, one more for the next to push this one out */
static uint64_t tv_reclaim_hang_ns(struct st_tx_video_session_impl* s) {
  return RTE_MAX(s->tx_hang_detect_time_thresh, (uint64_t)(2 * s->pacing.frame_time));
}

/* Reclaims the sent pkts of every port, unless the last reclaim is less than
 * ST_TX_VIDEO_RECLAIM_NS ago. Returns false when it did not. */
static bool tv_reclaim_poll(struct st_tx_video_session_impl* s, uint64_t now) {
  struct st_tx_video_reclaim* reclaim = &s->reclaim;

  if (now - reclaim->last_tsc < ST_TX_VIDEO_RECLAIM_NS) return false;
  /* a gap past half of hang is the scheduler stalling, not the queue: restart the wait */
  if (now - reclaim->last_tsc > tv_reclaim_hang_ns(s) / 2) reclaim->wait_tsc = now;
  reclaim->last_tsc = now;
  for (int p = 0; p < s->ops.num_port; p++) mt_txq_reclaim(s->queue[p]);
  return true;
}

/* the warm-up start of the frame built last: the frames before it leave behind it. The
 * transmitters send a target past 1 s at once. */
static uint64_t tv_next_frame_open_tsc(struct st_tx_video_session_impl* s, uint64_t now) {
  uint64_t target = s->pacing.tsc_time_frame_start;
  uint64_t warm_ns = (uint64_t)(s->pacing.warm_pkts * s->pacing.trs);

  if (now < target && target - now > NS_PER_S) return 0;
  return target > warm_ns ? target - warm_ns : 0;
}

/* idle legs of a wait past the hang threshold: their transmitter has no burst to fail */
static void tv_frames_out_hang(struct st_tx_video_session_impl* s, uint64_t now) {
  uint64_t hang = tv_reclaim_hang_ns(s);
  uint64_t start = RTE_MAX(s->reclaim.wait_tsc, tv_next_frame_open_tsc(s, now));

  if (now < start || now - start <= hang) return;
  /* the builder may write these transmitter fields: both run on one scheduler thread */
  for (int p = 0; p < s->ops.num_port; p++) {
    if (!s->tx_link_drain &&
        !mt_dev_link_down(s->impl, mt_port_logic2phy(s->port_maps, p)))
      continue;
    /* A recovery of any leg flushes the ring of a link down leg, which fails. The frames
     * held over a link resync wait for the bound of tv_link_drain(). */
    for (int i = 0; i < s->ops.num_port; i++) s->last_burst_succ_time_tsc[i] = now;
    return;
  }
  for (int p = 0; p < s->ops.num_port; p++) {
    if (now - s->last_burst_succ_time_tsc[p] <= hang) continue;
    err("%s(%d,%d), frames out for %" PRIu64 " ms\n", __func__, s->idx, p,
        (now - start) / (uint64_t)NS_PER_MS);
    s->tx_queue_recovery_pending[p] = true;
    s->last_burst_succ_time_tsc[p] = now;
  }
}

static bool tv_link_held(struct st_tx_video_session_impl* s, bool stale) {
  /* Sweep all stale pools before returning so every drained leg marks tx_link_live. */
  bool held = false;
  for (int p = 0; stale && p < s->ops.num_port; p++) {
    if (!(s->tx_link_stale & (1 << p)) || !s->tx_link_stale_pool[p]) continue;
    if (rte_mempool_in_use_count(s->tx_link_stale_pool[p])) {
      held = true;
    } else {
      s->tx_link_stale_pool[p] = NULL;
      s->tx_link_live |= 1 << p;
    }
  }
  if (held) return true;
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (frame->link_held && rte_atomic32_read(&frame->refcnt)) return true;
    for (int j = 0; stale && s->tx_link_stale && j < ST_FT_SH_INFO_MAX; j++) {
      if (!(frame->link_stale_sh & (1u << j))) continue;
      if (rte_mbuf_ext_refcnt_read(&frame->sh_info[j])) return true;
      frame->link_stale_sh &= ~(1u << j);
    }
  }
  return false;
}

/* Finds a sh_info slot of frame that no mbuf counts on, other than sh_cur; NULL if none.
 */
static struct rte_mbuf_ext_shared_info* tv_frame_find_free_sh_info(
    struct st_frame_trans* frame) {
  for (int i = 0; i < ST_FT_SH_INFO_MAX; i++) {
    struct rte_mbuf_ext_shared_info* sh = &frame->sh_info[i];
    if (sh != frame->sh_cur && !rte_mbuf_ext_refcnt_read(sh)) return sh;
  }
  return NULL;
}

/* chain mbufs that a ring keeps count on the retired sh_cur: their late free does not
 * count on the next use of the frame */
static void tv_frame_sh_retire(struct st_frame_trans* frame,
                               struct rte_mbuf_ext_shared_info* next) {
  frame->sh_cur->free_cb = tx_video_frame_retired_release_callback;
  next->free_cb = tx_video_frame_release_callback;
  frame->link_stale_sh &= ~(1u << (next - frame->sh_info));
  frame->sh_cur = next;
}

/* a held frame that a ring counts on, with no sh_info to retire to: no release */
static bool tv_link_held_no_sh_info(struct st_tx_video_session_impl* s) {
  for (uint16_t i = 0; !s->tx_no_chain && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (!frame->sh_cur) continue;
    int cur = (int)(frame->sh_cur - frame->sh_info);

    if (frame->link_held && rte_atomic32_read(&frame->refcnt) &&
        rte_mbuf_ext_refcnt_read(frame->sh_cur) >
            __builtin_popcount(frame->sh_pins[cur]) &&
        !tv_frame_find_free_sh_info(frame))
      return true;
  }
  return false;
}

/* drops the link pins of mask on sh_info slot, with no free callback */
static void tv_frame_drop_slot_pins(struct st_frame_trans* frame, int slot,
                                    uint8_t mask) {
  uint8_t drop = frame->sh_pins[slot] & mask;
  if (!drop) return;
  frame->sh_pins[slot] &= ~drop;
  rte_mbuf_ext_refcnt_update(&frame->sh_info[slot], (int16_t)(-__builtin_popcount(drop)));
}

static void tv_link_reset_stale(struct st_tx_video_session_impl* s, uint8_t drop_pins) {
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    frame->link_stale_sh = 0;
    for (int j = 0; s->tx_direct_chain && drop_pins && j < ST_FT_SH_INFO_MAX; j++)
      if (&frame->sh_info[j] != frame->sh_cur)
        tv_frame_drop_slot_pins(frame, j, drop_pins);
  }
  memset(s->tx_link_stale_pool, 0, sizeof(s->tx_link_stale_pool));
  s->tx_link_stale = 0;
  s->tx_link_live = 0;
  s->tx_link_short = 0;
  s->tx_link_release_tsc = 0;
}

/* keep_pins != 0: release library frames while down/stale legs keep their pins on a
 * retired sh_info; keep_pins == 0: release all held frames after all links drain. */
static void tv_link_release_held_frames(struct st_tx_video_session_impl* s,
                                        uint8_t keep_pins) {
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    struct rte_mbuf_ext_shared_info* next = NULL;

    if (!frame->link_held || !rte_atomic32_read(&frame->refcnt)) continue;
    if (keep_pins && !(frame->flags & ST_FT_FLAG_RTE_MALLOC)) continue;
    int cur = frame->sh_cur ? (int)(frame->sh_cur - frame->sh_info) : 0;
    uint8_t drop = frame->sh_pins[cur] & ~keep_pins;
    if (!s->tx_no_chain &&
        rte_mbuf_ext_refcnt_read(frame->sh_cur) > __builtin_popcount(drop)) {
      next = tv_frame_find_free_sh_info(frame);
      if (!next) continue; /* each sh_info in use: the frame waits for its ring */
    }
    tv_frame_drop_slot_pins(frame, cur, drop);
    if (next) {
      if (keep_pins) frame->link_stale_sh |= 1u << cur;
      tv_frame_sh_retire(frame, next);
    }
    frame->link_held = false;
    tv_frame_put(frame, __func__);
  }
}

static void tv_link_release_held(struct st_tx_video_session_impl* s) {
  tv_link_release_held_frames(s, 0);
}

/* DPDK 26.07 ci_tx_done_cleanup() double-frees mbufs already freed by
 * ci_tx_xmit_cleanup() on iavf, ice and i40e: never call it in the link drain. */
static bool tv_link_done_cleanup_unsafe(struct mtl_main_impl* impl, enum mtl_port port) {
#if RTE_VERSION >= RTE_VERSION_NUM(26, 7, 0, 0)
  enum mt_driver_type drv = mt_if(impl, port)->drv_info.drv_type;
  return drv == MT_DRV_IAVF || drv == MT_DRV_ICE || drv == MT_DRV_I40E;
#else
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  return false;
#endif
}

/* the pkts the NIC of leg p freed; a deep port and a path with no count tell 1 */
static int tv_link_free_done(struct mtl_main_impl* impl,
                             struct st_tx_video_session_impl* s, int p) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, p);

  if (!mt_if_tx_deep(impl, port) && !tv_link_done_cleanup_unsafe(impl, port)) {
    int freed = mt_txq_done_cleanup(s->queue[p]);
    if (freed >= 0) return freed;
  }
  mt_txq_reclaim(s->queue[p]);
  return 1;
}

/* no done comes under rs descriptors, two a chain pkt; a shared mono pool is not short */
static bool tv_link_ring_short(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s, int p) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, p);
  uint16_t rs = mt_if(impl, port)->dev_info.default_txconf.tx_rs_thresh;
  unsigned int segs = s->tx_no_chain ? 1 : 2;

  if (!rs) rs = 32; /* fallback when the PMD leaves default_txconf.tx_rs_thresh unset */
  return s->mbuf_mempool_hdr[p] &&
         rte_mempool_in_use_count(s->mbuf_mempool_hdr[p]) * segs < rs;
}

/* Frees the done pkts of each leg while frames are held over a link resync: no later
 * burst may free them. The drain ends with no frame held and each link up. A leg that
 * lost its link with an rs batch in its ring and freed nothing since is dead, as an E610
 * ring: recover it a hang threshold after the up. Else hand back the frames. */
static void tv_link_drain(struct mtl_main_impl* impl,
                          struct st_tx_video_session_impl* s) {
  uint64_t now = mt_get_tsc(impl);
  bool held;
  uint8_t down_mask = 0, up_mask = 0, dead, short_legs;

  if (now < s->tx_link_drain_tsc || now - s->tx_link_drain_tsc < ST_TX_VIDEO_RECLAIM_NS)
    return;
  s->tx_link_drain_tsc = now;
  for (int p = 0; p < s->ops.num_port; p++) {
    if (mt_dev_link_down(impl, mt_port_logic2phy(s->port_maps, p)))
      down_mask |= 1 << p;
    else
      up_mask |= 1 << p;
  }
  if (down_mask && up_mask && s->tx_link_release_tsc && now >= s->tx_link_release_tsc) {
    s->tx_link_release_tsc = 0;
    tv_link_release_held_frames(s, down_mask | s->tx_link_stale);
  }
  held = tv_link_held(s, !down_mask);
  for (int p = 0; held && p < s->ops.num_port; p++) {
    if ((up_mask & (1 << p)) && s->queue[p] && tv_link_free_done(impl, s, p) > 0)
      s->tx_link_live |= 1 << p;
  }
  if (down_mask) {
    s->tx_link_up_tsc = now;
    return;
  }
  if (held) held = tv_link_held(s, true);
  if (!held) {
    tv_link_reset_stale(s, s->tx_link_stale);
    s->tx_link_drain = false;
    s->pacing.epoch_resync = true;
    return;
  }
  if (now < s->tx_link_up_tsc || now - s->tx_link_up_tsc <= tv_reclaim_hang_ns(s)) return;

  short_legs = tv_link_held_no_sh_info(s) ? 0 : s->tx_link_short;
  dead = s->tx_link_stale & ~s->tx_link_live & ~short_legs;
  for (int p = 0; p < s->ops.num_port; p++) {
    if (!(dead & (1 << p))) continue;
    err("%s(%d,%d), the NIC freed nothing since the link loss, dead queue\n", __func__,
        s->idx, p);
    s->tx_queue_recovery_pending[p] = true;
  }
  tv_link_reset_stale(s, s->tx_link_stale & ~dead);
  if (!dead) tv_link_release_held(s);
  s->tx_link_up_tsc = now;
}

/* true while a frame but the last one built is referenced: the app gets free ones only */
static bool tv_frames_out(struct mtl_main_impl* impl,
                          struct st_tx_video_session_impl* s) {
  if (!s->reclaim.enabled) return false;
  for (uint16_t i = 0; i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    /* a frame held over a link resync waits for the NIC ring, not for a reclaim */
    if (i == s->st20_frame_idx || !rte_atomic32_read(&frame->refcnt) || frame->link_held)
      continue;
    uint64_t now = mt_get_tsc(impl);
    if (!s->reclaim.wait_tsc) s->reclaim.wait_tsc = now;
    if (!tv_reclaim_poll(s, now)) return true;
    if (!rte_atomic32_read(&frame->refcnt)) continue;
    tv_frames_out_hang(s, now);
    return true;
  }
  s->reclaim.wait_tsc = 0;
  return false;
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

      if (tv_frames_out(impl, s)) {
        s->stat_build_ret_code = -STI_FRAME_RECLAIM_WAIT;
        return MTL_TASKLET_ALL_DONE;
      }
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
          s->port_user_stats.stat_user_busy++;
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
        s->port_user_stats.stat_user_meta_cnt++;
        /* copy user meta to frame meta */
        mt_memcpy(frame->user_meta, meta.user_meta, meta.user_meta_size);
        frame->user_meta_data_size = meta.user_meta_size;
      }

      s->stat_user_busy_first = true;
      /* all check fine */
      rte_atomic32_inc(&frame->refcnt);
      frame->link_held = false;
      tv_frame_build_hold(s, frame);
      s->st20_frame_idx = next_frame_idx;
      s->st20_frame_lines_ready = 0;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      /* user timestamp control if any */
      uint64_t required_tai = tv_pacing_required_tai(s, meta.tfmt, meta.timestamp);
      if (s->ops.interlaced) {
        if (frame->tv_meta.second_field) {
          s->port_user_stats.stat_interlace_second_field++;
        } else {
          s->port_user_stats.stat_interlace_first_field++;
        }
        /* s->second_field is used to init the next frame */
        s->second_field = !frame->tv_meta.second_field;
      }
      tv_sync_pacing(impl, s, required_tai, frame->tv_meta.second_field);
      frame->tv_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
      /* Report the same TAI instant frame->rtp_timestamp (set next) was
       * derived from, not tv_sync_pacing()'s scheduled ptp_time_cursor --
       * they can legitimately differ (USER_TIMESTAMP, RTP_TIMESTAMP_EPOCH,
       * rtp_timestamp_delta_us). See tv_update_rtp_time_stamp() above. */
      frame->tv_meta.timestamp = tv_update_rtp_time_stamp(s, meta.tfmt, meta.timestamp);
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
        s->port_user_stats.stat_lines_not_ready++;
        s->st20_frame_lines_waited = true;
        s->stat_build_ret_code = -STI_FRAME_APP_SLICE_NOT_READY;
        return MTL_TASKLET_ALL_DONE;
      }
    }
  }

  /* pkts of a frame that waited for its lines are due before they are built */
  if (s->st20_pkt_idx && !s->st20_frame_lines_waited) tv_lead_visit(impl, s);
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

  /* RL transmitters read only the stamps of a frame's first pkt */
  bool stamp_all = s->pacing_way[MTL_SESSION_PORT_P] != ST21_TX_PACING_WAY_RL ||
                   (send_r && s->pacing_way[MTL_SESSION_PORT_R] != ST21_TX_PACING_WAY_RL);
  long double tsc_cursor = pacing->tsc_time_cursor;
  long double ptp_cursor = pacing->ptp_time_cursor;

  for (unsigned int i = 0; i < bulk; i++) {
    bool stamp = stamp_all || !s->st20_pkt_idx;

    st_tx_mbuf_set_priv(pkts[i], &s->st20_frames[s->st20_frame_idx]);
    if (s->st20_pkt_idx >= s->st20_total_pkts) {
      s->port_user_stats.stat_pkts_dummy++;
      if (!s->tx_no_chain) rte_pktmbuf_free(pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
    } else {
      if (s->tx_no_chain)
        tv_build_st20(s, pkts[i]);
      else
        tv_build_st20_chain(s, pkts[i], pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
      s->port_user_stats.common.port[MTL_SESSION_PORT_P].build++;
    }
    if (stamp) {
      st_tx_mbuf_set_tsc(pkts[i], (uint64_t)tsc_cursor);
      st_tx_mbuf_set_ptp(pkts[i], (uint64_t)ptp_cursor);
      st_tx_mbuf_set_vrx_room(pkts[i], pacing->vrx_room_ns);
    }

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
        s->port_user_stats.common.port[MTL_SESSION_PORT_R].build++;
      }
      if (stamp) {
        st_tx_mbuf_set_tsc(pkts_r[i], (uint64_t)tsc_cursor);
        st_tx_mbuf_set_ptp(pkts_r[i], (uint64_t)ptp_cursor);
        st_tx_mbuf_set_vrx_room(pkts_r[i], pacing->vrx_room_ns);
      }
    }

    tsc_cursor += pacing->trs;
    ptp_cursor += pacing->trs;
    s->st20_pkt_idx++;
  }
  pacing->tsc_time_cursor = tsc_cursor;
  pacing->ptp_time_cursor = ptp_cursor;

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
    s->st20_frame_lines_waited = false;
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    if (send_r) s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    tv_frame_build_put(s, &s->st20_frames[s->st20_frame_idx]);

    uint64_t frame_end_time = mt_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      s->port_user_stats.common.stat_exceed_frame_time++;
      rte_atomic32_inc(&s->cbs_build_timeout);
      dbg("%s(%d), frame %d build time out %Lfus\n", __func__, idx, s->st20_frame_idx,
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
        uint16_t data_len = rte_pktmbuf_data_len(mbuf[i]);
        if (data_len < sizeof(struct mt_udp_hdr)) continue; /* runt, no rtcp payload */
        struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(mbuf[i], struct mt_udp_hdr*);
        /* the rtcp offset below is correct only for an ipv4 header with no options */
        if ((hdr->ipv4.version_ihl & RTE_IPV4_HDR_IHL_MASK) != RTE_IPV4_MIN_IHL) continue;
        uint16_t dgram_len = ntohs(hdr->udp.dgram_len);
        if (dgram_len < sizeof(struct rte_udp_hdr)) continue;
        /* a frame can hold ethernet padding after the udp datagram. Give the parser
         * only the bytes that the udp header declares and that the mbuf holds. */
        size_t rtcp_len = RTE_MIN((size_t)(data_len - sizeof(struct mt_udp_hdr)),
                                  (size_t)(dgram_len - sizeof(struct rte_udp_hdr)));
        struct mt_rtcp_hdr* rtcp = rte_pktmbuf_mtod_offset(mbuf[i], struct mt_rtcp_hdr*,
                                                           sizeof(struct mt_udp_hdr));
        mt_rtcp_tx_parse_rtcp_packet(s->rtcp_tx[s_port], rtcp, rtcp_len);
        if (s->rtcp_tx[s_port]->nack_drop_sampled) { /* the parser logged a sample */
          uint8_t* ip = (uint8_t*)&hdr->ipv4.src_addr;
          MTL_MAY_UNUSED(ip);
          dbg("%s(%d), invalid rtcp from %u.%u.%u.%u:%u, data_len %u dgram_len %u\n",
              __func__, s->idx, ip[0], ip[1], ip[2], ip[3], ntohs(hdr->udp.src_port),
              data_len, dgram_len);
        }
      }
      rte_pktmbuf_free_bulk(&mbuf[0], rv);
    }
  }

  return 0;
}

static inline uint32_t tv_rtp_pkt_tmstamp(const struct st_tx_video_session_impl* s,
                                          const struct rte_mbuf* pkt) {
  size_t off = s->tx_no_chain ? sizeof(struct mt_udp_hdr) : 0;
  return rte_pktmbuf_mtod_offset(pkt, const struct st_rfc3550_rtp_hdr*, off)->tmstamp;
}

static unsigned int tv_rtp_dequeue(struct st_tx_video_session_impl* s,
                                   struct rte_mbuf** pkts_rtp, unsigned int pkts_bulk) {
  if (unlikely(s->rtp_drop_frame)) {
    struct rte_mbuf* pkt;
    while (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt) == 0) {
      s->ops.notify_rtp_done(s->ops.priv);
      if (tv_rtp_pkt_tmstamp(s, pkt) != s->st20_rtp_time) {
        s->rtp_drop_frame = false;
        s->rtp_pending_pkt = pkt;
        break;
      }
      rte_pktmbuf_free(pkt);
    }
    if (s->rtp_drop_frame) return 0;
  }
  unsigned int have = s->rtp_pending_pkt ? 1 : 0;
  if (have) pkts_rtp[0] = s->rtp_pending_pkt;
  if (pkts_bulk > have &&
      !mt_rte_ring_sc_dequeue_bulk(s->packet_ring, (void**)&pkts_rtp[have],
                                   pkts_bulk - have, NULL))
    return 0;
  s->rtp_pending_pkt = NULL;
  return pkts_bulk;
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
      s->inflight[MTL_SESSION_PORT_R][0] = NULL;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_rtp[bulk];
  int pkts_remaining = s->st20_total_pkts - s->st20_pkt_idx;
  bool eof = (pkts_remaining > 0) && ((unsigned int)pkts_remaining < bulk) ? true : false;
  unsigned int pkts_bulk = eof ? 1 : bulk; /* bulk one only at end of frame */

  if (eof)
    dbg("%s(%d), pkts_bulk %d pkt idx %d\n", __func__, idx, pkts_bulk, s->st20_pkt_idx);

  n = tv_rtp_dequeue(s, pkts_rtp, pkts_bulk);
  if (n == 0) {
    if (s->stat_user_busy_first) {
      s->port_user_stats.stat_user_busy++;
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
  ret =
      alloc_bulk ? rte_pktmbuf_alloc_bulk(hdr_pool_p, &pkts[alloc_begin], alloc_bulk) : 0;
  if (ret < 0) {
    dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_rtp, pkts_bulk);
    s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }
  if (send_r && alloc_bulk) {
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
      s->port_user_stats.stat_pkts_dummy++;
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
  if ((size_t)n != size) {
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
          s->port_user_stats.stat_user_busy++;
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
      frame->link_held = false;
      tv_frame_build_hold(s, frame);
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
          s->port_user_stats.stat_interlace_second_field++;
        } else {
          s->port_user_stats.stat_interlace_first_field++;
        }
        /* s->second_field is used to init the next frame */
        s->second_field = !frame->tx_st22_meta.second_field;
      }
      tv_sync_pacing_st22(impl, s, required_tai, st22_info->st22_total_pkts,
                          frame->tx_st22_meta.second_field);
      frame->tx_st22_meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
      /* Same reasoning as tv_tasklet_frame()'s frame->tv_meta.timestamp above:
       * report the instant frame->rtp_timestamp actually came from. */
      frame->tx_st22_meta.timestamp =
          tv_update_rtp_time_stamp(s, meta.tfmt, meta.timestamp);
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
      s->port_user_stats.stat_pkts_dummy++;
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
        s->port_user_stats.stat_pkts_dummy++;
        if (!s->tx_no_chain) rte_pktmbuf_free(pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
      } else {
        if (s->tx_no_chain)
          tv_build_st22(s, pkts[i]);
        else
          tv_build_st22_chain(s, pkts[i], pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
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
    st22_info->frame_idx++;
    tv_frame_build_put(s, &s->st20_frames[s->st20_frame_idx]);

    uint64_t frame_end_time = mt_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      s->port_user_stats.common.stat_exceed_frame_time++;
      rte_atomic32_inc(&s->cbs_build_timeout);
      dbg("%s(%d), frame %d build time out %Lfus\n", __func__, idx, s->st20_frame_idx,
          (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
    }
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

/* Frees the pkts the session holds in software. The next frame takes a new epoch, after
 * the time it is built: see tv_prev_epoch(). */
static void tv_drop_sw_pkts(struct mtl_main_impl* impl,
                            struct st_tx_video_session_impl* s) {
  tv_frame_build_drop(s);
  if (s->rtp_pending_pkt) {
    s->st20_rtp_time = tv_rtp_pkt_tmstamp(s, s->rtp_pending_pkt);
    rte_pktmbuf_free(s->rtp_pending_pkt);
    s->rtp_pending_pkt = NULL;
  }
  if (s->packet_ring) {
    struct rte_mbuf* pkt;
    bool drained = false;
    while (rte_ring_sc_dequeue(s->packet_ring, (void**)&pkt) == 0) {
      s->st20_rtp_time = tv_rtp_pkt_tmstamp(s, pkt);
      rte_pktmbuf_free(pkt);
      drained = true;
    }
    if (drained && s->ops.notify_rtp_done) s->ops.notify_rtp_done(s->ops.priv);
  }
  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    if (s->ring[i]) mt_ring_dequeue_clean(s->ring[i]);
  }
  /* payload pools are shared by redundant ports: every port gives its mbufs back */
  st_tx_video_transmitter_state_cleanup(s);
  /* the freed pkts had seqs: a ring with a seq gap refuses each later pkt */
  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    if (s->rtcp_tx[i]) mt_rtcp_tx_buffer_reset(s->rtcp_tx[i]);
  }
  uint64_t now = mt_get_tsc(impl);
  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    s->last_burst_succ_time_tsc[i] = now;
    s->trs_frame_started[i] = NULL;
  }
  s->st20_pkt_idx = 0;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  if (!st20_is_frame_type(s->ops.type) && !s->st22_info && s->st20_rtp_time != UINT32_MAX)
    s->rtp_drop_frame = true;
  s->pacing.epoch_resync = true;
}

/* true for a frame that the dead queue holds on sh_cur and earlier dead queues on each
 * other sh_info: the recovery has no free sh_info to retire sh_cur to */
static bool tv_frames_no_sh_info(struct st_tx_video_session_impl* s, uint8_t down_mask,
                                 uint8_t keep_pins, bool keep_all) {
  for (uint16_t i = 0; !s->tx_no_chain && s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (!frame->sh_cur || !rte_atomic32_read(&frame->refcnt) ||
        tv_frame_find_free_sh_info(frame))
      continue;
    if (down_mask && !(frame->flags & ST_FT_FLAG_RTE_MALLOC) &&
        (keep_all || frame->link_held))
      continue;
    int cur = (int)(frame->sh_cur - frame->sh_info);
    uint16_t holds =
        s->tx_direct_chain + __builtin_popcount(frame->sh_pins[cur] & ~keep_pins);
    if (rte_mbuf_ext_refcnt_read(frame->sh_cur) > holds) return true;
  }
  return false;
}

/* Hands back each frame still held on a TX queue recovery, with the count of
 * tv_frames_hold(). Chain mbufs that the dead queue keeps count on the retired sh_info,
 * which tv_frames_no_sh_info() found. EXT frames held by a down sibling stay held. */
static void tv_recovery_release_frames(struct st_tx_video_session_impl* s,
                                       enum mtl_session_port s_port, uint8_t down_mask,
                                       uint8_t keep_pins, bool keep_all) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++)
    if (!(down_mask & (1 << i))) s->trs_link_fence[i] = 0;
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];

    for (int j = 0; j < ST_FT_SH_INFO_MAX; j++) {
      frame->sh_pins[j] &= ~(1u << s_port);
      tv_frame_drop_slot_pins(frame, j, ~keep_pins);
    }
    if (!rte_atomic32_read(&frame->refcnt)) continue;
    if (s->tx_direct_chain) rte_mbuf_ext_refcnt_update(frame->sh_cur, -1);
    if (!s->tx_no_chain && rte_mbuf_ext_refcnt_read(frame->sh_cur)) {
      if (down_mask && !(frame->flags & ST_FT_FLAG_RTE_MALLOC) &&
          (keep_all || frame->link_held)) {
        frame->link_held = true;
        continue;
      }
      tv_frame_sh_retire(frame, tv_frame_find_free_sh_info(frame));
    }
    frame->link_held = false;
    tv_frame_put(frame, __func__);
  }
}

/* Hands back each frame no ring holds. A NIC or RTCP ring that still holds chain mbufs
 * of a frame counts on its sh_cur, and their free hands the frame back. */
static void tv_link_release_frames(struct st_tx_video_session_impl* s) {
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];

    if (!rte_atomic32_read(&frame->refcnt)) continue;
    if (!s->tx_no_chain && rte_mbuf_ext_refcnt_read(frame->sh_cur)) continue;
    tv_frame_put(frame, __func__);
  }
}

/* Direct chain: a frame the NIC of a leg started holds no count for its pkts before the
 * last one, so that ring may hold pkts of it after the frame is done. A leg with its link
 * up drops the pin once its ring took nb_tx_desc pkts more, a leg with no link once it
 * did after the up. */
static void tv_link_pin_frames(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s) {
  for (int p = 0; p < s->ops.num_port; p++) {
    if (!s->tx_link[p].down)
      s->trs_link_fence[p] = mt_if_nb_tx_desc(impl, mt_port_logic2phy(s->port_maps, p));
    st_tx_video_frame_link_pin(s->trs_frame_started[p], p);
    s->trs_frame_started[p] = NULL;
  }
}

static uint64_t tv_link_up_drain_tsc(struct mtl_main_impl* impl,
                                     struct st_tx_video_session_impl* s, uint64_t now,
                                     uint8_t skip_mask) {
  uint64_t release_tsc = 0;

  for (int p = 0; p < s->ops.num_port; p++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, p);
    if ((skip_mask & (1u << p)) || s->tx_link[p].down || mt_dev_link_down(impl, port))
      continue;
    uint64_t end = now + (uint64_t)(mt_if_nb_tx_desc(impl, port) * s->pacing.trs);
    if (s->pacing_way[p] == ST21_TX_PACING_WAY_RL)
      end += (uint64_t)s->rl_plan[p].refill_ns;
    else if (s->pacing_way[p] == ST21_TX_PACING_WAY_TSN && s->trs_wire_tsc[p])
      end = RTE_MAX(end, s->trs_wire_tsc[p] + (uint64_t)s->pacing.trs);
    if (end > release_tsc) release_tsc = end;
  }
  return release_tsc;
}

static void tv_link_start_drain(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s, uint64_t now,
                                uint8_t stale_mask, uint64_t release_tsc) {
  if (!s->tx_link_drain) tv_link_reset_stale(s, 0);
  for (int p = 0; p < s->ops.num_port; p++) {
    if (!(stale_mask & (1 << p))) continue;
    s->tx_link_stale |= 1 << p;
    s->tx_link_live &= ~(1 << p);
    s->tx_link_short &= ~(1 << p);
    /* what the NIC did before the loss tells nothing after it */
    if (s->queue[p]) tv_link_free_done(impl, s, p);
    if (tv_link_ring_short(impl, s, p)) s->tx_link_short |= 1 << p;
  }
  s->tx_link_up_tsc = now;
  s->tx_link_release_tsc = release_tsc;
  s->tx_link_drain = true;
}

/* a port lost its link: no leg sends the pkts of the stall after their time */
static void tv_link_resync(struct mtl_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  uint8_t mask = s->tx_link_resync;
  s->tx_link_resync = 0;
  if (!mask) {
    for (int p = 0; p < s->ops.num_port; p++)
      if (s->tx_link[p].down) mask |= 1 << p;
  }
  uint64_t now = mt_get_tsc(impl);
  uint64_t release_tsc = tv_link_up_drain_tsc(impl, s, now, mask);
  /* before the drop: the last pkt of a frame the drop frees ends its count */
  if (s->tx_direct_chain) tv_link_pin_frames(impl, s);
  tv_drop_sw_pkts(impl, s);
  tv_link_release_frames(s);
  for (uint16_t i = 0; s->st20_frames && i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (rte_atomic32_read(&frame->refcnt)) frame->link_held = true;
  }
  tv_link_start_drain(impl, s, now, mask, release_tsc);
  notice("%s(%d), the next frame starts on a new epoch\n", __func__, s->idx);
}

static int tvs_tasklet_handler(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_tx_video_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_dp_get(mgr, sidx);
    if (!s || !s->active) continue;
    if (unlikely(s->tx_link_resync)) tv_link_resync(impl, s);
    if (unlikely(s->tx_link_drain)) tv_link_drain(impl, s);

    if (time_measure) tsc_s = mt_get_tsc(impl);

    if (s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP) tv_tasklet_rtcp(s);
    /* check vsync if it has vsync enabled */
    if (s->ops.flags & ST20_TX_FLAG_ENABLE_VSYNC) tv_poll_vsync(impl, s);

    s->stat_build_ret_code = 0;
    if (s->st22_info)
      pending += tv_tasklet_st22(impl, s);
    else if (st20_is_frame_type(s->ops.type))
      pending += tv_tasklet_frame(impl, s);
    else
      pending += tv_tasklet_rtp(impl, s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
  }
  tx_video_session_dp_done(mgr);

  return pending;
}

/* Retiring an unflushed TX queue protects against wedged dedicated rings, but
 * must not poison a shared TX queue, permanently burn IGC's sole LaunchTime TSN
 * queue 0, or retire a queue whose private header pools are empty while the link
 * is down. */
static bool tv_uinit_should_mark_fatal(struct st_tx_video_session_impl* s, int i) {
  struct mtl_main_impl* impl = s->impl;
  bool link_down = true;
  if (impl) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    if (mt_user_shared_txq(impl, port)) return false;
    if (s->pacing_way[i] == ST21_TX_PACING_WAY_TSN &&
        mt_if(impl, port)->drv_info.drv_type == MT_DRV_IGC)
      return false;
    link_down = mt_dev_link_down(impl, port);
  }
  if (link_down && !s->tx_mono_pool && !s->mbuf_mempool_reuse_rx[i] &&
      s->mbuf_mempool_hdr[i] && !rte_mempool_in_use_count(s->mbuf_mempool_hdr[i]) &&
      !(s->tx_link_stale_pool[i] && rte_mempool_in_use_count(s->tx_link_stale_pool[i])))
    return false;
  return true;
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
      /* flush all the pkts in the tx ring desc; mask a ring that did not drain */
      if (pad && mt_txq_flush(s->queue[i], pad) < 0 && tv_uinit_should_mark_fatal(s, i))
        mt_txq_fatal_error(s->queue[i]);
      mt_txq_put(s->queue[i]);
      s->queue[i] = NULL;
    }

    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (s->pad[i][j]) {
        rte_pktmbuf_free(s->pad[i][j]);
        s->pad[i][j] = NULL;
      }
    }
    for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX; j++) {
      if (s->trim_pad[i][j]) {
        rte_pktmbuf_free(s->trim_pad[i][j]);
        s->trim_pad[i][j] = NULL;
      }
    }
    if (s->pre_arm_pad[i]) {
      rte_pktmbuf_free(s->pre_arm_pad[i]);
      s->pre_arm_pad[i] = NULL;
    }
  }

  return 0;
}

/* the pre-arm's pads: an uncut one where it is longer than the NORMAL pad, and the cut
 * ones, ST_TX_VIDEO_RL_TRIM_COARSE and _FINE bytes shorter; only on an RL port whose
 * NORMAL pkt is still a frame 64 bytes short */
static int tv_init_trim_pads(struct mtl_main_impl* impl,
                             struct st_tx_video_session_impl* s, int i,
                             enum mtl_port port, struct rte_mempool* pool) {
  const uint16_t trims[ST_TX_VIDEO_RL_TRIM_PAD_MAX] = {ST_TX_VIDEO_RL_TRIM_COARSE,
                                                       ST_TX_VIDEO_RL_TRIM_FINE};
  uint32_t normal = s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size;
  uint16_t size = st_tx_video_rl_pad_size(normal);

  if (s->pacing_way[i] != ST21_TX_PACING_WAY_RL ||
      normal < ST_TX_VIDEO_RL_TRIM_COARSE + RTE_ETHER_MIN_LEN)
    return 0;
  if (size != normal) {
    s->pre_arm_pad[i] = mt_build_pad(impl, pool, port, RTE_ETHER_TYPE_IPV4, size);
    if (!s->pre_arm_pad[i]) return -ENOMEM;
  }
  for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX; j++) {
    s->trim_pad[i][j] =
        mt_build_pad(impl, pool, port, RTE_ETHER_TYPE_IPV4, (uint16_t)(size - trims[j]));
    if (!s->trim_pad[i][j]) return -ENOMEM;
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
    mt_pacing_train_bps_result_search(impl, port, flow.bytes_per_sec,
                                      &flow.bytes_per_sec);
    mt_memcpy(&flow.dip_addr, &s->ops.dip_addr[i], MTL_IP_ADDR_LEN);
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
    if (tv_init_trim_pads(impl, s, i, port, pad_mempool) < 0) {
      tv_uinit_hw(s);
      return -ENOMEM;
    }
  }

  return 0;
}

static int tv_mempool_free_owned(struct rte_mempool** pool) {
  if (!*pool) return 0;
  if (rte_mempool_in_use_count(*pool)) {
    warn("%s, still has mbuf in mempool %s\n", __func__, (*pool)->name);
    return -EBUSY;
  }

  mt_mempool_free(*pool);
  *pool = NULL;
  return 0;
}

/* takes out into idle the parked pools whose mbufs are all back, parked min_ns ago or
 * more: an app call that read one is done. Returns how many. */
static int tv_mempool_unpark(struct st_tx_video_session_impl* s, uint64_t min_ns,
                             struct rte_mempool** idle) {
  uint64_t now = mt_get_tsc(s->impl);
  int n = 0;

  for (int i = 0; i < ST_TX_VIDEO_PARKED_MEMPOOLS; i++) {
    struct rte_mempool* pool = s->mbuf_mempool_parked[i];
    if (!pool || now < s->mbuf_mempool_parked_tsc[i] ||
        now - s->mbuf_mempool_parked_tsc[i] < min_ns || rte_mempool_in_use_count(pool))
      continue;
    idle[n++] = pool;
    s->mbuf_mempool_parked[i] = NULL;
    for (int p = 0; p < MTL_SESSION_PORT_MAX; p++) {
      if (s->tx_link_stale_pool[p] != pool) continue;
      s->tx_link_stale_pool[p] = NULL;
      if (s->tx_link_stale & (1 << p)) s->tx_link_live |= 1 << p;
    }
  }
  return n;
}

/* frees the pools tv_mempool_unpark() takes out; -EBUSY while one stays parked */
static int tv_mempool_reap(struct st_tx_video_session_impl* s, uint64_t min_ns) {
  struct rte_mempool* idle[ST_TX_VIDEO_PARKED_MEMPOOLS];
  int n = tv_mempool_unpark(s, min_ns, idle);

  for (int i = 0; i < n; i++) mt_mempool_free(idle[i]);
  for (int i = 0; i < ST_TX_VIDEO_PARKED_MEMPOOLS; i++)
    if (s->mbuf_mempool_parked[i]) return -EBUSY;
  return 0;
}

static int tv_mempool_free(struct st_tx_video_session_impl* s) {
  int ret = 0;

  if (tv_mempool_reap(s, 0) < 0) {
    warn("%s(%d), a parked mempool still in use\n", __func__, s->idx);
    ret = -EBUSY;
  }

  if (s->mbuf_mempool_chain && !s->tx_mono_pool) {
    if (tv_mempool_free_owned(&s->mbuf_mempool_chain) < 0) ret = -EBUSY;
  } else {
    s->mbuf_mempool_chain = NULL;
  }
  if (s->mbuf_mempool_copy_chain && !s->tx_mono_pool) {
    if (tv_mempool_free_owned(&s->mbuf_mempool_copy_chain) < 0) ret = -EBUSY;
  } else {
    s->mbuf_mempool_copy_chain = NULL;
  }

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i]) {
      if (!s->mbuf_mempool_reuse_rx[i] && !s->tx_mono_pool) {
        if (tv_mempool_free_owned(&s->mbuf_mempool_hdr[i]) < 0) ret = -EBUSY;
      } else {
        s->mbuf_mempool_hdr[i] = NULL;
      }
    }
  }

  return ret;
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

/* dedicated queues, no RTCP ring, and frames of rs pkts or more, whose 2 * rs descriptors
 * free the batch the frame before ended in */
static bool tv_tx_reclaims(struct mtl_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  if (!st20_is_frame_type(s->ops.type) || (s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP))
    return false;
  for (int i = 0; i < s->ops.num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    uint16_t rs_thresh = mt_if_tx_reclaim_rs_thresh(impl, port);
    if (!rs_thresh || s->st20_total_pkts < rs_thresh || mt_user_shared_txq(impl, port))
      return false;
  }
  return true;
}

/* the queue's sent pkts, 2 descriptors each, or the larger RTCP ring, and the builder
 * ring's unsent ones: at least two swap buffers past them */
static bool tv_deep_port_capable_chain(struct mtl_main_impl* impl,
                                       struct st_tx_video_session_impl* s,
                                       enum mtl_port s_port) {
  int max_buffer_nb = mt_if_nb_tx_desc(impl, s_port) / 2;

  if (s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP)
    max_buffer_nb = RTE_MAX(max_buffer_nb, tv_rtcp_buffer_size(impl, s, s_port));
  max_buffer_nb += (int)s->ring_count;
  if ((s->st20_total_pkts * (s->st20_frames_cnt - 1)) < max_buffer_nb) {
    info("%s(%d), copy mode, %d pkts held on s_port %d, st20_total_pkts %d\n", __func__,
         s->idx, max_buffer_nb, s_port, s->st20_total_pkts);
    return false;
  }
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
    if (mt_if_tx_deep(impl, s_port)) {
      if (!tv_deep_port_capable_chain(impl, s, s_port)) return false;
      continue;
    }
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

/* zero copy unless disabled or any port can't chain; a reclaiming session gets a frame
 * once the earlier ones are back, so any 2 buffers do */
static void tv_init_tx_chain(struct mtl_main_impl* impl,
                             struct st_tx_video_session_impl* s) {
  s->reclaim.enabled = tv_tx_reclaims(impl, s);
  s->tx_no_chain = mt_user_tx_no_chain(impl) || !tv_has_chain_buf(s) ||
                   !(s->reclaim.enabled || tv_pkts_capable_chain(impl, s));
  if (s->tx_no_chain) s->reclaim.enabled = false;
}

/* a recycled direct mbuf may not be written (PA cross-page copy, mono pool headers) or
 * resent (RTCP); ice, iavf, igc free a dedicated queue's mbufs only after DD, in order */
static bool tv_tx_direct_chain(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s) {
  if (s->tx_no_chain || !st20_is_frame_type(s->ops.type) || s->tx_mono_pool) return false;
  if ((s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP) || impl->iova_mode == RTE_IOVA_PA)
    return false;
  for (int i = 0; i < s->ops.num_port; i++) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
    enum mt_driver_type drv = mt_if(impl, port)->drv_info.drv_type;
    if (drv != MT_DRV_ICE && drv != MT_DRV_IAVF && drv != MT_DRV_IGC) return false;
    if (mt_user_shared_txq(impl, port)) return false;
  }
  return true;
}

/* mbufs a pool needs for session port i: a descriptor each and the builder ring; deep,
 * what the session holds and the ring share. The RTCP and RTP rings come on top. */
static unsigned int tv_mempool_n(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s, int i) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, i);
  bool deep = mt_if_tx_deep(impl, port);
  unsigned int n = mt_if_nb_tx_desc(impl, port) + s->ring_count;

  if (deep) {
    unsigned int trs_bulks = s->pacing_way[i] == ST21_TX_PACING_WAY_RL ? 2 : 1;
    unsigned int held = s->ring_count + trs_bulks * s->bulk +
                        mt_if_tx_pool_ring_share(impl, port, s->tx_no_chain ? 1 : 2, 1);
    /* a migrated session leaves mbufs in the cache of a second lcore */
    n = mt_user_tx_video_migrate(impl) ? RTE_MAX(n, held + MT_MBUF_CACHE_SLACK) : held;
  }
  if (s->ops.flags & ST20_TX_FLAG_ENABLE_RTCP)
    n += deep ? tv_rtcp_buffer_size(impl, s, port) : s->ops.rtcp.buffer_size;
  if (s->ops.type == ST20_TYPE_RTP_LEVEL) n += s->ops.rtp_ring_size;
  return n;
}

/* a padded frame copies the chain of each pkt across a line end: one a line the held
 * pkts of chain_n span and one more a frame they touch, plus the copy pool's own slack */
static unsigned int tv_copy_mempool_n(struct st_tx_video_session_impl* s,
                                      unsigned int chain_n) {
  uint64_t held = chain_n - MT_MBUF_CACHE_SLACK;

  return held * s->st20_pkt_len / s->st20_bytes_in_line + held / s->st20_total_pkts + 2 +
         MT_MBUF_CACHE_SLACK;
}

struct tv_mempools {
  struct rte_mempool* hdr[MTL_SESSION_PORT_MAX];
  struct rte_mempool* chain;
  struct rte_mempool* copy_chain;
};

/* renew: new pools in place of the ones the session owns, which stay on a failure */
static int tv_mempool_init(struct mtl_main_impl* impl,
                           struct st_tx_video_sessions_mgr* mgr,
                           struct st_tx_video_session_impl* s, bool renew) {
  struct st20_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum mtl_port port;
  unsigned int n;
  uint16_t hdr_room_size = 0;
  uint16_t chain_room_size = 0;
  struct tv_mempools p = {0};

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
      p.hdr[i] = NULL; /* reuse rx mempool for zero copy */
    } else if (s->tx_mono_pool) {
      p.hdr[i] = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx, p.hdr[i],
           i);
    } else {
      n = tv_mempool_n(impl, s, i);
      if (s->mbuf_mempool_hdr[i] && !renew) {
        warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
        p.hdr[i] = s->mbuf_mempool_hdr[i];
      } else {
        char pool_name[32];
        snprintf(pool_name, 32, "%sM%dS%dP%d_HDR_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
                 i, s->recovery_idx);
        p.hdr[i] = mt_mempool_create_by_socket(impl, pool_name, n, MT_MBUF_CACHE_SIZE,
                                               sizeof(struct mt_muf_priv_data),
                                               hdr_room_size, s->socket_id);
        if (!p.hdr[i]) goto err;
      }
    }
  }

  /* payload(chain) mbuf pool on primary port; the redundant pkt shares it, so with a deep
   * port it is sized for the port that keeps more */
  if (!s->tx_no_chain) {
    bool deep = mt_tx_deep_session(impl, s->port_maps, num_port);
    port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    n = tv_mempool_n(impl, s, MTL_SESSION_PORT_P);
    for (int i = 1; deep && i < num_port; i++) n = RTE_MAX(n, tv_mempool_n(impl, s, i));

    if (s->tx_mono_pool) {
      p.chain = mt_sys_tx_mempool(impl, port);
      info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx, p.chain);
    } else {
      char pool_name[32];
      snprintf(pool_name, 32, "%sM%dS%d_CHAIN_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
               s->recovery_idx);
      p.chain = mt_mempool_create_by_socket(impl, pool_name, n, MT_MBUF_CACHE_SIZE, 0,
                                            chain_room_size, s->socket_id);
      if (!p.chain) goto err;

      /* has copy (not attach extbuf) and chain mbuf, create a special mempool */
      if (s->st20_linesize > (size_t)s->st20_bytes_in_line &&
          s->ops.packing != ST20_PACKING_GPM_SL) {
        chain_room_size = s->st20_pkt_len;
        if (deep)
          n = tv_copy_mempool_n(s, n);
        else
          n /= s->st20_total_pkts / s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
        char pool_name[32];
        snprintf(pool_name, 32, "%sM%dS%d_COPY_%d", ST_TX_VIDEO_PREFIX, mgr->idx, idx,
                 s->recovery_idx);
        p.copy_chain = mt_mempool_create_by_socket(impl, pool_name, n, MT_MBUF_CACHE_SIZE,
                                                   0, chain_room_size, s->socket_id);
        if (!p.copy_chain) goto err;
      }
    }
  }

  for (int i = 0; i < num_port; i++)
    __atomic_store_n(&s->mbuf_mempool_hdr[i], p.hdr[i], __ATOMIC_RELEASE);
  if (!s->tx_no_chain) {
    __atomic_store_n(&s->mbuf_mempool_chain, p.chain, __ATOMIC_RELEASE);
    if (p.copy_chain)
      __atomic_store_n(&s->mbuf_mempool_copy_chain, p.copy_chain, __ATOMIC_RELEASE);
  }
  return 0;

err:
  if (!s->tx_mono_pool) {
    for (int i = 0; i < num_port; i++)
      if (p.hdr[i] && p.hdr[i] != s->mbuf_mempool_hdr[i]) mt_mempool_free(p.hdr[i]);
    if (p.chain) mt_mempool_free(p.chain);
    if (p.copy_chain) mt_mempool_free(p.copy_chain);
  }
  return -ENOMEM;
}

/* the pools the session owns, NULL for one it only uses */
static int tv_mempools_owned(struct st_tx_video_session_impl* s, struct tv_mempools* p) {
  int n = 0;

  memset(p, 0, sizeof(*p));
  if (s->tx_mono_pool) return 0;
  p->chain = s->mbuf_mempool_chain;
  p->copy_chain = s->mbuf_mempool_copy_chain;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++)
    if (!s->mbuf_mempool_reuse_rx[i]) p->hdr[i] = s->mbuf_mempool_hdr[i];
  n += !!p->chain + !!p->copy_chain;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) n += !!p->hdr[i];
  return n;
}

static void tv_mempool_park(struct st_tx_video_session_impl* s,
                            struct rte_mempool* pool) {
  for (int i = 0; pool && i < ST_TX_VIDEO_PARKED_MEMPOOLS; i++) {
    if (s->mbuf_mempool_parked[i]) continue;
    s->mbuf_mempool_parked[i] = pool;
    s->mbuf_mempool_parked_tsc[i] = mt_get_tsc(s->impl);
    return;
  }
}

/* New pools for a TX queue recovery, allocated before publishing so st20_tx_get_mbuf()
 * of the app thread never reads a NULL or freed pool. Each old pool parks: the dead
 * queue or the app thread may still hold it. With no room to park them all, the old
 * pools stay. */
static int tv_mempool_renew(struct mtl_main_impl* impl,
                            struct st_tx_video_session_impl* s) {
  struct tv_mempools old;
  int n = tv_mempools_owned(s, &old), room = 0;

  tv_mempool_reap(s, ST_TX_VIDEO_PARK_NS);
  for (int i = 0; i < ST_TX_VIDEO_PARKED_MEMPOOLS; i++)
    room += !s->mbuf_mempool_parked[i];
  if (n > room) return -ENOSPC;

  s->recovery_idx++;
  int ret = tv_mempool_init(impl, s->mgr, s, true);
  if (ret < 0) return ret;
  /* Shared mono-pool and rx-reuse legs have old.hdr[i] == NULL and rely on burst-fail
   * detection if the sibling stays dead after recovery. */
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    tv_mempool_park(s, old.hdr[i]);
    if ((s->tx_link_stale & (1 << i)) && !s->tx_link_stale_pool[i])
      s->tx_link_stale_pool[i] = old.hdr[i];
  }
  tv_mempool_park(s, old.chain);
  tv_mempool_park(s, old.copy_chain);
  return 0;
}

static int tv_init_packet_ring(struct st_tx_video_sessions_mgr* mgr,
                               struct st_tx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;

  if (s->ops.flags & ST20_TX_FLAG_USER_PACING) {
    static const struct rte_mbuf_dynfield user_tai_desc = {
        .name = "mtl_tx_video_user_tai",
        .size = sizeof(uint64_t),
        .align = __alignof__(uint64_t),
    };
    int offset = rte_mbuf_dynfield_register(&user_tai_desc);
    if (offset < 0) {
      err("%s(%d,%d), user tai dynfield register fail %d\n", __func__, mgr_idx, idx,
          rte_errno);
      return -rte_errno;
    }
    s->user_tai_offset = offset;
  }

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

static void tv_transmitter_port_state_init(struct st_tx_video_session_impl* s, int i) {
  memset(s->inflight[i], 0, sizeof(s->inflight[i]));
  memset(s->trs_inflight[i], 0, sizeof(s->trs_inflight[i]));
  memset(s->trs_inflight2[i], 0, sizeof(s->trs_inflight2[i]));
  s->inflight_cnt[i] = 0;
  s->trs_target_tsc[i] = 0;
  s->trs_inflight_num[i] = 0;
  s->trs_inflight_idx[i] = 0;
  s->trs_pad_inflight_num[i] = 0;
  s->trs_inflight_cnt[i] = 0;
  s->trs_inflight_num2[i] = 0;
  s->trs_inflight_idx2[i] = 0;
  s->trs_inflight_cnt2[i] = 0;
  s->rl_state[i] = ST_TX_VIDEO_RL_STATE_IDLE;
  s->stat_trs_ret_code[i] = 0;
  s->last_burst_succ_time_tsc[i] = 0;
  s->trs_rl_tail[i] = false;
  s->trs_rl_drained[i] = 0;
  s->trs_wire_tsc[i] = 0;
}

static void tv_transmitter_state_init(struct st_tx_video_session_impl* s) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    tv_transmitter_port_state_init(s, i);
    s->tx_queue_recovery_pending[i] = false;
  }
}

/* teardown frees software-held pkts before the queues flush the older ones */
static void tv_frames_hold(struct st_tx_video_session_impl* s) {
  if (!s->tx_direct_chain || !s->st20_frames) return;
  for (uint16_t i = 0; i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (rte_atomic32_read(&frame->refcnt)) rte_mbuf_ext_refcnt_update(frame->sh_cur, 1);
  }
}

/* the counts of tv_frames_hold(), with no callback: the frames stay with the session */
static void tv_frames_unhold(struct st_tx_video_session_impl* s) {
  if (!s->tx_direct_chain || !s->st20_frames) return;
  for (uint16_t i = 0; i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    if (rte_atomic32_read(&frame->refcnt)) rte_mbuf_ext_refcnt_update(frame->sh_cur, -1);
  }
}

/* with no payload out no queue can still send a held frame: it is done, built or not */
static void tv_frames_release(struct st_tx_video_session_impl* s) {
  if (!s->tx_direct_chain || !s->st20_frames) return;
  if (s->mbuf_mempool_chain && rte_mempool_in_use_count(s->mbuf_mempool_chain)) return;
  uint8_t busy_legs = 0;
  for (int p = 0; p < s->ops.num_port; p++) {
    if (s->tx_link_stale_pool[p] && rte_mempool_in_use_count(s->tx_link_stale_pool[p]))
      busy_legs |= 1u << p;
  }
  for (uint16_t i = 0; i < s->st20_frames_cnt; i++) {
    struct st_frame_trans* frame = &s->st20_frames[i];
    for (int j = 0; j < ST_FT_SH_INFO_MAX; j++)
      tv_frame_drop_slot_pins(frame, j, (uint8_t)~busy_legs);
    if (!rte_atomic32_read(&frame->refcnt)) continue;
    if (!rte_mbuf_ext_refcnt_update(frame->sh_cur, -1)) tv_frame_put(frame, __func__);
  }
}

/* Releases every mbuf reference this port still holds in software. Must run before
 * tv_uinit_hw(), which frees and clears s->pad[] and s->pre_arm_pad[]: the pad references
 * retained by failed bursts can only be given back while those pointers are still live.
 *
 * A chain mbuf may carry an extbuf reference to the frame, so freeing it may run
 * tx_video_frame_release_callback() while the NIC still owns earlier pkts of the
 * frame: a direct chain's only reference is its last pkt, so tv_frames_hold() must run
 * first. tv_uinit_hw() still has to precede tv_free_frames(). */
static void tv_transmitter_port_state_cleanup(struct st_tx_video_session_impl* s, int i) {
  struct rte_mbuf* pad = s->trs_pad_inflight[i];

  if (s->inflight[i][0]) rte_pktmbuf_free_bulk(s->inflight[i], s->bulk);
  if (s->trs_inflight_num[i])
    rte_pktmbuf_free_bulk(&s->trs_inflight[i][s->trs_inflight_idx[i]],
                          s->trs_inflight_num[i]);
  if (s->trs_inflight_num2[i])
    rte_pktmbuf_free_bulk(&s->trs_inflight2[i][s->trs_inflight_idx2[i]],
                          s->trs_inflight_num2[i]);
  /* every pad that failed to burst holds a reference on the pad it retries as */
  if (pad)
    for (unsigned int j = 0; j < s->trs_pad_inflight_num[i]; j++) rte_pktmbuf_free(pad);

  tv_transmitter_port_state_init(s, i);
}

void st_tx_video_transmitter_state_cleanup(struct st_tx_video_session_impl* s) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) tv_transmitter_port_state_cleanup(s, i);
}

static int tv_uinit_sw(struct st_tx_video_session_impl* s) {
  if (s->rtp_pending_pkt) {
    rte_pktmbuf_free(s->rtp_pending_pkt);
    s->rtp_pending_pkt = NULL;
  }
  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  /* best effort: on teardown there is no later retry, tv_mempool_free() already
   * warns about any pool it had to leave behind */
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
  ret = tv_mempool_free(s);
  if (ret < 0) return ret;
  ret = tv_mempool_init(impl, mgr, s, false);
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
    while (temp % bytes_per_pkt != 0 && (size_t)temp <= s->st20_frame_size) {
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
    while (temp % bytes_per_pkt != 0 && (size_t)temp <= s->st20_frame_size) {
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
  /* software mbufs, then hw, then pools and frames -- ordering rationale is on
   * tv_transmitter_port_state_cleanup() */
  tv_frame_build_drop(s);
  tv_frames_hold(s);
  st_tx_video_transmitter_state_cleanup(s);
  tv_uinit_hw(s);
  tv_frames_release(s);
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
  tv_transmitter_state_init(s);
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
  while (s->ring_count > (unsigned int)s->st20_total_pkts) {
    s->ring_count /= 2;
  }

  if (st22_frame_ops) {
    /* no chain support for st22 since the pkts for each frame may be very small */
    s->tx_no_chain = true;
  } else {
    tv_init_tx_chain(impl, s);
  }
  s->tx_direct_chain = tv_tx_direct_chain(impl, s);
  if (s->tx_no_chain) {
    info("%s(%d), no chain mbuf support\n", __func__, idx);
  } else {
    info("%s(%d), chain mbuf, direct %s\n", __func__, idx,
         s->tx_direct_chain ? "yes" : "no");
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

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  memset(&s->stat_snapshot, 0, sizeof(s->stat_snapshot));
  s->stat_user_busy_first = true;
  s->stat_last_time = mt_get_monotonic_time();
  mt_stat_u64_init(&s->stat_time);

  for (int i = 0; i < num_port; i++) {
    s->last_burst_succ_time_tsc[i] = mt_get_tsc(impl);
  }

  tv_reset_pacing_epoch(impl, s);
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

struct tv_stat_dump {
  int m_idx;
  int idx;
  char ops_name[ST_MAX_NAME_LEN];
  double time_sec;
  struct st20_tx_user_stats us;
  struct st20_tx_user_stats snap;
  struct st_tx_video_rl_stat rl;
  int64_t vrx_budget_ns;
  int64_t lead_ns; /* the shortest lead its builder measured, see tv_lead_visit() */
  long double round_ns_max; /* its round budget, INFINITY while idle */
  int trs_inflight_cnt;
  int inflight_cnt;
  double cpu_busy_score;
  int pkts_burst;
  int pkts_burst_dummy;
  int pkts_link_down_drop;
  int build_ret_code;
  int trs_ret_code[MTL_SESSION_PORT_MAX];
  bool interlaced;
  bool frames_busy;
  int frames_in_trans;
  uint16_t framebuff_cnt;
  struct mt_stat_u64 stat_time;
  uint32_t max_next_frame_us;
  uint32_t max_notify_frame_us;
};

/* the tasklets skip the session while this holds its lock: copy and reset only, no log */
static void tv_stat_collect(struct st_tx_video_sessions_mgr* mgr,
                            struct st_tx_video_session_impl* s, struct tv_stat_dump* d) {
  uint64_t cur_time_ns = mt_get_monotonic_time();

  d->m_idx = mgr->idx;
  d->idx = s->idx;
  memcpy(d->ops_name, s->ops_name, sizeof(d->ops_name));
  d->time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  s->stat_last_time = cur_time_ns;

  d->us = s->port_user_stats;
  d->snap = s->stat_snapshot;
  s->stat_snapshot = s->port_user_stats;
  d->rl = s->stat_rl;
  memset(&s->stat_rl, 0, sizeof(s->stat_rl));
  d->vrx_budget_ns = tv_vrx_budget_ns(s);
  d->lead_ns = s->lead_min_ns;
  s->lead_min_ns = INT64_MAX;
  /* only a session that streamed in the period has a budget */
  bool streamed = d->us.common.port[MTL_SESSION_PORT_P].frames !=
                      d->snap.common.port[MTL_SESSION_PORT_P].frames ||
                  d->lead_ns != INT64_MAX;
  /* the frame-average round its builder keeps up with, adding a bulk a visit */
  d->round_ns_max = INFINITY;
  if (s->st20_total_pkts && streamed)
    d->round_ns_max = (long double)s->bulk * s->pacing.frame_time / s->st20_total_pkts;

  d->trs_inflight_cnt = s->trs_inflight_cnt[0];
  d->inflight_cnt = s->inflight_cnt[0];
  s->trs_inflight_cnt[0] = 0;
  s->inflight_cnt[0] = 0;
  d->cpu_busy_score = s->stat_cpu_busy_score;
  d->pkts_burst = s->stat_pkts_burst;
  s->stat_pkts_burst = 0;
  d->pkts_link_down_drop = s->stat_pkts_link_down_drop;
  s->stat_pkts_link_down_drop = 0;
  d->pkts_burst_dummy = s->stat_pkts_burst_dummy;
  if (d->us.stat_pkts_dummy != d->snap.stat_pkts_dummy) s->stat_pkts_burst_dummy = 0;
  d->build_ret_code = s->stat_build_ret_code;
  d->trs_ret_code[MTL_SESSION_PORT_P] = s->stat_trs_ret_code[MTL_SESSION_PORT_P];
  d->trs_ret_code[MTL_SESSION_PORT_R] = s->stat_trs_ret_code[MTL_SESSION_PORT_R];
  d->interlaced = s->ops.interlaced;

  d->frames_in_trans = 0;
  d->framebuff_cnt = s->ops.framebuff_cnt;
  if (s->st20_frames) {
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      if (rte_atomic32_read(&s->st20_frames[i].refcnt)) d->frames_in_trans++;
    }
  }
  d->frames_busy = s->st20_frames &&
                   ((d->frames_in_trans > 2) || (d->frames_in_trans >= d->framebuff_cnt));

  d->stat_time = s->stat_time;
  mt_stat_u64_init(&s->stat_time);
  d->max_next_frame_us = s->stat_max_next_frame_us;
  d->max_notify_frame_us = s->stat_max_notify_frame_us;
  s->stat_max_next_frame_us = 0;
  s->stat_max_notify_frame_us = 0;
}

static void tv_stat_log(const struct tv_stat_dump* dump) {
  int m_idx = dump->m_idx, idx = dump->idx;
  double time_sec = dump->time_sec;
  const struct st20_tx_user_stats* us = &dump->us;
  const struct st20_tx_user_stats* snap = &dump->snap;
  uint64_t d;

  uint64_t frames = us->common.port[MTL_SESSION_PORT_P].frames -
                    snap->common.port[MTL_SESSION_PORT_P].frames;
  double framerate = frames / time_sec;

  uint64_t build_p = us->common.port[MTL_SESSION_PORT_P].build -
                     snap->common.port[MTL_SESSION_PORT_P].build;
  uint64_t build_r = us->common.port[MTL_SESSION_PORT_R].build -
                     snap->common.port[MTL_SESSION_PORT_R].build;
  uint64_t bytes_p = us->common.port[MTL_SESSION_PORT_P].bytes -
                     snap->common.port[MTL_SESSION_PORT_P].bytes;
  uint64_t bytes_r = us->common.port[MTL_SESSION_PORT_R].bytes -
                     snap->common.port[MTL_SESSION_PORT_R].bytes;

  notice("TX_VIDEO_SESSION(%d,%d:%s): fps %f frames %" PRIu64 " pkts %" PRIu64 ":%" PRIu64
         " inflight %d:%d\n",
         m_idx, idx, dump->ops_name, framerate, frames, build_p, build_r,
         dump->trs_inflight_cnt, dump->inflight_cnt);
  notice("TX_VIDEO_SESSION(%d,%d): throughput %f Mb/s: %f Mb/s, cpu busy %f\n", m_idx,
         idx, (double)bytes_p * 8 / time_sec / MTL_STAT_M_UNIT,
         (double)bytes_r * 8 / time_sec / MTL_STAT_M_UNIT, dump->cpu_busy_score);
  if (dump->pkts_burst > 0) {
    notice("TX_VIDEO_SESSION(%d,%d): pkts burst %d\n", m_idx, idx, dump->pkts_burst);
  }
  if (dump->pkts_link_down_drop > 0) {
    notice("TX_VIDEO_SESSION(%d,%d): pkts link down drop %d\n", m_idx, idx,
           dump->pkts_link_down_drop);
  }

  d = us->stat_pkts_dummy - snap->stat_pkts_dummy;
  if (d) {
    dbg("TX_VIDEO_SESSION(%d,%d): dummy pkts %" PRIu64 ", burst %u\n", m_idx, idx, d,
        dump->pkts_burst_dummy);
  }

  d = us->stat_epoch_troffset_mismatch - snap->stat_epoch_troffset_mismatch;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): mismatch epoch troffset %" PRIu64 "\n", m_idx, idx,
           d);
  }
  d = us->stat_trans_troffset_mismatch - snap->stat_trans_troffset_mismatch;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): transmitter mismatch troffset %" PRIu64 "\n", m_idx,
           idx, d);
  }
  d = us->stat_trans_recalculate_warmup - snap->stat_trans_recalculate_warmup;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): transmitter recalculate warmup %" PRIu64 "\n", m_idx,
           idx, d);
  }
  const struct st_tx_video_rl_stat* rl = &dump->rl;
  if (rl->vrx_frames || rl->start_late) {
    err("TX_VIDEO_SESSION(%d,%d): %" PRIu64
        " frames may break VRX >= 0, a pkt up to %.1fus past its target over the %.1fus "
        "budget: packet 0 %" PRIu64 " mid-frame %" PRIu64 "; %" PRIu64
        " frames started up to %.1fus late\n",
        m_idx, idx, rl->vrx_frames, (double)rl->vrx_late_max_ns / NS_PER_US,
        (double)dump->vrx_budget_ns / NS_PER_US, rl->vrx_pkt0,
        rl->vrx_frames - rl->vrx_pkt0, rl->start_late,
        (double)rl->start_late_max_ns / NS_PER_US);
  }
  d = us->common.stat_epoch_drop - snap->common.stat_epoch_drop;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): epoch drop %" PRIu64 "\n", m_idx, idx, d);
  }
  d = us->common.stat_epoch_onward - snap->common.stat_epoch_onward;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): epoch onward %" PRIu64 "\n", m_idx, idx, d);
  }
  d = us->common.stat_exceed_frame_time - snap->common.stat_exceed_frame_time;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): build timeout frames %" PRIu64 "\n", m_idx, idx, d);
  }
  d = us->common.stat_error_user_timestamp - snap->common.stat_error_user_timestamp;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): error user timestamp %" PRIu64 "\n", m_idx, idx, d);
  }
  d = us->stat_user_busy - snap->stat_user_busy;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): busy as no ready frame from user %" PRIu64 "\n",
           m_idx, idx, d);
  }
  d = us->stat_lines_not_ready - snap->stat_lines_not_ready;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): query new lines but app not ready %" PRIu64 "\n",
           m_idx, idx, d);
  }
  d = us->stat_vsync_mismatch - snap->stat_vsync_mismatch;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): vsync mismatch cnt %" PRIu64 "\n", m_idx, idx, d);
  }
  d = us->stat_pkts_chain_realloc_fail - snap->stat_pkts_chain_realloc_fail;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): chain pkt realloc fail cnt %" PRIu64 "\n", m_idx,
           idx, d);
    notice("TX_VIDEO_SESSION(%d,%d): SERIOUS MEMORY ISSUE!\n", m_idx, idx);
  }
  if (frames <= 0) {
    warn("TX_VIDEO_SESSION(%d,%d:%s): build ret %d, trs ret %d:%d\n", m_idx, idx,
         dump->ops_name, dump->build_ret_code, dump->trs_ret_code[MTL_SESSION_PORT_P],
         dump->trs_ret_code[MTL_SESSION_PORT_R]);
  }
  uint64_t d_meta = us->stat_user_meta_cnt - snap->stat_user_meta_cnt;
  uint64_t d_meta_pkt = us->stat_user_meta_pkt_cnt - snap->stat_user_meta_pkt_cnt;
  if (d_meta || d_meta_pkt) {
    notice("TX_VIDEO_SESSION(%d,%d): user meta %" PRIu64 " pkt %" PRIu64 "\n", m_idx, idx,
           d_meta, d_meta_pkt);
  }
  d = us->common.stat_recoverable_error - snap->common.stat_recoverable_error;
  if (d) {
    notice("TX_VIDEO_SESSION(%d,%d): recoverable_error %" PRIu64 " \n", m_idx, idx, d);
  }
  d = us->common.stat_unrecoverable_error - snap->common.stat_unrecoverable_error;
  if (d) {
    err("TX_VIDEO_SESSION(%d,%d): unrecoverable_error %" PRIu64 " \n", m_idx, idx, d);
    /* not reset unrecoverable_error */
  }
  if (dump->interlaced) {
    uint64_t d_first = us->stat_interlace_first_field - snap->stat_interlace_first_field;
    uint64_t d_second =
        us->stat_interlace_second_field - snap->stat_interlace_second_field;
    notice("TX_VIDEO_SESSION(%d,%d): interlace first field %" PRIu64
           " second field %" PRIu64 "\n",
           m_idx, idx, d_first, d_second);
  }

  if (dump->frames_busy) {
    notice("TX_VIDEO_SESSION(%d,%d): %d frames are in trans, total %u\n", m_idx, idx,
           dump->frames_in_trans, dump->framebuff_cnt);
  }

  const struct mt_stat_u64* stat_time = &dump->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("TX_VIDEO_SESSION(%d,%d): tasklet time avg %.2fus max %.2fus min %.2fus\n",
           m_idx, idx, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
  }
  if (dump->max_next_frame_us > 8 || dump->max_notify_frame_us > 8) {
    notice("TX_VIDEO_SESSION(%d,%d): get next frame max %uus, notify done max %uus\n",
           m_idx, idx, dump->max_next_frame_us, dump->max_notify_frame_us);
  }
}

/* Frames fall behind once the round averages past the tightest budget or a pkt met the
 * wire unbuilt (lead_ns < 0); round_ns 0 judges the lead only. With epochs dropped and
 * the round in budget, a frame the app gave late started late. */
static void tv_behind_err(int m_idx, uint64_t round_ns, long double round_ns_max,
                          int64_t lead_ns, uint64_t epoch_drop) {
  char round[64] = "", lead[32] = "";

  if (round_ns > round_ns_max)
    (void)snprintf(round, sizeof(round), " round %.1fus past the %.1Lfus budget,",
                   (double)round_ns / NS_PER_US, round_ns_max / NS_PER_US);
  if (lead_ns < 0)
    (void)snprintf(lead, sizeof(lead), " lead %.1fus,", (double)lead_ns / NS_PER_US);
  if (!round[0] && lead[0] && epoch_drop)
    err("TX_VIDEO_SESSION(%d):%s frames fall behind: the app gave a frame late, epoch "
        "drop %" PRIu64 "\n",
        m_idx, lead, epoch_drop);
  else if (round[0] || lead[0])
    err("TX_VIDEO_SESSION(%d):%s%s frames fall behind: move sessions off this "
        "scheduler\n",
        m_idx, round, lead);
}

static uint64_t tv_stat_epoch_drop(const struct tv_stat_dump* dump) {
  return dump->us.common.stat_epoch_drop - dump->snap.common.stat_epoch_drop;
}

static int tv_detach(struct st_tx_video_sessions_mgr* mgr,
                     struct st_tx_video_session_impl* s) {
  struct tv_stat_dump dump;

  tv_stat_collect(mgr, s, &dump);
  tv_stat_log(&dump);
  tv_behind_err(mgr->idx, 0, INFINITY, dump.lead_ns, tv_stat_epoch_drop(&dump));
  atomic_store_explicit(&mgr->trs_refused[s->idx], 0, memory_order_relaxed);
  tv_uinit(s);
  return 0;
}

static int tv_init(struct st_tx_video_session_impl* s, int idx) {
  s->idx = idx;
  s->lead_min_ns = INT64_MAX;
  /* a call of the transmitter it leaves may match one of its new scheduler's */
  memset(s->trs_released, 0, sizeof(s->trs_released));
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
    if (!mt_user_random_src_port(impl))
      s->st20_src_port[i] =
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

/* For the public APIs on s: 0 with its lock held and its slot in *mgr / *idx; -EIO if
 * neither try found s in the slot it read, i.e. migrations raced both. In a callback, on
 * the scheduler's own thread, -EBUSY rather than spin on the lock. */
static int tv_api_lock(struct st_tx_video_session_impl* s,
                       struct st_tx_video_sessions_mgr** mgr, int* idx) {
  /* a migration moves s before it releases the old slot, so one retry finds it */
  for (int tries = 0; tries < 2; tries++) {
    struct st_tx_video_sessions_mgr* m = s->mgr;
    int i = s->idx;

    /* in the session's own callback a holder waits for that callback, so no spinning */
    if (mt_sch_on_own_thread(mt_sch_instance(m->parent, m->idx)) &&
        atomic_load_explicit(&m->dp_slot.idx, memory_order_relaxed) == i) {
      if (!rte_spinlock_trylock(&m->mutex[i])) return -EBUSY;
    } else if (!tx_video_session_get(m, i)) {
      continue;
    }
    if (m->sessions[i] == s) {
      *mgr = m;
      *idx = i;
      return 0;
    }
    tx_video_session_put(m, i);
  }
  return -EIO;
}

static int tv_mgr_update_dst(struct st_tx_video_session_impl* s,
                             struct st_tx_dest_info* dst) {
  struct st_tx_video_sessions_mgr* mgr = s->mgr;
  int idx;

  /* in a callback the ARP lookup of the new destination would stall the scheduler */
  if (mt_sch_on_own_thread(mt_sch_instance(mgr->parent, mgr->idx))) return -EBUSY;
  int ret = tv_api_lock(s, &mgr, &idx);
  if (ret < 0) return ret;

  ret = tv_update_dst(mgr->parent, s, dst);
  tx_video_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, mgr->idx, idx, ret);
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
  mt_sch_dp_slot_init(&mgr->dp_slot);

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
  struct mtl_sch_impl* sch = mt_sch_instance(mgr->parent, mgr->idx);
  struct st_tx_video_session_impl* s;
  struct tv_stat_dump dump;
  struct rte_mempool* idle[ST_TX_VIDEO_PARKED_MEMPOOLS];
  long double round_ns_max = INFINITY;
  int64_t lead_ns = INT64_MAX;
  uint64_t epoch_drop = 0;
  bool lead_without_drop = false;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = tx_video_session_get_timeout(mgr, j, ST_TX_VIDEO_STAT_TIMEOUT_US);
    if (!s) continue;
    tv_stat_collect(mgr, s, &dump);
    /* the lock keeps a recovery from parking meanwhile */
    int n = tv_mempool_unpark(s, ST_TX_VIDEO_PARK_NS, idle);
    tx_video_session_put(mgr, j);
    for (int i = 0; i < n; i++) mt_mempool_free(idle[i]);
    tv_stat_log(&dump);
    round_ns_max = RTE_MIN(round_ns_max, dump.round_ns_max);
    lead_ns = RTE_MIN(lead_ns, dump.lead_ns);
    uint64_t s_drop = tv_stat_epoch_drop(&dump);
    if (dump.lead_ns < 0) {
      if (s_drop)
        epoch_drop += s_drop;
      else
        lead_without_drop = true;
    }
  }

  /* a sleeping scheduler's loop avg holds its sleeps */
  tv_behind_err(mgr->idx, sch->allow_sleep ? 0 : mt_sch_avg_ns_loop(sch), round_ns_max,
                lead_ns, lead_without_drop ? 0 : epoch_drop);

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
  /* the lead it measured on the scheduler it leaves */
  tv_behind_err(s->mgr->idx, 0, INFINITY, s->lead_min_ns, 0);
  /* under both slots' locks: the new scheduler's late entries wait for its retry too */
  uint8_t refused =
      atomic_exchange_explicit(&s->mgr->trs_refused[s->idx], 0, memory_order_relaxed);
  s->mgr = mgr;
  tv_init(s, idx);
  atomic_store_explicit(&mgr->trs_refused[idx], refused, memory_order_relaxed);
  return 0;
}

/* Remove any session ports that map to a down physical port.
 * When a down port is found at index i, all further entries are shifted down
 * one slot (port[i] = port[i+1], ...) and num_port is decremented.
 * Only ops->port[] is shifted — all other per-port arrays (dip_addr, udp_port, etc.)
 * are left untouched; with the reduced num_port they are simply never indexed.
 * Returns -EIO if every port is down (caller must abort). */
static int tv_ops_prune_down_ports(struct mtl_main_impl* impl, struct st20_tx_ops* ops) {
  int num_ports = ops->num_port;

  if (num_ports > MTL_SESSION_PORT_MAX || num_ports <= 0) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    enum mtl_port phy = mt_port_by_name(impl, ops->port[i]);
    if (phy >= MTL_PORT_MAX || !mt_if_port_is_down(impl, phy)) continue;

    warn("%s(%d), port %s is down, it will not be used\n", __func__, i, ops->port[i]);

    /* shift all further port names one slot down */
    for (int j = i; j < num_ports - 1; j++) {
      mt_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      mt_memcpy(ops->dip_addr[j], ops->dip_addr[j + 1], MTL_IP_ADDR_LEN);
      mt_memcpy(ops->tx_dst_mac[j], ops->tx_dst_mac[j + 1], MTL_MAC_ADDR_LEN);
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
    if (!ops->rtp_pkt_size || ops->rtp_pkt_size > TV_RTP_PKT_MAX_SIZE) {
      err("%s, invalid rtp_pkt_size %u, allowed [1:%zu]\n", __func__, ops->rtp_pkt_size,
          TV_RTP_PKT_MAX_SIZE);
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

static int tv_st22_ops_prune_down_ports(struct mtl_main_impl* impl,
                                        struct st22_tx_ops* ops) {
  int num_ports = ops->num_port;

  if (num_ports > MTL_SESSION_PORT_MAX || num_ports <= 0) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    enum mtl_port phy = mt_port_by_name(impl, ops->port[i]);
    if (phy >= MTL_PORT_MAX || !mt_if_port_is_down(impl, phy)) continue;

    warn("%s(%d), port %s is down, it will not be used\n", __func__, i, ops->port[i]);

    for (int j = i; j < num_ports - 1; j++) {
      mt_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      mt_memcpy(ops->dip_addr[j], ops->dip_addr[j + 1], MTL_IP_ADDR_LEN);
      mt_memcpy(ops->tx_dst_mac[j], ops->tx_dst_mac[j + 1], MTL_MAC_ADDR_LEN);
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
    if (!ops->rtp_pkt_size || ops->rtp_pkt_size > TV_RTP_PKT_MAX_SIZE) {
      err("%s, invalid rtp_pkt_size %u, allowed [1:%zu]\n", __func__, ops->rtp_pkt_size,
          TV_RTP_PKT_MAX_SIZE);
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

/* a recovery that cannot go on ends the session; held: tv_frames_hold() ran */
static int tv_recovery_fail(struct st_tx_video_session_impl* s, bool held, int ret) {
  if (held) tv_frames_unhold(s);
  s->port_user_stats.common.stat_unrecoverable_error++;
  s->active = false;
  if (s->ops.notify_event) s->ops.notify_event(s->ops.priv, ST_EVENT_FATAL_ERROR, NULL);
  return ret;
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

  uint8_t down_mask = 0, down_unresynced = 0, keep_pins = 1u << s_port;
  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    if (s->tx_queue_recovery_pending[i]) keep_pins |= 1u << i;
    if (i == s_port || !s->queue[i] || s->tx_queue_recovery_pending[i]) continue;
    if (!s->tx_link[i].down &&
        !mt_dev_link_down(impl, mt_port_logic2phy(s->port_maps, i)))
      continue;
    down_mask |= 1 << i;
    if (!s->tx_link[i].down || (s->tx_link_resync & (1 << i))) {
      down_unresynced |= 1 << i;
      if (s->tx_direct_chain) st_tx_video_frame_link_pin(s->trs_frame_started[i], i);
    }
  }
  keep_pins |= down_mask;

  tv_frames_hold(s); /* tv_recovery_release_frames() drops the holds */
  tv_drop_sw_pkts(impl, s);
  /* clean the queue done mbuf */
  mt_txq_done_cleanup(s->queue[s_port]);

  for (uint8_t i = 0; i < s->ops.num_port; i++) {
    /* a leg whose recovery is pending or whose link is down cannot flush its ring */
    if (!s->queue[i] || (keep_pins & (1u << i))) continue;
    struct rte_mbuf* pad = s->pad[i][ST20_PKT_TYPE_NORMAL];
    ret = pad ? mt_txq_recovery_flush(s->queue[i], pad) : 0;
    if (ret < 0) {
      /* its queue may still hold the frames and the pool mbufs: hand none back */
      err("%s(%d,%d), flush port %u fail %d\n", __func__, s_port, idx, i, ret);
      return tv_recovery_fail(s, true, ret);
    }
  }

  if (st20_is_frame_type(s->ops.type) &&
      tv_frames_no_sh_info(s, down_mask, keep_pins, down_unresynced != 0)) {
    err("%s(%d,%d), dead queues hold a frame on each of its %d sh_info\n", __func__,
        s_port, idx, ST_FT_SH_INFO_MAX);
    return tv_recovery_fail(s, true, -EBUSY);
  }

  /* the rate pad_interval and the RL plan were trained on, which a later retrain of
   * tv_rl_bps() for another session must not change */
  uint64_t trained_bps = tv_queue_bps(s->queue[s_port]);
  mt_txq_fatal_error(s->queue[s_port]);
  mt_txq_put(s->queue[s_port]);
  s->queue[s_port] = NULL;

  struct mt_txq_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.bytes_per_sec = tv_rl_bps(s);
  mt_pacing_train_bps_result_search(impl, port, flow.bytes_per_sec, &flow.bytes_per_sec);
  if (trained_bps) flow.bytes_per_sec = trained_bps;
  mt_memcpy(&flow.dip_addr, &s->ops.dip_addr[s_port], MTL_IP_ADDR_LEN);
  flow.dst_port = s->ops.udp_port[s_port];
  s->queue[s_port] = mt_txq_get(impl, port, &flow);
  if (!s->queue[s_port]) {
    err("%s(%d,%d), get new txq fail\n", __func__, s_port, idx);
    return tv_recovery_fail(s, true, -EIO);
  }
  uint16_t queue_id = mt_txq_queue_id(s->queue[s_port]);
  info("%s(%d,%d), new queue_id %u\n", __func__, s_port, idx, queue_id);

  uint8_t port_bit = 1u << s_port;
  s->tx_link_stale &= ~port_bit;
  s->tx_link_live &= ~port_bit;
  s->tx_link_short &= ~port_bit;
  s->tx_link_stale_pool[s_port] = NULL;

  if (st20_is_frame_type(s->ops.type))
    tv_recovery_release_frames(s, s_port, down_mask, keep_pins, down_unresynced != 0);
  if (down_mask) {
    uint64_t now = mt_get_tsc(impl);
    for (int i = 0; i < s->ops.num_port; i++) {
      if (!(down_unresynced & (1 << i))) continue;
      s->tx_link[i].down = true;
      s->tx_link[i].tsc = now;
      s->tx_link_resync &= ~(1 << i);
    }
    tv_link_start_drain(impl, s, now, down_unresynced,
                        tv_link_up_drain_tsc(impl, s, now, keep_pins));
  }

  ret = tv_mempool_renew(impl, s);
  if (ret < 0) {
    err("%s(%d,%d), new mempool fail %d\n", __func__, s_port, idx, ret);
    return tv_recovery_fail(s, false, ret);
  }

  s->port_user_stats.common.stat_recoverable_error++;
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
  mt_memcpy(hdr, &s->s_hdr[s_port], sizeof(*hdr));

  /* set timestamp */
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
  /* indicate it's user meta pkt */
  rtp->row_length = htons(frame->user_meta_data_size | ST20_LEN_USER_META);

  /* init mbuf with ipv4 */
  mt_mbuf_init_ipv4(pkt);

  /* copy user meta */
  void* payload = (uint8_t*)rtp + sizeof(struct st20_rfc4175_rtp_hdr);
  mt_memcpy(payload, frame->user_meta, frame->user_meta_data_size);

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
  s->port_user_stats.stat_user_meta_pkt_cnt++;

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

  ret = tv_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, tv_ops_prune_down_ports fail %d\n", __func__, ret);
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
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);

  if (!ext_frame) {
    err("%s, NULL ext frame\n", __func__);
    ret = -EIO;
    goto out;
  }

  s = s_impl->impl;
  s_idx = s->idx;

  if (ext_frame->buf_len < s->st20_fb_size) {
    err("%s(%d), ext framebuffer size %" PRIu64 " can not hold frame, need %" PRIu64 "\n",
        __func__, s_idx, ext_frame->buf_len, s->st20_fb_size);
    ret = -EIO;
    goto out;
  }
  void* addr = ext_frame->buf_addr;
  if (!addr) {
    err("%s(%d), invalid ext frame address\n", __func__, s_idx);
    ret = -EIO;
    goto out;
  }
  rte_iova_t iova_addr = ext_frame->buf_iova;
  if (iova_addr == MTL_BAD_IOVA || iova_addr == 0) {
    err("%s(%d), invalid ext frame iova 0x%" PRIx64 "\n", __func__, s_idx, iova_addr);
    ret = -EIO;
    goto out;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (addr == s->st20_frames[i].addr) {
      warn_once("%s(%d), buffer %p still in tansport!\n", __func__, s_idx, addr);
    }
  }

  if (idx >= s->st20_frames_cnt) {
    err("%s(%d), invalid idx %d, should be in range [0, %d]\n", __func__, s_idx, idx,
        s->st20_frames_cnt);
    ret = -EIO;
    goto out;
  }
  if (!s->st20_frames) {
    err("%s(%d), st20_frames not valid\n", __func__, s_idx);
    ret = -EINVAL;
    goto out;
  }
  struct st_frame_trans* frame = &s->st20_frames[idx];
  int refcnt = rte_atomic32_read(&frame->refcnt);
  if (refcnt) {
    err("%s(%d), frame %d are not free, refcnt %d\n", __func__, s_idx, idx, refcnt);
    ret = -EINVAL;
    goto out;
  }
  if (!(frame->flags & ST_FT_FLAG_EXT)) {
    err("%s(%d), frame %d are not ext enabled\n", __func__, s_idx, idx);
    ret = -EINVAL;
    goto out;
  }

  frame->addr = addr;
  frame->iova = iova_addr;
  ret = 0;
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

void* st20_tx_get_framebuffer(st20_tx_handle handle, uint16_t idx) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  void* ret_addr = NULL;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, NULL);

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    goto out;
  }
  if (!s->st20_frames) {
    err("%s, st20_frames not allocated\n", __func__);
    goto out;
  }

  ret_addr = s->st20_frames[idx].addr;
  /* an ext frame inside its notify_frame_done */
  if (!ret_addr) ret_addr = s->st20_frames[idx].done_addr;
  if (!ret_addr) err("%s, st20_frames not allocated\n", __func__);
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret_addr;
}

size_t st20_tx_get_framebuffer_size(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  size_t ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, 0);

  s = s_impl->impl;
  ret = s->st20_fb_size;
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st20_tx_get_framebuffer_count(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);

  s = s_impl->impl;
  ret = s->st20_frames_cnt;
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

void* st20_tx_get_mbuf(st20_tx_handle handle, void** usrptr) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = NULL;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, NULL);

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    goto out;
  }

  if (rte_ring_full(packet_ring)) {
    dbg("%s(%d), packet ring is full\n", __func__, idx);
    goto out;
  }

  struct rte_mempool* mp =
      s->tx_no_chain
          ? __atomic_load_n(&s->mbuf_mempool_hdr[MTL_SESSION_PORT_P], __ATOMIC_ACQUIRE)
          : __atomic_load_n(&s->mbuf_mempool_chain, __ATOMIC_ACQUIRE);
  pkt = rte_pktmbuf_alloc(mp);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    goto out;
  }

  size_t hdr_offset = s->tx_no_chain ? sizeof(struct mt_udp_hdr) : 0;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_offset);
out:
  MT_HANDLE_RELEASE(s_impl);
  return pkt;
}

int st20_tx_put_mbuf(st20_tx_handle handle, void* mbuf, uint16_t len) {
  return st20_tx_put_mbuf_tai(handle, mbuf, len, 0);
}

int st20_tx_put_mbuf_tai(st20_tx_handle handle, void* mbuf, uint16_t len, uint64_t tai) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!len) {
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  /* Acquire ref; on failure free the caller's mbuf and return -EIO. */
  if (mt_handle_acquire(&s_impl->lc_destroying, &s_impl->lc_refcnt, &s_impl->type,
                        MT_HANDLE_TX_VIDEO) < 0) {
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    ret = -EIO;
    goto out;
  }

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
    rte_pktmbuf_free(mbuf);
    ret = -EIO;
    goto out;
  }

  if (s->tx_no_chain) len += sizeof(struct mt_udp_hdr);

  if (s->ops.flags & ST20_TX_FLAG_USER_PACING)
    *RTE_MBUF_DYNFIELD(pkt, s->user_tai_offset, uint64_t*) = tai;
  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    ret = -EBUSY;
    goto out;
  }

  ret = 0;
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st20_tx_get_sch_idx(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);

  ret = s_impl->sch->idx;
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st20_tx_get_pacing_params(st20_tx_handle handle, double* tr_offset_ns, double* trs_ns,
                              uint32_t* vrx_pkts) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);

  struct st_tx_video_session_impl* s = s_impl->impl;
  if (tr_offset_ns) *tr_offset_ns = s->pacing.tr_offset;
  if (trs_ns) *trs_ns = s->pacing.trs;
  if (vrx_pkts) *vrx_pkts = s->pacing.vrx;
  MT_HANDLE_RELEASE(s_impl);
  return 0;
}

int st20_tx_get_session_stats(st20_tx_handle handle, struct st20_tx_user_stats* stats) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);
  struct st_tx_video_session_impl* s = s_impl->impl;
  struct st_tx_video_sessions_mgr* mgr;
  int idx;

  int ret = tv_api_lock(s, &mgr, &idx);
  if (!ret) {
    memcpy(stats, &s->port_user_stats, sizeof(*stats));
    tx_video_session_put(mgr, idx);
  }
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st20_tx_reset_session_stats(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EINVAL);
  struct st_tx_video_session_impl* s = s_impl->impl;
  struct st_tx_video_sessions_mgr* mgr;
  int idx;

  int ret = tv_api_lock(s, &mgr, &idx);
  if (!ret) {
    memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
    memset(&s->stat_snapshot, 0, sizeof(s->stat_snapshot));
    tx_video_session_put(mgr, idx);
  }
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st20_tx_free(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct mtl_sch_impl* sch;
  struct st_tx_video_session_impl* s;
  int ret, sch_idx, idx;

  /* in a callback on its scheduler the calling tasklet may still use the session */
  if (s_impl->type == MT_HANDLE_TX_VIDEO && mt_sch_on_own_thread(s_impl->sch)) {
    warn("%s, not from a callback of its scheduler\n", __func__);
    return -EBUSY;
  }

  int _gd =
      mt_handle_begin_destroy(&s_impl->lc_destroying, &s_impl->type, MT_HANDLE_TX_VIDEO);
  if (_gd < 0) {
    if (_gd == -EIO) err("%s, invalid type %d\n", __func__, s_impl->type);
    return _gd;
  }
  mt_handle_drain(&s_impl->lc_refcnt);

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

  MT_HANDLE_GUARD(s_impl, MT_HANDLE_TX_VIDEO, -EIO);

  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s_impl->sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) goto out;

  ret = tv_mgr_update_dst(s, dst);
  if (ret == -EBUSY) {
    warn("%s(%d), not from a callback of its scheduler\n", __func__, idx);
    goto out;
  }
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    goto out;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  ret = 0;
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret;
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

  ret = tv_st22_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, tv_st22_ops_prune_down_ports fail %d\n", __func__, ret);
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
  st20_ops.notify_frame_late = ops->notify_frame_late;
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

  /* in a callback on its scheduler the calling tasklet may still use the session */
  if (s_impl->type == MT_ST22_HANDLE_TX_VIDEO && mt_sch_on_own_thread(s_impl->sch)) {
    warn("%s, not from a callback of its scheduler\n", __func__);
    return -EBUSY;
  }

  int _gd = mt_handle_begin_destroy(&s_impl->lc_destroying, &s_impl->type,
                                    MT_ST22_HANDLE_TX_VIDEO);
  if (_gd < 0) {
    if (_gd == -EIO) err("%s, invalid type %d\n", __func__, s_impl->type);
    return _gd;
  }
  mt_handle_drain(&s_impl->lc_refcnt);

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

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EIO);

  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s_impl->sch->idx;

  ret = st_tx_dest_info_check(dst, s->ops.num_port);
  if (ret < 0) goto out;

  ret = tv_mgr_update_dst(s, dst);
  if (ret == -EBUSY) {
    warn("%s(%d), not from a callback of its scheduler\n", __func__, idx);
    goto out;
  }
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    goto out;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  ret = 0;
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

void* st22_tx_get_mbuf(st22_tx_handle handle, void** usrptr) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = NULL;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, NULL);

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    goto out;
  }

  if (rte_ring_full(packet_ring)) {
    dbg("%s(%d), packet ring is full\n", __func__, idx);
    goto out;
  }

  struct rte_mempool* mp =
      s->tx_no_chain
          ? __atomic_load_n(&s->mbuf_mempool_hdr[MTL_SESSION_PORT_P], __ATOMIC_ACQUIRE)
          : __atomic_load_n(&s->mbuf_mempool_chain, __ATOMIC_ACQUIRE);
  pkt = rte_pktmbuf_alloc(mp);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    goto out;
  }

  size_t hdr_offset = s->tx_no_chain ? sizeof(struct mt_udp_hdr) : 0;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_offset);
out:
  MT_HANDLE_RELEASE(s_impl);
  return pkt;
}

int st22_tx_put_mbuf(st22_tx_handle handle, void* mbuf, uint16_t len) {
  return st22_tx_put_mbuf_tai(handle, mbuf, len, 0);
}

int st22_tx_put_mbuf_tai(st22_tx_handle handle, void* mbuf, uint16_t len, uint64_t tai) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!len) {
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  /* Acquire ref; on failure free the caller's mbuf and return -EIO. */
  if (mt_handle_acquire(&s_impl->lc_destroying, &s_impl->lc_refcnt, &s_impl->type,
                        MT_ST22_HANDLE_TX_VIDEO) < 0) {
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    ret = -EIO;
    goto out;
  }

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
    rte_pktmbuf_free(mbuf);
    ret = -EIO;
    goto out;
  }

  if (s->tx_no_chain) len += sizeof(struct mt_udp_hdr);

  if (s->ops.flags & ST20_TX_FLAG_USER_PACING)
    *RTE_MBUF_DYNFIELD(pkt, s->user_tai_offset, uint64_t*) = tai;
  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    ret = -EBUSY;
    goto out;
  }

  ret = 0;
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

int st22_tx_get_sch_idx(st22_tx_handle handle) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  int ret;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, -EINVAL);

  ret = s_impl->sch->idx;
  MT_HANDLE_RELEASE(s_impl);
  return ret;
}

void* st22_tx_get_fb_addr(st22_tx_handle handle, uint16_t idx) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  void* ret_addr = NULL;

  MT_HANDLE_GUARD(s_impl, MT_ST22_HANDLE_TX_VIDEO, NULL);

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    goto out;
  }
  if (!s->st20_frames || !s->st20_frames[idx].addr) {
    err("%s, st22_frames not allocated\n", __func__);
    goto out;
  }

  if (s->st22_info) {
    ret_addr = s->st20_frames[idx].addr + s->st22_box_hdr_length;
  } else {
    ret_addr = s->st20_frames[idx].addr;
  }
out:
  MT_HANDLE_RELEASE(s_impl);
  return ret_addr;
}
