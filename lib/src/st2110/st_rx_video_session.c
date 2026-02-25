/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_video_session.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_pcap.h"
#include "../mt_ptp.h"
#include "../mt_rtcp.h"
#include "../mt_stat.h"
#include "st_fmt.h"
#include "st_rx_timing_parser.h"

#ifdef MTL_GPU_DIRECT_ENABLED
#include <mtl_gpu_direct/gpu.h>
#endif /* MTL_GPU_DIRECT_ENABLED */

static int rv_init_pkt_handler(struct st_rx_video_session_impl* s);
static int rvs_mgr_update(struct st_rx_video_sessions_mgr* mgr);

static inline struct mtl_main_impl* rv_get_impl(struct st_rx_video_session_impl* s) {
  return s->parent->parent;
}

static inline uint16_t rv_queue_id(struct st_rx_video_session_impl* s,
                                   enum mtl_session_port s_port) {
  return mt_rxq_queue_id(s->rxq[s_port]);
}

static int rv_detector_init(struct st_rx_video_session_impl* s) {
  struct st_rx_video_detector* detector = &s->detector;
  struct st20_detect_meta* meta = &detector->meta;

  detector->status = ST20_DETECT_STAT_DETECTING;
  detector->bpm = true;
  detector->frame_num = 0;
  detector->single_line = true;
  detector->pkt_per_frame = 0;

  meta->width = 0;
  meta->height = 0;
  meta->fps = ST_FPS_MAX;
  meta->packing = ST20_PACKING_MAX;
  meta->interlaced = false;
  return 0;
}

static void rv_detector_calculate_dimension(struct st_rx_video_session_impl* s,
                                            struct st_rx_video_detector* detector,
                                            int max_line_num) {
  struct st20_detect_meta* meta = &detector->meta;

  dbg("%s(%d), interlaced %d, max_line_num %d\n", __func__, s->idx,
      meta->interlaced ? 1 : 0, max_line_num);
  if (meta->interlaced) {
    switch (max_line_num) {
      case 539:
        meta->height = 1080;
        meta->width = 1920;
        break;
      case 239:
        meta->height = 480;
        meta->width = 640;
        break;
      case 359:
        meta->height = 720;
        meta->width = 1280;
        break;
      case 1079:
        meta->height = 2160;
        meta->width = 3840;
        break;
      case 2159:
        meta->height = 4320;
        meta->width = 7680;
        break;
      default:
        err("%s(%d), max_line_num %d\n", __func__, s->idx, max_line_num);
        break;
    }
  } else {
    switch (max_line_num) {
      case 1079:
        meta->height = 1080;
        meta->width = 1920;
        break;
      case 479:
        meta->height = 480;
        meta->width = 640;
        break;
      case 719:
        meta->height = 720;
        meta->width = 1280;
        break;
      case 2159:
        meta->height = 2160;
        meta->width = 3840;
        break;
      case 4319:
        meta->height = 4320;
        meta->width = 7680;
        break;
      default:
        err("%s(%d), max_line_num %d\n", __func__, s->idx, max_line_num);
        break;
    }
  }
}

static void rv_detector_calculate_fps(struct st_rx_video_session_impl* s,
                                      struct st_rx_video_detector* detector) {
  struct st20_detect_meta* meta = &detector->meta;
  int d0 = detector->rtp_tm[1] - detector->rtp_tm[0];
  int d1 = detector->rtp_tm[2] - detector->rtp_tm[1];

  if (abs(d0 - d1) <= 1) {
    dbg("%s(%d), d0 = %d, d1 = %d\n", __func__, s->idx, d0, d1);
    switch (d0) {
      case 750:
        meta->fps = ST_FPS_P120;
        return;
      case 751:
        meta->fps = ST_FPS_P119_88;
        return;
      case 900:
        meta->fps = ST_FPS_P100;
        return;
      case 1500:
        meta->fps = ST_FPS_P60;
        return;
      case 1501:
      case 1502:
        meta->fps = ST_FPS_P59_94;
        return;
      case 1800:
        meta->fps = ST_FPS_P50;
        return;
      case 3000:
        meta->fps = ST_FPS_P30;
        return;
      case 3003:
        meta->fps = ST_FPS_P29_97;
        return;
      case 3600:
        meta->fps = ST_FPS_P25;
        return;
      case 3750:
        meta->fps = ST_FPS_P24;
        return;
      case 3753:
      case 3754:
        meta->fps = ST_FPS_P23_98;
        return;
      default:
        err("%s(%d), err d0 %d d1 %d\n", __func__, s->idx, d0, d1);
        break;
    }
  } else {
    err("%s(%d), err d0 %d d1 %d\n", __func__, s->idx, d0, d1);
  }
}

static void rv_detector_calculate_n_packet(struct st_rx_video_session_impl* s,
                                           struct st_rx_video_detector* detector) {
  int total0 = detector->pkt_num[1] - detector->pkt_num[0];
  int total1 = detector->pkt_num[2] - detector->pkt_num[1];

  if (total0 == total1) {
    detector->pkt_per_frame = total0;
  } else {
    err("%s(%d), err total0 %d total1 %d\n", __func__, s->idx, total0, total1);
  }
}

static void rv_detector_calculate_packing(struct st_rx_video_detector* detector) {
  struct st20_detect_meta* meta = &detector->meta;

  if (detector->bpm)
    meta->packing = ST20_PACKING_BPM;
  else if (detector->single_line)
    meta->packing = ST20_PACKING_GPM_SL;
  else
    meta->packing = ST20_PACKING_GPM;
}

static bool inline rv_is_hdr_split(struct st_rx_video_session_impl* s) {
  return s->is_hdr_split;
}

static bool inline rv_is_dynamic_ext_frame(struct st_rx_video_session_impl* s) {
  return s->ops.query_ext_frame != NULL;
}

static bool inline rv_framebuffer_in_gpu_direct_vram(struct st_rx_video_session_impl* s) {
  return s->ops.gpu_direct_framebuffer_in_vram_device_address;
}

static struct st_frame_trans* rv_get_frame(struct st_rx_video_session_impl* s) {
  struct st_frame_trans* st20_frame;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];

    if (0 == rte_atomic32_read(&st20_frame->refcnt)) {
      dbg("%s(%d), find frame at %d\n", __func__, s->idx, i);
      rte_atomic32_inc(&st20_frame->refcnt);
      return st20_frame;
    }
  }

  dbg("%s(%d), no free frame\n", __func__, s->idx);
  return NULL;
}

static int rv_put_frame(struct st_rx_video_session_impl* s,
                        struct st_frame_trans* frame) {
  MTL_MAY_UNUSED(s);
  dbg("%s(%d), put frame at %d\n", __func__, s->idx, frame->idx);
  rte_atomic32_dec(&frame->refcnt);
  if (s->st22_info)
    MT_USDT_ST22_RX_FRAME_PUT(s->parent->idx, s->idx, frame->idx, frame->addr);
  else
    MT_USDT_ST20_RX_FRAME_PUT(s->parent->idx, s->idx, frame->idx, frame->addr);
  return 0;
}

static int rv_uinit_hdr_split_frame(struct st_rx_video_session_impl* s) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (s->hdr_split_info[i].frames) {
      if (!s->ops.ext_frames) mt_rte_free(s->hdr_split_info[i].frames);
      s->hdr_split_info[i].frames = NULL;
    }
  }

  return 0;
}

static int rv_init_hdr_split_frame(struct st_rx_video_session_impl* s) {
  int num_port = s->ops.num_port;
  int idx = s->idx;
  size_t frame_size = s->st20_frame_size;
  int soc_id;
  uint32_t mbufs_per_frame;
  uint32_t mbufs_total;

  mbufs_per_frame = frame_size / ST_VIDEO_BPM_SIZE;
  if (frame_size % ST_VIDEO_BPM_SIZE) mbufs_per_frame++;
  mbufs_total = mbufs_per_frame * s->st20_frames_cnt;
  /* extra mbufs since frame may not start from zero pos */
  mbufs_total += (mbufs_per_frame - 1);

  for (int i = 0; i < num_port; i++) {
    soc_id = s->socket_id;
    size_t frames_size = mbufs_total * ST_VIDEO_BPM_SIZE;

    if (s->hdr_split_info[i].frames) {
      err("%s(%d, %d), frames malloc already\n", __func__, idx, i);
      return -EIO;
    }

    /* more extra space since rte_mbuf_data_iova_default has offset */
    size_t malloc_size = frames_size + 4096;
    void* frames;
    rte_iova_t frames_iova;

    if (s->ops.ext_frames) {
      struct st20_ext_frame* ext_frame = &s->ops.ext_frames[i];
      frames = ext_frame->buf_addr;
      if (!frames) {
        err("%s(%d, %d), NULL frame for ext frames\n", __func__, idx, i);
        rv_uinit_hdr_split_frame(s);
        return -EIO;
      }
      frames_iova = ext_frame->buf_iova;
      if (!frames_iova) {
        err("%s(%d, %d), no iova for ext frames\n", __func__, idx, i);
        rv_uinit_hdr_split_frame(s);
        return -EIO;
      }
      if (ext_frame->buf_len < malloc_size) {
        err("%s(%d, %d), ext frames size too small, need %" PRIu64 " but only %" PRIu64
            "\n",
            __func__, idx, i, malloc_size, ext_frame->buf_len);
        rv_uinit_hdr_split_frame(s);
        return -EIO;
      }
    } else {
      frames = mt_rte_zmalloc_socket(malloc_size, soc_id);
      if (!frames) {
        err("%s(%d), frames malloc fail for %d, mbufs_total %u\n", __func__, idx, i,
            mbufs_total);
        rv_uinit_hdr_split_frame(s);
        return -ENOMEM;
      }
      frames_iova = rte_malloc_virt2iova(frames);
    }
    s->hdr_split_info[i].frames = frames;
    s->hdr_split_info[i].frames_iova = frames_iova;
    s->hdr_split_info[i].frames_size = frames_size;
    s->hdr_split_info[i].mbufs_per_frame = mbufs_per_frame;
    s->hdr_split_info[i].mbufs_total = mbufs_total;
    info("%s(%d,%d), frames (%p-%p), mbufs_total %u, iova %" PRIx64 "\n", __func__, idx,
         i, frames, frames + frames_size, mbufs_total, s->hdr_split_info[i].frames_iova);
  }

  return 0;
}

static int rv_free_frames(struct st_rx_video_session_impl* s) {
  if (s->st20_frames) {
    struct st_frame_trans* frame;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      st_frame_trans_uinit(frame, s->ops.gpu_context);
    }
    mt_rte_free(s->st20_frames);
    s->st20_frames = NULL;
  }

  rv_uinit_hdr_split_frame(s);

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static rte_iova_t rv_frame_get_offset_iova(struct st_rx_video_session_impl* s,
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

static int rv_frame_create_page_table(struct st_rx_video_session_impl* s,
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

static inline bool rv_frame_payload_cross_page(struct st_rx_video_session_impl* s,
                                               struct st_frame_trans* frame_info,
                                               size_t offset, size_t len) {
  if (frame_info->page_table_len == 0) return false;
  return ((rv_frame_get_offset_iova(s, frame_info, offset + len - 1) -
           rv_frame_get_offset_iova(s, frame_info, offset)) != len - 1);
}

static int rv_alloc_frames(struct mtl_main_impl* impl,
                           struct st_rx_video_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  size_t size = s->st20_uframe_size ? s->st20_uframe_size : s->st20_fb_size;
  struct st_frame_trans* st20_frame;
  void* frame;
  int ret;

  s->st20_frames =
      mt_rte_zmalloc_socket(sizeof(*s->st20_frames) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames) {
    err("%s(%d), st20_frames alloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];
    rte_atomic32_set(&st20_frame->refcnt, 0);
    st20_frame->idx = i;
  }

  if (rv_is_hdr_split(s)) {
    ret = rv_init_hdr_split_frame(s);
    if (ret < 0) {
      rv_free_frames(s);
      return ret;
    }
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];

    if (rv_is_hdr_split(s)) { /* leave to zero for hdr split */
      st20_frame->iova = 0;
      st20_frame->addr = NULL;
      st20_frame->flags = 0;
    } else if (s->ops.ext_frames) {
      frame = s->ops.ext_frames[i].buf_addr;
      if (!frame) {
        err("%s(%d), no external framebuffer\n", __func__, idx);
        rv_free_frames(s);
        return -EIO;
      }
      rte_iova_t frame_iova = s->ops.ext_frames[i].buf_iova;
      if (frame_iova == MTL_BAD_IOVA || frame_iova == 0) {
        err("%s(%d), external framebuffer not mapped to iova\n", __func__, idx);
        rv_free_frames(s);
        return -EIO;
      }
      st20_frame->addr = frame;
      st20_frame->iova = frame_iova;
      st20_frame->flags = ST_FT_FLAG_EXT;
      info("%s(%d), attach external frame %d, addr %p, iova %" PRIu64 "\n", __func__, idx,
           i, frame, frame_iova);
    } else if (rv_is_dynamic_ext_frame(s)) {
      st20_frame->iova = 0; /* detect later */
      st20_frame->addr = NULL;
      st20_frame->flags = 0;
    } else {
#ifdef MTL_GPU_DIRECT_ENABLED
      if (rv_framebuffer_in_gpu_direct_vram(s)) {
        info("%s: using GPU direct feature.\n", __func__);
        GpuContext* gpu = s->ops.gpu_context;
        ret = gpu_allocate_shared_buffer(gpu, &frame, size);
        if (ret < 0) {
          err("%s: failed to allocate GPU memory on vram. ret: %d\n", __func__, ret);
          return -ENOMEM;
        }
      } else
#endif /* MTL_GPU_DIRECT_ENABLED */
        frame = mt_rte_zmalloc_socket(size, soc_id);

      if (!frame) {
        err("%s(%d), frame malloc %" PRIu64 " fail for %d\n", __func__, idx, size, i);
        rv_free_frames(s);
        return -ENOMEM;
      }

      if (rv_framebuffer_in_gpu_direct_vram(s)) {
        st20_frame->flags = ST_FT_FLAG_GPU_MALLOC;
      } else {
        st20_frame->flags = ST_FT_FLAG_RTE_MALLOC;
        st20_frame->iova = rte_malloc_virt2iova(frame);
      }

      st20_frame->addr = frame;

      if (impl->iova_mode == RTE_IOVA_PA && s->dma_dev) {
        ret = rv_frame_create_page_table(s, st20_frame);
        if (ret < 0) {
          rv_free_frames(s);
          return ret;
        }
      }
    }

    /* init user meta */
    st20_frame->user_meta_buffer_size =
        impl->pkt_udp_suggest_max_size - sizeof(struct st20_rfc4175_rtp_hdr);
    st20_frame->user_meta =
        mt_rte_zmalloc_socket(st20_frame->user_meta_buffer_size, soc_id);
    if (!st20_frame->user_meta) {
      err("%s(%d), user_meta malloc %" PRIu64 " fail at %d\n", __func__, idx,
          st20_frame->user_meta_buffer_size, i);
      return -ENOMEM;
    }
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rv_free_rtps(struct st_rx_video_session_impl* s) {
  if (s->rtps_ring) {
    mt_ring_dequeue_clean(s->rtps_ring);
    rte_ring_free(s->rtps_ring);
    s->rtps_ring = NULL;
  }

  return 0;
}

static int rv_alloc_rtps(struct st_rx_video_sessions_mgr* mgr,
                         struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;

  snprintf(ring_name, 32, "%sM%dS%d_RTP", ST_RX_VIDEO_PREFIX, mgr_idx, idx);
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
  s->rtps_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

#ifdef ST_HAS_DPDK_HDR_SPLIT
/* run within the context of receiver lcore */
static int rv_hdrs_mbuf_callback_fn(void* priv, struct rte_eth_hdrs_mbuf* mbuf) {
  struct st_rx_video_session_impl* s = priv;
  struct st_rx_video_hdr_split_info* hdr_split = &s->hdr_split_info[MTL_SESSION_PORT_P];
  uint32_t alloc_idx = hdr_split->mbuf_alloc_idx;
  uint32_t cur_frame_mbuf_idx = hdr_split->cur_frame_mbuf_idx;

  if (cur_frame_mbuf_idx) {
    uint32_t next_frame_start_idx = cur_frame_mbuf_idx + hdr_split->mbufs_per_frame;

    if (alloc_idx == next_frame_start_idx) {
      /* start of next frame, reset if remaining frame space is capable one frame */
      if ((alloc_idx + hdr_split->mbufs_per_frame) >= hdr_split->mbufs_total) {
        dbg("%s(%d), idx reset at idx %u, cur_frame_mbuf_idx %u\n", __func__, s->idx,
            alloc_idx, cur_frame_mbuf_idx);
        /* notify for mismatch frame address */
        if (cur_frame_mbuf_idx % hdr_split->mbufs_per_frame) {
          dbg("%s(%d), idx reset for mismatch frame at idx %u, cur_frame_mbuf_idx %u\n",
              __func__, s->idx, alloc_idx, cur_frame_mbuf_idx);
        }
        alloc_idx = 0;
      }
    }
  } else { /* warm up mbuf stage */
    uint32_t remaining_mbufs = hdr_split->mbufs_total - alloc_idx;
    if (remaining_mbufs < hdr_split->mbufs_per_frame) {
      /* all mbuf ready, start from zero */
      info("%s(%d), alloc idx reset at %u as pool ready\n", __func__, s->idx, alloc_idx);
      alloc_idx = 0;
      hdr_split->mbuf_pool_ready = true;
    }
  }

  mbuf->buf_addr = hdr_split->frames + alloc_idx * ST_VIDEO_BPM_SIZE;
  mbuf->buf_iova = hdr_split->frames_iova + alloc_idx * ST_VIDEO_BPM_SIZE;
  dbg("%s(%d), mbuf alloc idx %u, buf %p\n", __func__, s->idx, alloc_idx, mbuf->buf_addr);

  /* point to next alloc_idx */
  alloc_idx++;
  if (alloc_idx >= hdr_split->mbufs_total) {
    /* only happen if cur_frame_mbuf_idx not get updated since imiss */
    err("%s(%d), alloc idx %u(%p) reset as it reach end %u\n", __func__, s->idx,
        alloc_idx, mbuf->buf_addr, hdr_split->mbufs_total);
    alloc_idx = 0;
    hdr_split->mbuf_pool_ready = true;
  }
  hdr_split->mbuf_alloc_idx = alloc_idx;

  return 0;
}
#endif

static inline void rv_slot_init_frame_size(struct st_rx_video_slot_impl* slot) {
  slot->frame_recv_size = 0;
  slot->pkt_lcore_frame_recv_size = 0;
}

static inline size_t rv_slot_get_frame_size(struct st_rx_video_slot_impl* slot) {
  return slot->frame_recv_size + slot->pkt_lcore_frame_recv_size;
}

static inline void rv_slot_add_frame_size(struct st_rx_video_slot_impl* slot,
                                          size_t size) {
  slot->frame_recv_size += size;
}

static inline void rv_slot_pkt_lcore_add_frame_size(struct st_rx_video_slot_impl* slot,
                                                    size_t size) {
  slot->pkt_lcore_frame_recv_size += size;
}

void rv_slot_dump(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    info("%s(%d), tmstamp %ld recv_size %" PRIu64 " pkts_received %u\n", __func__, i,
         slot->tmstamp, rv_slot_get_frame_size(slot), slot->pkts_received);
  }
}

static int rv_init(struct st_rx_video_sessions_mgr* mgr,
                   struct st_rx_video_session_impl* s, int idx) {
  s->idx = idx;
  s->parent = mgr;
  return 0;
}

static int rv_uinit_slot(struct st_rx_video_session_impl* s) {
  struct st_rx_video_slot_impl* slot;

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];
    if (slot->frame_bitmap) {
      mt_rte_free(slot->frame_bitmap);
      slot->frame_bitmap = NULL;
    }
    if (slot->slice_info) {
      mt_rte_free(slot->slice_info);
      slot->slice_info = NULL;
    }
    if (slot->frame) {
      rv_put_frame(s, slot->frame);
      slot->frame = NULL;
    }
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int rv_init_slot(struct st_rx_video_session_impl* s) {
  int soc_id = s->socket_id;
  int idx = s->idx;
  size_t bitmap_size = s->st20_frame_bitmap_size;
  struct st_rx_video_slot_impl* slot;
  uint8_t* frame_bitmap;
  struct st_rx_video_slot_slice_info* slice_info;
  enum st20_type type = s->ops.type;

  /* init slot */
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    slot->idx = i;
    slot->frame = NULL;
    rv_slot_init_frame_size(slot);
    slot->pkts_received = 0;
    slot->tmstamp = -1;
    slot->seq_id_got = false;
    frame_bitmap = mt_rte_zmalloc_socket(bitmap_size, soc_id);
    if (!frame_bitmap) {
      err("%s(%d), bitmap malloc %" PRIu64 " fail\n", __func__, idx, bitmap_size);
      return -ENOMEM;
    }
    slot->frame_bitmap = frame_bitmap;

    if (ST20_TYPE_SLICE_LEVEL == type) {
      slice_info = mt_rte_zmalloc_socket(sizeof(*slice_info), soc_id);
      if (!slice_info) {
        err("%s(%d), slice malloc fail\n", __func__, idx);
        return -ENOMEM;
      }
      slot->slice_info = slice_info;
    }
  }
  s->slot_idx = -1;

  if (s->ops.flags & ST20_RX_FLAG_ENABLE_RTCP)
    s->slot_max = ST_RX_VIDEO_RTCP_SLOT_NUM;
  else if (s->ops.num_port > 1)
    s->slot_max = ST_RX_VIDEO_REDUNDANT_SLOT_NUM;
  else
    s->slot_max = 1;

  if (s->slot_max > ST_VIDEO_RX_REC_NUM_OFO) {
    err("%s(%d), slot_max %d exceed max %d\n", __func__, idx, s->slot_max,
        ST_VIDEO_RX_REC_NUM_OFO);
    return -EINVAL;
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static inline int rv_notify_frame_ready(struct st_rx_video_session_impl* s, void* frame,
                                        struct st20_rx_frame_meta* meta) {
  int ret;
  uint64_t tsc_start = 0;
  bool time_measure = mt_sessions_time_measure(s->impl);

  if (time_measure) tsc_start = mt_get_tsc(s->impl);
  ret = s->ops.notify_frame_ready(s->ops.priv, frame, meta);
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(s->impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
  }

  return ret;
}

static inline int st22_notify_frame_ready(struct st_rx_video_session_impl* s, void* frame,
                                          struct st22_rx_frame_meta* meta) {
  int ret;
  uint64_t tsc_start = 0;
  struct st22_rx_video_info* st22_info = s->st22_info;
  bool time_measure = mt_sessions_time_measure(s->impl);

  if (time_measure) tsc_start = mt_get_tsc(s->impl);
  ret = st22_info->notify_frame_ready(s->ops.priv, frame, meta);
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(s->impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_frame_us = RTE_MAX(s->stat_max_notify_frame_us, delta_us);
  }

  return ret;
}

static int rv_usdt_dump_frame(struct mtl_main_impl* impl,
                              struct st_rx_video_session_impl* s,
                              struct st_frame_trans* frame) {
  struct st_rx_video_sessions_mgr* mgr = s->parent;
  int idx = s->idx;
  int fd;
  char usdt_dump_path[64];
  struct st20_rx_ops* ops = &s->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st20rx_m%ds%d_%d_%d_XXXXXX.yuv", mgr->idx, idx, ops->width,
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
    MT_USDT_ST20_RX_FRAME_DUMP(mgr->idx, s->idx, usdt_dump_path, frame->addr, n);
  }

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static int rv_st22_usdt_dump_frame(struct mtl_main_impl* impl,
                                   struct st_rx_video_session_impl* s,
                                   struct st_frame_trans* frame, size_t size) {
  struct st_rx_video_sessions_mgr* mgr = s->parent;
  int idx = s->idx;
  int fd;
  char usdt_dump_path[64];
  struct st20_rx_ops* ops = &s->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st22rx_m%ds%d_%d_%d_XXXXXX.raw", mgr->idx, idx, ops->width,
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
    MT_USDT_ST22_RX_FRAME_DUMP(mgr->idx, s->idx, usdt_dump_path, frame->addr, n);
  }

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static void rv_frame_notify(struct st_rx_video_session_impl* s,
                            struct st_rx_video_slot_impl* slot) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_rx_frame_meta* meta = &slot->meta;
  struct st_frame_trans* frame = slot->frame;

  if (s->enable_timing_parser) {
    for (int s_port = 0; s_port < ops->num_port; s_port++) {
      struct st_rv_tp_slot* tp_slot = &s->tp->slots[slot->idx][s_port];
      rv_tp_slot_parse_result(s, s_port, tp_slot);
      if (s->enable_timing_parser_meta) {
        meta->tp[s_port] = &tp_slot->meta;
      }
    }
  }

  dbg("%s(%d), start\n", __func__, s->idx);
  meta->width = ops->width;
  meta->height = ops->height;
  meta->fmt = ops->fmt;
  meta->fps = ops->fps;
  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->timestamp_first_pkt = slot->timestamp_first_pkt;
  /* calculate FPT */
  uint64_t epochs = (double)meta->timestamp_first_pkt / s->frame_time;
  uint64_t epoch_tmstamp = (double)epochs * s->frame_time;
  double fpt_delta = (double)meta->timestamp_first_pkt - epoch_tmstamp;
  dbg("%s(%d): fpt_delta %f\n", __func__, s->idx, fpt_delta);
  meta->fpt = fpt_delta;
  meta->timestamp_last_pkt = mtl_ptp_read_time(rv_get_impl(s));
  meta->second_field = slot->second_field;
  if (ops->interlaced) {
    if (slot->second_field)
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
    else
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
  }
  meta->frame_total_size = s->st20_frame_size;
  meta->uframe_total_size = s->st20_uframe_size;
  meta->frame_recv_size = rv_slot_get_frame_size(slot);
  meta->pkts_total = slot->pkts_received;
  for (enum mtl_session_port s_port = MTL_SESSION_PORT_P; s_port < MTL_SESSION_PORT_MAX;
       s_port++) {
    meta->pkts_recv[s_port] = slot->pkts_recv_per_port[s_port];
  }
  meta->rtp_timestamp = slot->tmstamp;

  if (frame->user_meta_data_size) {
    meta->user_meta_size = frame->user_meta_data_size;
    meta->user_meta = frame->user_meta;
  } else {
    meta->user_meta_size = 0;
    meta->user_meta = NULL;
  }

  MT_USDT_ST20_RX_FRAME_AVAILABLE(s->parent->idx, s->idx, frame->idx, frame->addr,
                                  slot->tmstamp, meta->frame_recv_size);
  /* check if dump USDT enabled */
  if (MT_USDT_ST20_RX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ops->fps) * 5; /* dump every 5s now */
    if ((s->usdt_frame_cnt % period) == (period / 2)) {
      rv_usdt_dump_frame(s->impl, s, frame);
    }
    s->usdt_frame_cnt++;
  } else {
    s->usdt_frame_cnt = 0;
  }

  if (meta->frame_recv_size >= s->st20_frame_size) {
    meta->status = ST_FRAME_STATUS_COMPLETE;
    if (ops->num_port > 1) {
      if ((slot->pkts_recv_per_port[MTL_SESSION_PORT_P] < slot->pkts_received) &&
          (slot->pkts_recv_per_port[MTL_SESSION_PORT_R] < slot->pkts_received))
        meta->status = ST_FRAME_STATUS_RECONSTRUCTED;
    }
    rte_atomic32_inc(&s->stat_frames_received);

    if (slot->pkts_recv_per_port[MTL_SESSION_PORT_P] >= slot->pkts_received) {
      s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    } else {
      s->port_user_stats.common.port[MTL_SESSION_PORT_P].incomplete_frames++;
    }

    if (slot->pkts_recv_per_port[MTL_SESSION_PORT_R] >= slot->pkts_received) {
      s->port_user_stats.common.port[MTL_SESSION_PORT_R].frames++;
    } else {
      s->port_user_stats.common.port[MTL_SESSION_PORT_R].incomplete_frames++;
    }

    /* notify frame */
    dbg("%s(%d): tmstamp %ld\n", __func__, s->idx, slot->tmstamp);
    int ret = rv_notify_frame_ready(s, frame->addr, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready fail %d\n", __func__, s->idx, ret);
      rv_put_frame(s, frame);
      slot->frame = NULL;
    }

    /* update trs */
    double reactive = 1080.0 / 1125.0;
    s->trs = s->frame_time * reactive / meta->pkts_total;
  } else {
    dbg("%s(%d): frame_recv_size %" PRIu64 ", frame_total_size %" PRIu64
        ", tmstamp %ld\n",
        __func__, s->idx, meta->frame_recv_size, meta->frame_total_size, slot->tmstamp);
    MT_USDT_ST20_RX_FRAME_INCOMPLETE(s->parent->idx, s->idx, frame->idx, slot->tmstamp,
                                     meta->frame_recv_size, s->st20_frame_size);
    meta->status = ST_FRAME_STATUS_CORRUPTED;
    ST_SESSION_STAT_INC(s, port_user_stats, stat_frames_dropped);

    /* record the miss pkts */
    float pd_sz_per_pkt = (float)meta->frame_recv_size / slot->pkts_received;
    int miss_pkts = (s->st20_frame_size - meta->frame_recv_size) / pd_sz_per_pkt;
    dbg("%s(%d), miss pkts %d for current frame\n", __func__, s->idx, miss_pkts);
    ST_SESSION_STAT_ADD(s, port_user_stats, stat_frames_pks_missed, miss_pkts);

#if 0 /* for miss pkt detail */
    int total_pkts = s->st20_frame_size / pd_sz_per_pkt;
    dbg("%s(%d), total_pkts %d\n", __func__, s->idx, total_pkts);
    for (int i = 0; i < total_pkts; i++) {
      if (!mt_bitmap_test(slot->frame_bitmap, i))
        info("%s(%d): pkt %d miss for tmstamp %u\n", __func__, s->idx, i, slot->tmstamp);
    }
#endif

    rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
    s->port_user_stats.incomplete_frames_cnt++;
    /* notify the incomplete frame if user required */
    if (ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME) {
      rv_notify_frame_ready(s, frame->addr, meta);
    } else {
      rv_put_frame(s, frame);
      slot->frame = NULL;
    }
  }
}

static void rv_st22_frame_notify(struct st_rx_video_session_impl* s,
                                 struct st_rx_video_slot_impl* slot,
                                 enum st_frame_status status) {
  struct st20_rx_ops* ops = &s->ops;
  struct st22_rx_frame_meta* meta = &slot->st22_meta;
  struct st_frame_trans* frame = slot->frame;

  meta->second_field = slot->second_field;
  if (ops->interlaced) {
    if (slot->second_field)
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_second_field);
    else
      ST_SESSION_STAT_INC(s, port_user_stats, stat_interlace_first_field);
  }
  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->tmstamp;
  meta->rtp_timestamp = slot->tmstamp;
  meta->frame_total_size = rv_slot_get_frame_size(slot);
  meta->status = status;
  meta->pkts_total = slot->pkts_received;
  for (enum mtl_session_port s_port = MTL_SESSION_PORT_P; s_port < MTL_SESSION_PORT_MAX;
       s_port++) {
    meta->pkts_recv[s_port] = slot->pkts_recv_per_port[s_port];
  }

  MT_USDT_ST22_RX_FRAME_AVAILABLE(s->parent->idx, s->idx, frame->idx, frame->addr,
                                  slot->tmstamp, meta->frame_total_size);
  /* check if dump USDT enabled */
  if (MT_USDT_ST22_RX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ops->fps) * 5; /* dump every 5s now */
    if ((s->usdt_frame_cnt % period) == (period / 2)) {
      rv_st22_usdt_dump_frame(s->impl, s, frame, meta->frame_total_size);
    }
    s->usdt_frame_cnt++;
  } else {
    s->usdt_frame_cnt = 0;
  }

  /* notify frame */
  int ret = -EIO;

  if (st_is_frame_complete(status)) {
    s->port_user_stats.common.port[MTL_SESSION_PORT_P].frames++;
    ret = st22_notify_frame_ready(s, frame->addr, meta);
    if (ret < 0) {
      err("%s(%d), notify_frame_ready return fail %d\n", __func__, s->idx, ret);
      rv_put_frame(s, frame);
      slot->frame = NULL;
    }
    /* update trs */
    double reactive = 1080.0 / 1125.0;
    s->trs = s->frame_time * reactive / meta->pkts_total;
  } else {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_frames_dropped);
    /* record the miss pkts */
    float pd_sz_per_pkt = (float)s->st22_expect_size_per_frame / slot->pkts_received;
    int miss_pkts =
        (s->st22_expect_size_per_frame - meta->frame_total_size) / pd_sz_per_pkt;
    if (miss_pkts < 0) miss_pkts = 0;
    dbg("%s(%d), miss pkts %d for current frame\n", __func__, s->idx, miss_pkts);
    ST_SESSION_STAT_ADD(s, port_user_stats, stat_frames_pks_missed, miss_pkts);
#if 0 /* for miss pkt detail */
    int total_pkts = s->st22_expect_size_per_frame / pd_sz_per_pkt;
    dbg("%s(%d), total_pkts %d\n", __func__, s->idx, total_pkts);
    for (int i = 0; i < total_pkts; i++) {
      if (!mt_bitmap_test(slot->frame_bitmap, i))
        info("%s(%d): pkt %d miss for tmstamp %u\n", __func__, s->idx, i, slot->tmstamp);
    }
#endif

    rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
    /* notify the incomplete frame if user required */
    if (ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME) {
      st22_notify_frame_ready(s, frame->addr, meta);
    } else {
      rv_put_frame(s, frame);
      slot->frame = NULL;
    }
  }
  s->st22_expect_frame_size = 0;
}

static void rv_slice_notify(struct st_rx_video_session_impl* s,
                            struct st_rx_video_slot_impl* slot,
                            struct st_rx_video_slot_slice_info* slice_info) {
  struct st20_rx_ops* ops = &s->ops;
  struct st20_rx_slice_meta* meta = &s->slice_meta;

  /* w, h, fps, fmt, etc are fixed info */
  meta->timestamp = slot->tmstamp;
  meta->second_field = slot->second_field;
  meta->frame_recv_size = rv_slot_get_frame_size(slot);
  meta->frame_recv_lines = slice_info->ready_slices * s->slice_lines;
  ops->notify_slice_ready(ops->priv, slot->frame->addr, meta);
  ST_SESSION_STAT_INC(s, port_user_stats, stat_slices_received);
}

static void rv_slice_add(struct st_rx_video_session_impl* s,
                         struct st_rx_video_slot_impl* slot, uint32_t offset,
                         uint32_t size) {
  struct st_rx_video_slot_slice_info* slice_info = slot->slice_info;
  struct st_rx_video_slot_slice* main_slice = &slice_info->slices[0];
  uint32_t ready_slices = 0;
  struct st_rx_video_slot_slice* slice;

  /* main slice always start from 0(seq_id_base) */
  if (unlikely(offset != main_slice->size)) {
    /* check all slice and try to append */
    for (int i = 1; i < ST_VIDEO_RX_SLICE_NUM; i++) {
      slice = &slice_info->slices[i];
      if (!slice->size) { /* a null slice */
        slice->offset = offset;
        slice->size = size;
        slice_info->extra_slices++;
        dbg("%s(%d), slice(%u:%u) add to %d\n", __func__, s->idx, offset, size, i);
        return;
      }

      /* append to exist slice */
      if (offset == (slice->size + slice->offset)) {
        slice->size += size;
        return;
      }
    }

    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_slice_fail);
    return;
  }

  main_slice->size += size;
  if (unlikely(slice_info->extra_slices)) {
    /* try to merge the slice */
    bool merged;
  repeat_merge:
    merged = false;
    for (int i = 1; i < ST_VIDEO_RX_SLICE_NUM; i++) {
      slice = &slice_info->slices[i];
      if (slice->size && (slice->offset == main_slice->size)) {
        main_slice->size += slice->size;
        slice->size = 0;
        slice->offset = 0;
        merged = true;
        slice_info->extra_slices--;
        ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_slice_merged);
        dbg("%s(%d), slice %d(%u:%u) merge to main\n", __func__, s->idx, i, offset, size);
      }
    }
    if (merged) goto repeat_merge;
  }

  /* check ready slice */
  ready_slices = main_slice->size / s->slice_size;
  if (ready_slices > slice_info->ready_slices) {
    dbg("%s(%d), ready_slices %u\n", __func__, s->idx, ready_slices);
    slice_info->ready_slices = ready_slices;
    rv_slice_notify(s, slot, slice_info);
  }
}

static struct st_rx_video_slot_impl* rv_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp, void* hdr_split_pd,
    bool* exist_ts) {
  int i, slot_idx;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < s->slot_max; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) {
      *exist_ts = true;
      return slot;
    }
  }

  /* if the slot timestamp is in the past just drop it */
  bool timestamp_is_in_the_past = true;
  for (i = 0; i < s->slot_max; i++) {
    slot = &s->slots[i];

    if (slot->tmstamp == -1 || mt_seq32_greater(tmstamp, slot->tmstamp)) {
      timestamp_is_in_the_past = false;
      break;
    }
  }

  if (timestamp_is_in_the_past) {
    for (int i = 0; i < s->ops.num_port; i++) {
      if (s->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        dbg("%s(%d): tmstamp %u is in the past, drop it\n", __func__, s->idx, tmstamp);
        return NULL;
      }
    }

    /* should never happen */
    warn("%s(%d), redundant error threshold reached, accept packet tmstamp %d\n",
         __func__, s->idx, tmstamp);
  }

  dbg("%s(%d): new tmstamp %u\n", __func__, s->idx, tmstamp);

  if (s->dma_dev && !mt_dma_empty(s->dma_dev)) {
    /* still in progress of previous frame, drop current pkt */
    rte_atomic32_inc(&s->dma_previous_busy_cnt);
    dbg("%s(%d): still has dma inflight %u\n", __func__, s->idx,
        s->dma_dev->nb_borrowed[s->dma_dev].nb_borrowed);
    return NULL;
  }

  slot_idx = (s->slot_idx + 1) % s->slot_max;
  slot = &s->slots[slot_idx];
  // rv_slot_dump(s);

  /* drop frame if any previous */
  if (slot->frame) {
    if (s->st22_info)
      rv_st22_frame_notify(s, slot, ST_FRAME_STATUS_CORRUPTED);
    else
      rv_frame_notify(s, slot);
    slot->frame = NULL;
  }

  rv_slot_init_frame_size(slot);
  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  slot->pkts_received = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_P] = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_R] = 0;
  s->slot_idx = slot_idx;

  if (s->enable_timing_parser) {
    for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
      struct st_rv_tp_slot* tp_slot = &s->tp->slots[slot->idx][s_port];
      rv_tp_slot_init(tp_slot);
    }
  }

  struct st_frame_trans* frame_info = rv_get_frame(s);
  if (!frame_info) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_slot_get_frame_fail);
    if (s->st22_info)
      MT_USDT_ST22_RX_NO_FRAMEBUFFER(s->parent->idx, s->idx, tmstamp);
    else
      MT_USDT_ST20_RX_NO_FRAMEBUFFER(s->parent->idx, s->idx, tmstamp);
    dbg("%s(%d): slot %d get frame fail\n", __func__, s->idx, slot_idx);
    return NULL;
  }
  if (hdr_split_pd) { /* resolve base addr */
    frame_info->addr = hdr_split_pd;
  }
  if (rv_is_dynamic_ext_frame(s)) {
    struct st20_ext_frame ext_frame;
    struct st20_rx_ops* ops = &s->ops;
    struct st20_rx_frame_meta* meta = &slot->meta;
    size_t fb_size = s->st20_uframe_size ? s->st20_uframe_size : s->st20_fb_size;

    meta->width = ops->width;
    meta->height = ops->height;
    meta->fmt = ops->fmt;
    meta->fps = ops->fps;
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = slot->tmstamp;
    meta->frame_total_size = s->st20_frame_size;
    meta->uframe_total_size = s->st20_uframe_size;
    if (s->ops.query_ext_frame(s->ops.priv, &ext_frame, meta) < 0) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_slot_query_ext_fail);
      dbg("%s(%d): query ext frame fail\n", __func__, s->idx);
      rte_atomic32_dec(&frame_info->refcnt);
      return NULL;
    }
    if (ext_frame.buf_len < fb_size) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_slot_query_ext_fail);
      err("%s(%d): ext frame size too small, required %" PRIu64 " but get %" PRIu64 "\n",
          __func__, s->idx, fb_size, ext_frame.buf_len);
      rte_atomic32_dec(&frame_info->refcnt);
      return NULL;
    }
    frame_info->addr = ext_frame.buf_addr;
    frame_info->iova = ext_frame.buf_iova;
    frame_info->flags |= ST_FT_FLAG_EXT;
    meta->opaque = ext_frame.opaque;
  }
  frame_info->user_meta_data_size = 0;
  slot->frame = frame_info;
  slot->timestamp_first_pkt = mtl_ptp_read_time(rv_get_impl(s));

  s->dma_slot = slot;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);
  if (slot->slice_info) memset(slot->slice_info, 0x0, sizeof(*slot->slice_info));

  dbg("%s(%d): assign slot %d framebuff %p for tmstamp %u\n", __func__, s->idx, slot_idx,
      slot->frame->addr, tmstamp);
  return slot;
}

static struct st_rx_video_slot_impl* rv_rtp_slot_by_tmstamp(
    struct st_rx_video_session_impl* s, uint32_t tmstamp) {
  int i;
  int slot_idx = 0;
  struct st_rx_video_slot_impl* slot;

  for (i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    slot = &s->slots[i];

    if (tmstamp == slot->tmstamp) return slot;
  }

  /* replace the oldest slot*/
  slot_idx = (s->slot_idx + 1) % ST_VIDEO_RX_REC_NUM_OFO;
  slot = &s->slots[slot_idx];
  // rv_slot_dump(s);

  slot->tmstamp = tmstamp;
  slot->seq_id_got = false;
  s->slot_idx = slot_idx;

  /* clear bitmap */
  memset(slot->frame_bitmap, 0x0, s->st20_frame_bitmap_size);

  dbg("%s: assign slot %d for tmstamp %u\n", __func__, slot_idx, tmstamp);
  return slot;
}

static void rv_slot_full_frame(struct st_rx_video_session_impl* s,
                               struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rv_frame_notify(s, slot);
  rv_slot_init_frame_size(slot);
  slot->pkts_received = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_P] = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_R] = 0;
  slot->frame = NULL; /* frame pass to app */
}

static void rv_st22_slot_full_frame(struct st_rx_video_session_impl* s,
                                    struct st_rx_video_slot_impl* slot) {
  /* end of frame */
  rv_st22_frame_notify(s, slot, ST_FRAME_STATUS_COMPLETE);
  rv_slot_init_frame_size(slot);
  slot->pkts_received = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_P] = 0;
  slot->pkts_recv_per_port[MTL_SESSION_PORT_R] = 0;
  slot->frame = NULL; /* frame pass to app */
}

#if 0
static void rv_st22_slot_drop_frame(struct st_rx_video_session_impl* s,
                                    struct st_rx_video_slot_impl* slot) {
  rv_put_frame(s, slot->frame);
  slot->frame = NULL;
  s->stat_frames_dropped++;
  s->port_user_stats.stat_frames_dropped++;
  rte_atomic32_inc(&s->cbs_incomplete_frame_cnt);
   rv_slot_init_frame_size(slot);
  slot->pkts_received = 0;
  slot->pkts_redundant_received = 0;
}
#endif

static int rv_free_dma(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  if (s->dma_dev) {
    mt_dma_free_dev(impl, s->dma_dev);
    s->dma_dev = NULL;
  }

  return 0;
}

static int rv_slice_dma_drop_mbuf(void* priv, struct rte_mbuf* mbuf) {
  struct st_rx_video_session_impl* s = priv;
  rv_slice_add(s, s->dma_slot, st_rx_mbuf_get_offset(mbuf), st_rx_mbuf_get_len(mbuf));
  return 0;
}

static int rv_init_dma(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  enum st20_type type = s->ops.type;

  struct mt_dma_request_req req;
  req.nb_desc = s->dma_nb_desc;
  req.max_shared = MT_DMA_MAX_SESSIONS;
  req.sch_idx = s->parent->idx;
  req.socket_id = s->socket_id;
  req.priv = s;
  if (type == ST20_TYPE_SLICE_LEVEL)
    req.drop_mbuf_cb = rv_slice_dma_drop_mbuf;
  else
    req.drop_mbuf_cb = NULL;
  struct mtl_dma_lender_dev* dma_dev = mt_dma_request_dev(impl, &req);
  if (!dma_dev) {
    info("%s(%d), fail, can not request dma dev\n", __func__, idx);
    return -EIO;
  }

  s->dma_dev = dma_dev;

  info("%s(%d), succ, dma %d lender id %u\n", __func__, idx, mt_dma_dev_id(dma_dev),
       mt_dma_lender_id(dma_dev));
  return 0;
}

static int rv_stop_pcap(struct st_rx_video_session_impl* s,
                        enum mtl_session_port s_port) {
  struct mt_rx_pcap* pcap = &s->pcap[s_port];

  if (!pcap->pcap) return 0;

  info("%s(%d,%d), dumped %u packets to %s, dropped %u packets\n", __func__, s->idx,
       s_port, pcap->dumped_pkts, pcap->file_name, pcap->dropped_pkts);
  MT_USDT_ST20_RX_PCAP_DUMP(s->parent->idx, s->idx, s_port, pcap->file_name,
                            pcap->dumped_pkts);
  pcap->required_pkts = 0;
  mt_pcap_close(pcap->pcap);
  pcap->pcap = NULL;
  return 0;
}

static int rv_start_pcap(struct st_rx_video_session_impl* s, enum mtl_session_port s_port,
                         uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_rx_pcap* pcap = &s->pcap[s_port];

  if (pcap->pcap) {
    err("%s(%d,%d), pcap dump already started\n", __func__, idx, s_port);
    return -EIO;
  }
  snprintf(pcap->file_name, sizeof(pcap->file_name), "st22rx_s%dp%d_%u_XXXXXX.pcapng",
           idx, s_port, max_dump_packets);

  int fd = mt_mkstemps(pcap->file_name, strlen(".pcapng"));
  if (fd < 0) {
    err("%s(%d,%d), failed to create pcap file %s\n", __func__, idx, s_port,
        pcap->file_name);
    return -EIO;
  }
  pcap->pcap = mt_pcap_open(s->impl, port, fd);
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

  if (sync) {
    int time_out = 100; /* 100*100ms, 10s */
    int i = 0;
    for (; i < time_out; i++) {
      if (!pcap->pcap) break;
      mt_sleep_ms(100);
    }
    if (i >= time_out) {
      err("%s(%d,%d), timeout for sync, stop any way\n", __func__, idx, s_port);
      rv_stop_pcap(s, s_port);
      return -EIO;
    }
    if (meta) {
      meta->dumped_packets[s_port] = pcap->dumped_pkts;
      snprintf(meta->file_name[s_port], sizeof(meta->file_name[s_port]), "%s",
               pcap->file_name);
    }
  }

  return 0;
}

static int rv_start_pcap_dump(struct st_rx_video_session_impl* s,
                              uint32_t max_dump_packets, bool sync,
                              struct st_pcap_dump_meta* meta) {
  int ret;
  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    ret = rv_start_pcap(s, s_port, max_dump_packets, sync, meta);
    if (ret < 0) return ret;
  }
  return 0;
}

static int rv_stop_pcap_dump(struct st_rx_video_session_impl* s) {
  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    rv_stop_pcap(s, s_port);
  }
  return 0;
}

static int rv_dump_pcap(struct st_rx_video_session_impl* s, struct rte_mbuf** mbufs,
                        uint16_t nb, enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_rx_pcap* pcap = &s->pcap[s_port];
  uint16_t dump = mt_pcap_dump(s->impl, port, pcap->pcap, mbufs, nb);
  pcap->dumped_pkts += dump;
  pcap->dropped_pkts += nb - dump;
  return 0;
}

static int rv_dma_dequeue(struct st_rx_video_session_impl* s) {
  struct mtl_dma_lender_dev* dma_dev = s->dma_dev;

  uint16_t nb_dq = mt_dma_completed(dma_dev, s->rx_burst_size, NULL, NULL);

  if (nb_dq) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    mt_dma_drop_mbuf(dma_dev, nb_dq);
  }

  /* all dma action finished */
  struct st_rx_video_slot_impl* dma_slot = s->dma_slot;
  if (mt_dma_empty(dma_dev) && dma_slot) {
    dbg("%s(%d), nb_dq %u\n", __func__, s->idx, nb_dq);
    int32_t frame_recv_size = rv_slot_get_frame_size(dma_slot);
    if (frame_recv_size >= s->st20_frame_size) {
      dbg("%s(%d): full frame\n", __func__, s->idx);
      rv_slot_full_frame(s, dma_slot);
      s->dma_slot = NULL;
    }
  }

  return 0;
}

static inline uint32_t rfc4175_rtp_seq_id(struct st20_rfc4175_rtp_hdr* rtp) {
  uint16_t seq_id_base = ntohs(rtp->base.seq_number);
  uint16_t seq_id_ext = ntohs(rtp->seq_number_ext);
  uint32_t seq_id = (uint32_t)seq_id_base | (((uint32_t)seq_id_ext) << 16);
  return seq_id;
}

static inline void rv_tp_pkt_handle(struct st_rx_video_session_impl* s,
                                    struct rte_mbuf* mbuf, enum mtl_session_port s_port,
                                    struct st_rx_video_slot_impl* slot, uint32_t tmstamp,
                                    int pkt_idx) {
  struct st_rx_video_tp* tp = s->tp;
  if (s->cur_succ_burst_cnt > (tp->pass.cinst_max_narrow / 2)) {
    /* untrusted result */
    tp->stat_untrusted_pkts++;
    return;
  }
  if (s->in_continuous_burst[s_port]) {
    /* untrusted result */
    tp->stat_untrusted_pkts++;
    return;
  }
  struct mtl_main_impl* impl = rv_get_impl(s);
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);

  uint64_t pkt_ns = mt_mbuf_time_stamp(impl, mbuf, port);
  struct st_rv_tp_slot* tp_slot = &tp->slots[slot->idx][s_port];
  dbg("%s(%d,%d), tmstamp %u pkt_ns %" PRIu64 " pkt_idx %d\n", __func__, s->idx, s_port,
      tmstamp, pkt_ns, pkt_idx);
  rv_tp_on_packet(s, s_port, tp_slot, tmstamp, pkt_ns, pkt_idx);
}

static inline void* rv_frame_memcpy(void* dst, const void* src, size_t n) {
  /* not use rte_memcpy since it find performance issue on writing frame */
  return memcpy(dst, src, n);
}

static int rv_handle_frame_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                               enum mtl_session_port s_port, bool ctrl_thread) {
  struct st20_rx_ops* ops = &s->ops;
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t line1_number = ntohs(rtp->row_number); /* 0 to 1079 for 1080p */
  bool second_field = (line1_number & ST20_SECOND_FIELD) ? true : false;
  if (second_field) line1_number &= ~ST20_SECOND_FIELD;
  uint16_t line1_offset = ntohs(rtp->row_offset); /* [0, 480, 960, 1440] for 1080p */
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint16_t line1_length = ntohs(rtp->row_length); /* 1200 for 1080p */
  if (line1_length & ST20_RETRANSMIT) {
    line1_length &= ~ST20_RETRANSMIT;
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_retransmit);
  }
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint32_t seq_id_u32 = rfc4175_rtp_seq_id(rtp);
  uint8_t payload_type = rtp->base.payload_type;
  int pkt_idx = -1, ret;
  struct rte_mbuf* mbuf_next = mbuf->next;

  dbg("%s(%d,%d): line info %u %u %u\n", __func__, s->idx, s_port, line1_number,
      line1_offset, line1_length);

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->base.ssrc);
    dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
        ops->ssrc);
    if (ssrc != ops->ssrc) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }
  /* check interlace */
  if (!s->ops.interlaced) {
    if (second_field) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_interlace_dropped);
      return -EINVAL;
    }
  }
  if (mbuf_next && mbuf_next->data_len) {
    /* for some reason mbuf splits into 2 segments (1024 bytes + left bytes) */
    /* todo: payload needs to be copied from 2 places */
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_multi_segments_received);
    return -EIO;
  }

  /* find the target slot by tmstamp */
  bool exist_ts = false;
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, NULL, &exist_ts);
  /* Based on rv_slot_by_tmstamp - exist_ts is only true when slot is found */
  if (exist_ts && !slot->frame) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
    slot->pkts_recv_per_port[s_port]++;
    s->redundant_error_cnt[s_port]++;
    return 0;
  }
  s->redundant_error_cnt[s_port] = 0;

  if ((!slot || !slot->frame) && !exist_ts) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_no_slot);
    return -EIO;
  }

  /* special MTL extension to carry optional meta data between tx and rx, not standard of
   * ST2110 */
  if (line1_length & ST20_LEN_USER_META) {
    line1_length &= ~ST20_LEN_USER_META;
    dbg("%s(%d,%d): ST20_LEN_USER_META %u\n", __func__, s->idx, s_port, line1_length);
    if (line1_length <= slot->frame->user_meta_buffer_size) {
      rte_memcpy(slot->frame->user_meta, payload, line1_length);
      slot->frame->user_meta_data_size = line1_length;
    } else {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_user_meta_err);
      return -EIO;
    }
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_user_meta);
    return 0;
  }

  uint8_t* bitmap = slot->frame_bitmap;
  slot->second_field = second_field;

  /* calculate offset */
  uint32_t offset;
  offset = line1_number * (uint32_t)s->st20_linesize +
           line1_offset / s->st20_pg.coverage * s->st20_pg.size;
  size_t payload_length = line1_length;
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  if ((offset + payload_length) >
      s->st20_fb_size + s->st20_bytes_in_line - s->st20_linesize) {
    dbg("%s(%d,%d): invalid offset %u frame buffer size %" PRIu64 "\n", __func__, s->idx,
        s_port, offset, s->st20_fb_size);
    dbg("%s, number %u offset %u len %u\n", __func__, line1_number, line1_offset,
        line1_length);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_offset_dropped);
    return -EIO;
  }

  /* check if valid pkt len */
  size_t pkt_payload_len = mbuf->pkt_len - sizeof(struct st_rfc4175_video_hdr);
  if (extra_rtp) pkt_payload_len -= sizeof(*extra_rtp);
  if (pkt_payload_len != payload_length) {
    dbg("%s, invalid pkt_payload_len %" PRIu64 " payload_length %" PRIu64
        " retransmit %d\n",
        __func__, pkt_payload_len, payload_length,
        (ntohs(rtp->row_length) & ST20_RETRANSMIT) ? 1 : 0);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_len_dropped);
    return -EIO;
  }

  /* check if the same pkt got already */
  if (slot->seq_id_got) {
    if (seq_id_u32 >= slot->seq_id_base_u32)
      pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
    else
      pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base_u32);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_oo_bitmap);
      return -EIO;
    }

    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
      slot->pkts_recv_per_port[s_port]++;
      /* tp for the redundant packet */
      if (s->enable_timing_parser)
        rv_tp_pkt_handle(s, mbuf, s_port, slot, tmstamp, pkt_idx);
      return 0;
    }
    if (pkt_idx != (slot->last_pkt_idx + 1)) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
    }
  } else {
    /* the first pkt should always dispatch to control thread */
    if (ctrl_thread) {
      if (offset % payload_length) { /* GPM_SL packing */
        int bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc4175_video_hdr);
        int pkts_in_line = (s->st20_bytes_in_line / bytes_in_pkt) + 1;
        int pixel_in_pkt = (ops->width + pkts_in_line - 1) / pkts_in_line;
        pkt_idx = line1_number * pkts_in_line + line1_offset / pixel_in_pkt;
        dbg("%s(%d,%d), GPM_SL pkts_in_line %d pixel_in_pkt %d pkt_idx %d\n", __func__,
            s->idx, s_port, pkts_in_line, pixel_in_pkt, pkt_idx);
      } else {
        pkt_idx = offset / payload_length;
      }
      slot->seq_id_base_u32 = seq_id_u32 - pkt_idx;
      slot->seq_id_got = true;
      mt_bitmap_test_and_set(bitmap, pkt_idx);
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id_u32,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id not got, %u %u\n", __func__, s->idx,
          s_port, seq_id_u32, line1_number, line1_offset);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_dropped);
      return -EIO;
    }
  }
  slot->last_pkt_idx = pkt_idx;

  /* if enable_timing_parser */
  if (s->enable_timing_parser) rv_tp_pkt_handle(s, mbuf, s_port, slot, tmstamp, pkt_idx);

  bool dma_copy = false;
  bool need_copy = true;
  struct mtl_dma_lender_dev* dma_dev = s->dma_dev;

  if (s->st20_uframe_size) {
    /* user frame mode, pass to app to handle the payload */
    struct st20_rx_uframe_pg_meta* pg_meta = &s->pg_meta;
    pg_meta->payload = payload;
    pg_meta->row_length = line1_length;
    pg_meta->row_number = line1_number;
    pg_meta->row_offset = line1_offset;
    pg_meta->pg_cnt = line1_length / s->st20_pg.size;
    pg_meta->timestamp = tmstamp;
    ops->uframe_pg_callback(ops->priv, slot->frame->addr, pg_meta);
    if (extra_rtp) {
      pg_meta->payload = payload + line1_length;
      pg_meta->row_length = ntohs(extra_rtp->row_length);
      pg_meta->row_number = ntohs(extra_rtp->row_number);
      pg_meta->row_offset = ntohs(extra_rtp->row_offset);
      pg_meta->pg_cnt = pg_meta->row_length / s->st20_pg.size;
      ops->uframe_pg_callback(ops->priv, slot->frame->addr, pg_meta);
    }
  } else if (need_copy) {
    /* copy the payload to target frame by dma or cpu */
    if (extra_rtp && s->st20_linesize > s->st20_bytes_in_line) {
      /* packet crosses line padding, copy two lines data */
      rv_frame_memcpy(slot->frame->addr + offset, payload, line1_length);
      rv_frame_memcpy(slot->frame->addr + (line1_number + 1) * s->st20_linesize,
                      payload + line1_length, payload_length - line1_length);
    } else if (dma_dev && (payload_length > ST_RX_VIDEO_DMA_MIN_SIZE) &&
               !mt_dma_full(dma_dev) &&
               !rv_frame_payload_cross_page(s, slot->frame, offset, payload_length)) {
      rte_iova_t payload_iova =
          rte_pktmbuf_iova_offset(mbuf, sizeof(struct st_rfc4175_video_hdr));
      if (extra_rtp) payload_iova += sizeof(*extra_rtp);
      ret = mt_dma_copy(dma_dev, rv_frame_get_offset_iova(s, slot->frame, offset),
                        payload_iova, payload_length);
      if (ret < 0) {
        /* use cpu copy if dma copy fail */
        rv_frame_memcpy(slot->frame->addr + offset, payload, payload_length);
      } else {
        /* abstract dma dev takes ownership of this mbuf */
        st_rx_mbuf_set_offset(mbuf, offset);
        st_rx_mbuf_set_len(mbuf, payload_length);
        ret = mt_dma_borrow_mbuf(dma_dev, mbuf);
        if (ret)
          err("%s(%d,%d), mbuf copied but not enqueued \n", __func__, s->idx, s_port);
        dma_copy = true;
        ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_dma);
      }
    } else {
      rv_frame_memcpy(slot->frame->addr + offset, payload, payload_length);
    }
  }

  if (ctrl_thread) {
    rv_slot_pkt_lcore_add_frame_size(slot, payload_length);
  } else {
    rv_slot_add_frame_size(slot, payload_length);
  }
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  slot->pkts_received++;
  slot->pkts_recv_per_port[s_port]++;

  /* slice */
  if (slot->slice_info && !dma_copy) { /* ST20_TYPE_SLICE_LEVEL */
    rv_slice_add(s, slot, offset, payload_length);
  }

  /* check if frame is full */
  size_t frame_recv_size = rv_slot_get_frame_size(slot);
  bool end_frame = false;
  if (dma_dev) {
    if (frame_recv_size >= s->st20_frame_size && mt_dma_empty(dma_dev)) end_frame = true;
  } else {
    if (frame_recv_size >= s->st20_frame_size) end_frame = true;
  }
  if (end_frame) {
    dbg("%s(%d,%d): full frame on %p(%" PRIu64 ")\n", __func__, s->idx, s_port,
        slot->frame->addr, frame_recv_size);
    dbg("%s(%d,%d): tmstamp %ld slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    /* end of frame */
    rv_slot_full_frame(s, slot);
  }

  if (dma_copy) s->dma_copy = true;

  return 0;
}

static int rv_handle_rtp_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                             enum mtl_session_port s_port, bool ctrl_thread) {
  MTL_MAY_UNUSED(s_port);
  MTL_MAY_UNUSED(ctrl_thread);
  struct st20_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  uint16_t seq_id = ntohs(rtp->seq_number);
  uint32_t seq_id_u32 = rfc4175_rtp_seq_id((struct st20_rfc4175_rtp_hdr*)rtp);
  uint8_t payload_type = rtp->payload_type;
  int pkt_idx = -1;

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

  /* find the target slot by tmstamp */
  struct st_rx_video_slot_impl* slot = rv_rtp_slot_by_tmstamp(s, tmstamp);
  if (!slot || !slot->frame_bitmap) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_no_slot);
    return -ENOMEM;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  /* check if the same pks got already */
  if (slot->seq_id_got) {
    if (s->st22_handle) {
      if (seq_id >= slot->seq_id_base)
        pkt_idx = seq_id - slot->seq_id_base;
      else
        pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;
    } else {
      if (seq_id_u32 >= slot->seq_id_base_u32)
        pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
      else
        pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    }

    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_oo_bitmap);
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
      return 0;
    }
    if (pkt_idx != (slot->last_pkt_idx + 1)) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
    }
  } else {
    if (!slot->seq_id_got) { /* first packet */
      slot->seq_id_base = seq_id;
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      rte_atomic32_inc(&s->stat_frames_received);
      s->port_user_stats.common.port[s_port].frames++;
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id %d not got\n", __func__, s->idx,
          s_port, seq_id, slot->seq_id_base);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_dropped);
      return -EIO;
    }
  }
  slot->last_pkt_idx = pkt_idx;

  /* enqueue the packet ring to app */
  int ret = rte_ring_sp_enqueue(s->rtps_ring, (void*)mbuf);
  if (ret < 0) {
    dbg("%s(%d,%d), drop as rtps ring full, pkt_idx %d base %u\n", __func__, s->idx,
        s_port, pkt_idx, slot->seq_id_base);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_rtp_ring_full);
    return -EIO;
  }
  rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */

  ops->notify_rtp_ready(ops->priv);
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);

  return 0;
}

struct st22_box {
  uint32_t lbox; /* box lenght */
  char tbox[4];
};

/* Video Support Box and Color Specification Box */
static int rv_parse_st22_boxes(struct st_rx_video_session_impl* s, void* boxes,
                               struct st_rx_video_slot_impl* slot) {
  uint32_t jpvs_len = 0;
  uint32_t colr_len = 0;
  struct st22_box* box;

  box = boxes;
  if (0 == strncmp(box->tbox, "jpvs", 4)) {
    jpvs_len = ntohl(box->lbox);
    boxes += jpvs_len;
  }

  box = boxes;
  if (0 == strncmp(box->tbox, "colr", 4)) {
    colr_len = ntohl(box->lbox);
    boxes += colr_len;
  }

  if ((jpvs_len + colr_len) > 512) {
    err("%s(%d): err jpvs_len %u colr_len %u\n", __func__, s->idx, jpvs_len, colr_len);
    return -EIO;
  }

  slot->st22_box_hdr_length = jpvs_len + colr_len;
  dbg("%s(%d): st22_box_hdr_length %u\n", __func__, s->idx, slot->st22_box_hdr_length);

  if (slot->st22_box_hdr_length) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_st22_boxes);
  }

#if 0
  uint8_t* buf = boxes - slot->st22_box_hdr_length;
  for (uint16_t i = 0; i < slot->st22_box_hdr_length; i++) {
    info("0x%02x ", buf[i]);
  }
  info("end\n");
#endif
  return 0;
}

static int rv_handle_st22_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                              enum mtl_session_port s_port, bool ctrl_thread) {
  MTL_MAY_UNUSED(s_port);
  MTL_MAY_UNUSED(ctrl_thread);
  struct st20_rx_ops* ops = &s->ops;
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st22_rfc9134_video_hdr) - sizeof(struct st22_rfc9134_rtp_hdr);
  struct st22_rfc9134_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st22_rfc9134_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t payload_length = mbuf->data_len - sizeof(struct st22_rfc9134_video_hdr);
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint16_t seq_id = ntohs(rtp->base.seq_number);
  uint8_t payload_type = rtp->base.payload_type;
  uint16_t p_counter = (uint16_t)rtp->p_counter_lo + ((uint16_t)rtp->p_counter_hi << 8);
  uint16_t sep_counter =
      (uint16_t)rtp->sep_counter_lo + ((uint16_t)rtp->sep_counter_hi << 5);
  int pkt_counter = p_counter + sep_counter * 2048;
  int pkt_idx = -1;
  int ret;

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->base.ssrc);
    if (ssrc != ops->ssrc) {
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }

  if (rtp->kmode) { /* only pacKetization mode now */
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_kmod_dropped);
    return -EINVAL;
  }

  /* check interlace */
  if (s->ops.interlaced) {
    if (!(rtp->interlaced & 0x2)) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_interlace_dropped);
      return -EINVAL;
    }
  } else {
    if (rtp->interlaced) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_interlace_dropped);
      dbg("%s(%d,%d), rtp interlaced 0x%x set for progressive\n", __func__, s->idx,
          s_port, rtp->interlaced);
      return -EINVAL;
    }
  }

  /* find the target slot by tmstamp */
  bool exist_ts = false;
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, NULL, &exist_ts);
  /* Based on rv_slot_by_tmstamp - exist_ts is only true when slot is found */
  if (exist_ts && !slot->frame) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
    slot->pkts_recv_per_port[s_port]++;
    s->redundant_error_cnt[s_port]++;
    return 0;
  }
  s->redundant_error_cnt[s_port] = 0;

  if ((!slot || !slot->frame) && !exist_ts) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_no_slot);
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;

  slot->second_field = (rtp->interlaced == 0x3) ? true : false;

  dbg("%s(%d,%d), seq_id %d kmode %u trans_order %u\n", __func__, s->idx, s_port, seq_id,
      rtp->kmode, rtp->trans_order);
  dbg("%s(%d,%d), seq_id %d p_counter %u sep_counter %u\n", __func__, s->idx, s_port,
      seq_id, p_counter, sep_counter);

  if (slot->seq_id_got) {
    if (!rtp->base.marker && (payload_length != slot->st22_payload_length)) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_len_dropped);
      return -EIO;
    }
    /* check if the same pks got already */
    if (seq_id >= slot->seq_id_base)
      pkt_idx = seq_id - slot->seq_id_base;
    else
      pkt_idx = seq_id + (0xFFFF - slot->seq_id_base) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_oo_bitmap);
      return -EIO;
    }

    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
      slot->pkts_recv_per_port[s_port]++;
      return 0;
    }
    if (pkt_idx != (slot->last_pkt_idx + 1)) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
    }
  } else {
    /* first packet */
    if (!pkt_counter) { /* first packet */
      if (s->st22_ops_flags & ST22_RX_FLAG_DISABLE_BOXES) {
        slot->st22_box_hdr_length = 0;
      } else {
        ret = rv_parse_st22_boxes(s, payload, slot);
        if (ret < 0) {
          ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_dropped);
          return -EIO;
        }
      }
    }
    pkt_idx = pkt_counter;
    slot->seq_id_base = seq_id - pkt_idx;
    slot->st22_payload_length = payload_length;
    slot->seq_id_got = true;
    mt_bitmap_test_and_set(bitmap, pkt_idx);
    dbg("%s(%d,%d), get seq_id %d tmstamp %u, p_counter %u sep_counter %u, "
        "payload_length %u\n",
        __func__, s->idx, s_port, seq_id, tmstamp, p_counter, sep_counter,
        payload_length);
  }
  slot->last_pkt_idx = pkt_idx;

  /* copy payload */
  uint32_t offset;
  if (!pkt_counter) { /* first pkt */
    offset = 0;
    payload += slot->st22_box_hdr_length;
    payload_length -= slot->st22_box_hdr_length;
  } else {
    offset = pkt_counter * slot->st22_payload_length - slot->st22_box_hdr_length;
  }
  if ((offset + payload_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_offset_dropped);
    return -EIO;
  }
  rv_frame_memcpy(slot->frame->addr + offset, payload, payload_length);
  rv_slot_add_frame_size(slot, payload_length);
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  slot->pkts_received++;
  slot->pkts_recv_per_port[s_port]++;

  /* update the expect frame size */
  if (rtp->base.marker) {
    s->st22_expect_frame_size = offset + payload_length;
    s->st22_expect_size_per_frame = s->st22_expect_frame_size;
  }

  /* check if frame is full */
  if (s->st22_expect_frame_size) {
    size_t rece_frame_size = rv_slot_get_frame_size(slot);

    dbg("%s(%d,%d): marker got, frame size %" PRIu64 " %" PRIu64 ", tmstamp %u\n",
        __func__, s->idx, s_port, rece_frame_size, s->st22_expect_frame_size, tmstamp);
    if (s->st22_expect_frame_size == rece_frame_size) {
      rv_st22_slot_full_frame(s, slot);
    } else {
      dbg("%s(%d,%d): still has %" PRIu64
          " bytes unarrived pkt before marker, tmstamp %u\n",
          __func__, s->idx, s_port, s->st22_expect_frame_size - rece_frame_size, tmstamp);
    }
  }

  return 0;
}

static int rv_handle_hdr_split_pkt(struct st_rx_video_session_impl* s,
                                   struct rte_mbuf* mbuf, enum mtl_session_port s_port,
                                   bool ctrl_thread) {
  MTL_MAY_UNUSED(ctrl_thread);
  struct st20_rx_ops* ops = &s->ops;
  // size_t hdr_offset = mbuf->l2_len + mbuf->l3_len + mbuf->l4_len;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t line1_number = ntohs(rtp->row_number); /* 0 to 1079 for 1080p */
  uint16_t line1_offset = ntohs(rtp->row_offset); /* [0, 480, 960, 1440] for 1080p */
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint16_t line1_length = ntohs(rtp->row_length); /* 1200 for 1080p */
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint32_t seq_id_u32 = rfc4175_rtp_seq_id(rtp);
  uint8_t payload_type = rtp->base.payload_type;
  int pkt_idx = -1;
  struct st_rx_video_hdr_split_info* hdr_split = &s->hdr_split_info[s_port];
  struct rte_mbuf* mbuf_next = mbuf->next;

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->base.ssrc);
    if (ssrc != ops->ssrc) {
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }
  if (!hdr_split->mbuf_pool_ready) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_no_slot);
    return -EINVAL;
  }

  if (mbuf_next && mbuf_next->data_len) {
    payload = rte_pktmbuf_mtod(mbuf_next, void*);
  }

  /* find the target slot by tmstamp */
  bool exist_ts = false;
  struct st_rx_video_slot_impl* slot = rv_slot_by_tmstamp(s, tmstamp, payload, &exist_ts);
  /* Based on rv_slot_by_tmstamp - exist_ts is only true when slot is found */
  if (exist_ts && !slot->frame) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
    s->redundant_error_cnt[s_port]++;
    slot->pkts_recv_per_port[s_port]++;
    return 0;
  }
  s->redundant_error_cnt[s_port] = 0;

  if ((!slot || !slot->frame) && !exist_ts) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_no_slot);
    return -EIO;
  }
  uint8_t* bitmap = slot->frame_bitmap;
  slot->second_field = (line1_number & ST20_SECOND_FIELD) ? true : false;
  line1_number &= ~ST20_SECOND_FIELD;

  /* check if the same pkt got already */
  if (slot->seq_id_got) {
    if (seq_id_u32 >= slot->seq_id_base_u32)
      pkt_idx = seq_id_u32 - slot->seq_id_base_u32;
    else
      pkt_idx = seq_id_u32 + (0xFFFFFFFF - slot->seq_id_base_u32) + 1;
    if ((pkt_idx < 0) || (pkt_idx >= (s->st20_frame_bitmap_size * 8))) {
      dbg("%s(%d,%d), drop as invalid pkt_idx %d base %u\n", __func__, s->idx, s_port,
          pkt_idx, slot->seq_id_base_u32);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_oo_bitmap);
      return -EIO;
    }
    bool is_set = mt_bitmap_test_and_set(bitmap, pkt_idx);
    if (is_set) {
      dbg("%s(%d,%d), drop as pkt %d already received\n", __func__, s->idx, s_port,
          pkt_idx);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_redundant_dropped);
      slot->pkts_recv_per_port[s_port]++;
      return 0;
    }
    if (pkt_idx != (slot->last_pkt_idx + 1)) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_out_of_order);
    }
  } else {
    if (!line1_number && !line1_offset) { /* first packet */
      slot->seq_id_base_u32 = seq_id_u32;
      slot->seq_id_got = true;
      mt_bitmap_test_and_set(bitmap, 0);
      pkt_idx = 0;
      dbg("%s(%d,%d), seq_id_base %d tmstamp %u\n", __func__, s->idx, s_port, seq_id_u32,
          tmstamp);
    } else {
      dbg("%s(%d,%d), drop seq_id %d as base seq id not got, %u %u\n", __func__, s->idx,
          s_port, seq_id_u32, line1_number, line1_offset);
      ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_idx_dropped);
      return -EIO;
    }
  }
  slot->last_pkt_idx = pkt_idx;

  /* calculate offset */
  uint32_t offset =
      (line1_number * ops->width + line1_offset) / s->st20_pg.coverage * s->st20_pg.size;
  size_t payload_length = line1_length;
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  if ((offset + payload_length) > s->st20_frame_size) {
    dbg("%s(%d,%d): invalid offset %u frame size %" PRIu64 "\n", __func__, s->idx, s_port,
        offset, s->st20_frame_size);
    dbg("%s, number %u offset %u len %u\n", __func__, line1_number, line1_offset,
        line1_length);
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_offset_dropped);
    return -EIO;
  }

  uint8_t marker = rtp->base.marker;
  if ((payload_length != ST_VIDEO_BPM_SIZE) && !marker) {
    ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_not_bpm);
    return -EIO;
  }

  bool need_copy = false;

  if (!pkt_idx) {
    hdr_split->cur_frame_addr = payload;
    /* Cut RTE_PKTMBUF_HEADROOM since rte_mbuf_data_iova_default has offset */
    hdr_split->cur_frame_mbuf_idx =
        (payload - RTE_PKTMBUF_HEADROOM - hdr_split->frames) / ST_VIDEO_BPM_SIZE;
    dbg("%s(%d,%d), cur_frame_addr %p cur_frame_idx %u\n", __func__, s->idx, s_port,
        hdr_split->cur_frame_addr, hdr_split->cur_frame_mbuf_idx);
    if (hdr_split->cur_frame_mbuf_idx % hdr_split->mbufs_per_frame) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_mismatch_hdr_split_frame);
      dbg("%s(%d,%d), cur_frame_addr %p cur_frame_idx %u mbufs_per_frame %u\n", __func__,
          s->idx, s_port, hdr_split->cur_frame_addr, hdr_split->cur_frame_mbuf_idx,
          hdr_split->mbufs_per_frame);
    }
  } else {
    void* expect_payload = hdr_split->cur_frame_addr + pkt_idx * ST_VIDEO_BPM_SIZE;
    if (expect_payload != payload) {
      dbg("%s(%d,%d), payload mismatch %p:%p on pkt %d\n", __func__, s->idx, s_port,
          payload, expect_payload, pkt_idx);
      /* may caused by ooo, imiss, the last pkt(ddp not split for unknow cause) */
      if (marker && (expect_payload < (hdr_split->frames + hdr_split->frames_size))) {
        need_copy = true;
        ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_copy_hdr_split);
      } else { /* no way to recover since nic is in writing dram */
        ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_wrong_payload_hdr_split);
        return -EIO;
      }
    }
  }

  if (need_copy) {
    rv_frame_memcpy(slot->frame->addr + offset, payload, payload_length);
  }

  rv_slot_add_frame_size(slot, payload_length);
  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  slot->pkts_received++;
  slot->pkts_recv_per_port[s_port]++;

  /* slice */
  if (slot->slice_info) {
    rv_slice_add(s, slot, offset, payload_length);
  }

  /* check if frame is full */
  size_t frame_recv_size = rv_slot_get_frame_size(slot);
  if (frame_recv_size >= s->st20_frame_size) {
    dbg("%s(%d,%d): full frame on %p(%zu)\n", __func__, s->idx, s_port, slot->frame->addr,
        frame_recv_size);
    dbg("%s(%d,%d): tmstamp %ld slot %d\n", __func__, s->idx, s_port, slot->tmstamp,
        slot->idx);
    rv_slot_full_frame(s, slot);
  }

  return 0;
}

static int rv_uinit_pkt_lcore(struct mtl_main_impl* impl,
                              struct st_rx_video_session_impl* s) {
  int idx = s->idx;

  if (rte_atomic32_read(&s->pkt_lcore_active)) {
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    info("%s(%d), stop lcore\n", __func__, idx);
    while (rte_atomic32_read(&s->pkt_lcore_stopped) == 0) {
      mt_sleep_ms(10);
    }
  }

  if (s->has_pkt_lcore) {
    rte_eal_wait_lcore(s->pkt_lcore);
    mt_sch_put_lcore(impl, s->pkt_lcore);
    s->has_pkt_lcore = false;
  }

  if (s->pkt_lcore_ring) {
    mt_ring_dequeue_clean(s->pkt_lcore_ring);
    rte_ring_free(s->pkt_lcore_ring);
    s->pkt_lcore_ring = NULL;
  }

  return 0;
}

static int rv_pkt_lcore_func(void* args) {
  struct st_rx_video_session_impl* s = args;
  int idx = s->idx, ret;
  struct rte_mbuf* pkt = NULL;

  info("%s(%d), start\n", __func__, idx);
  while (rte_atomic32_read(&s->pkt_lcore_active)) {
    ret = rte_ring_sc_dequeue(s->pkt_lcore_ring, (void**)&pkt);
    if (ret >= 0) {
      rv_handle_frame_pkt(s, pkt, MTL_SESSION_PORT_P, true);
      rte_pktmbuf_free(pkt);
    }
  }

  rte_atomic32_set(&s->pkt_lcore_stopped, 1);
  info("%s(%d), end\n", __func__, idx);
  return 0;
}

static int rv_init_pkt_lcore(struct mtl_main_impl* impl,
                             struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count, lcore;
  int mgr_idx = mgr->idx, idx = s->idx, ret;

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_RX_VIDEO_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->rx_burst_size;
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
  if (!ring) {
    err("%s(%d,%d), ring create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->pkt_lcore_ring = ring;

  ret = mt_sch_get_lcore(impl, &lcore, MT_LCORE_TYPE_RXV_RING_LCORE, s->socket_id);
  if (ret < 0) {
    err("%s(%d,%d), get lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rv_uinit_pkt_lcore(impl, s);
    return ret;
  }
  s->pkt_lcore = lcore;
  s->has_pkt_lcore = true;

  rte_atomic32_set(&s->pkt_lcore_active, 1);
  ret = rte_eal_remote_launch(rv_pkt_lcore_func, s, lcore);
  if (ret < 0) {
    err("%s(%d,%d), launch lcore fail %d\n", __func__, mgr_idx, idx, ret);
    rte_atomic32_set(&s->pkt_lcore_active, 0);
    rv_uinit_pkt_lcore(impl, s);
    return ret;
  }

  return 0;
}

static int rv_init_st22(struct st_rx_video_session_impl* s,
                        struct st22_rx_ops* st22_frame_ops) {
  struct st22_rx_video_info* st22_info;

  st22_info = mt_rte_zmalloc_socket(sizeof(*st22_info), s->socket_id);
  if (!st22_info) return -ENOMEM;

  st22_info->notify_frame_ready = st22_frame_ops->notify_frame_ready;

  st22_info->meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;

  s->st22_info = st22_info;

  return 0;
}

static int rv_uinit_st22(struct st_rx_video_session_impl* s) {
  if (s->st22_info) {
    mt_rte_free(s->st22_info);
    s->st22_info = NULL;
  }

  return 0;
}

static int rv_uinit_sw(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  rv_tp_uinit(s);
  rv_uinit_pkt_lcore(impl, s);
  rv_free_dma(impl, s);
  rv_uinit_slot(s);
  rv_free_frames(s);
  rv_free_rtps(s);
  rv_uinit_st22(s);
  return 0;
}

static int rv_init_sw(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                      struct st_rx_video_session_impl* s, struct st22_rx_ops* st22_ops) {
  struct st20_rx_ops* ops = &s->ops;
  enum st20_type type = ops->type;
  int idx = s->idx;
  int ret;

  /* try to request dma dev */
  if (st20_is_frame_type(type) && (ops->flags & ST20_RX_FLAG_DMA_OFFLOAD) &&
      !s->st20_uframe_size && !rv_is_hdr_split(s)) {
    rv_init_dma(impl, s);
  }

  if (st22_ops) {
    ret = rv_init_st22(s, st22_ops);
    if (ret < 0) {
      err("%s(%d), st22 frame init fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  if (st20_is_frame_type(type)) {
    ret = rv_alloc_frames(impl, s);
  } else if (type == ST20_TYPE_RTP_LEVEL) {
    ret = rv_alloc_rtps(mgr, s);
  } else {
    err("%s(%d), error type %d\n", __func__, idx, type);
    return -EIO;
  }
  if (ret < 0) {
    rv_uinit_sw(impl, s);
    return ret;
  }

  ret = rv_init_slot(s);
  if (ret < 0) {
    rv_uinit_sw(impl, s);
    return ret;
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    struct st20_rx_slice_meta* slice_meta = &s->slice_meta;
    slice_meta->width = ops->width;
    slice_meta->height = ops->height;
    slice_meta->fmt = ops->fmt;
    slice_meta->fps = ops->fps;
    slice_meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    slice_meta->frame_total_size = s->st20_frame_size;
    slice_meta->uframe_total_size = s->st20_uframe_size;
    slice_meta->second_field = false;
    info("%s(%d), slice lines %u\n", __func__, idx, s->slice_lines);
  }

  if (s->st20_uframe_size) {
    /* user frame mode */
    struct st20_rx_uframe_pg_meta* pg_meta = &s->pg_meta;
    pg_meta->width = ops->width;
    pg_meta->height = ops->height;
    pg_meta->fmt = ops->fmt;
    pg_meta->fps = ops->fps;
    pg_meta->frame_total_size = s->st20_frame_size;
    pg_meta->uframe_total_size = s->st20_uframe_size;
    info("%s(%d), uframe size %" PRIu64 "\n", __func__, idx, s->st20_uframe_size);
  }

  s->has_pkt_lcore = false;
  rte_atomic32_set(&s->pkt_lcore_stopped, 0);
  rte_atomic32_set(&s->pkt_lcore_active, 0);

  uint64_t bps;
  bool pkt_handle_lcore = false;
  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps,
                               ops->interlaced, &bps);
  if (ret < 0) {
    err("%s(%d), get bps fail %d\n", __func__, idx, ret);
    rv_uinit_sw(impl, s);
    return ret;
  }
  if (st20_is_frame_type(type)) {
    /* for traffic > 40g, two lcore used  */
    if ((bps / (1000 * 1000)) > (40 * 1000)) {
      if (!s->dma_dev) pkt_handle_lcore = true;
    }

    if (ops->flags & ST20_RX_FLAG_USE_MULTI_THREADS) {
      info("%s(%d), user enable ST20_RX_FLAG_USE_MULTI_THREADS\n", __func__, idx);
      pkt_handle_lcore = true;
    }
  }

  /* only one core for hdr split mode */
  if (rv_is_hdr_split(s)) pkt_handle_lcore = false;

  if (pkt_handle_lcore) {
    if (type == ST20_TYPE_SLICE_LEVEL) {
      err("%s(%d), additional pkt lcore not support slice type\n", __func__, idx);
      rv_uinit_sw(impl, s);
      return -EINVAL;
    }
    if (ops->num_port > 1) {
      err("%s(%d), additional pkt lcore not support redundant, num_port %u\n", __func__,
          idx, ops->num_port);
      rv_uinit_sw(impl, s);
      return -EINVAL;
    }
    ret = rv_init_pkt_lcore(impl, mgr, s);
    if (ret < 0) {
      err("%s(%d), init_pkt_lcore fail %d\n", __func__, idx, ret);
      rv_uinit_sw(impl, s);
      return ret;
    }
    /* enable multi slot as it has two threads running */
    s->slot_max = ST_VIDEO_RX_REC_NUM_OFO;
  }

  if (s->enable_timing_parser) {
    ret = rv_tp_init(impl, s);
    if (ret < 0) {
      err("%s(%d), ebu init fail %d\n", __func__, idx, ret);
      rv_uinit_sw(impl, s);
      return ret;
    }
  }

  /* init vsync */
  struct st_fps_timing fps_tm;
  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    rv_uinit_sw(impl, s);
    return ret;
  }
  s->vsync.meta.frame_time = (double)1000000000.0 * fps_tm.den / fps_tm.mul;
  st_vsync_calculate(impl, &s->vsync);
  s->vsync.init = true;

  /* init advice sleep us */
  double sleep_ns = s->trs * 128;
  s->advice_sleep_us = sleep_ns / NS_PER_US;
  if (mt_user_tasklet_sleep(impl)) {
    info("%s(%d), advice sleep us %" PRIu64 "\n", __func__, idx, s->advice_sleep_us);
  }

  return 0;
}

static int rv_handle_detect_err(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                                enum mtl_session_port s_port, bool ctrl_thread) {
  MTL_MAY_UNUSED(ctrl_thread);
  MTL_MAY_UNUSED(mbuf);
  err_once("%s(%d,%d), detect fail, please choose the right format\n", __func__, s->idx,
           s_port);
  return 0;
}

static int rv_detect_change_status(struct st_rx_video_session_impl* s,
                                   enum st20_detect_status new_status) {
  if (s->detector.status == new_status) return 0;

  s->detector.status = new_status;
  rv_init_pkt_handler(s);
  return 0;
}

static int rv_handle_detect_pkt(struct st_rx_video_session_impl* s, struct rte_mbuf* mbuf,
                                enum mtl_session_port s_port, bool ctrl_thread) {
  int ret;
  struct st20_rx_ops* ops = &s->ops;
  struct st_rx_video_detector* detector = &s->detector;
  struct st20_detect_meta* meta = &detector->meta;
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st20_rfc4175_rtp_hdr*, hdr_offset);
  void* payload = &rtp[1];
  uint16_t line1_number = ntohs(rtp->row_number);
  uint16_t line1_offset = ntohs(rtp->row_offset);
  /* detect field bit */
  if (line1_number & ST20_SECOND_FIELD) meta->interlaced = true;
  line1_number &= ~ST20_SECOND_FIELD;
  struct st20_rfc4175_extra_rtp_hdr* extra_rtp = NULL;
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    line1_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    extra_rtp = payload;
    payload += sizeof(*extra_rtp);
  }
  uint32_t payload_length = ntohs(rtp->row_length);
  if (extra_rtp) payload_length += ntohs(extra_rtp->row_length);
  uint32_t tmstamp = ntohl(rtp->base.tmstamp);
  uint8_t payload_type = rtp->base.payload_type;
  MTL_MAY_UNUSED(ctrl_thread);

  /* only detect on the main port */
  if (MTL_SESSION_PORT_P != s_port) return 0;

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    dbg("%s, payload_type mismatch %d %d\n", __func__, payload_type, ops->payload_type);
    ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_pt_dropped);
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->base.ssrc);
    if (ssrc != ops->ssrc) {
      ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_wrong_ssrc_dropped);
      return -EINVAL;
    }
  }

  /* detect continuous bit */
  if (extra_rtp) detector->single_line = false;
  /* detect bpm */
  if (payload_length % 180 != 0) detector->bpm = false;
  /* on frame/field marker bit */
  if (rtp->base.marker) {
    if (detector->frame_num < 3) {
      detector->rtp_tm[detector->frame_num] = tmstamp;
      detector->pkt_num[detector->frame_num] = s->stat_pkts_received;
      detector->frame_num++;
    } else {
      rv_detector_calculate_dimension(s, detector, line1_number);
      rv_detector_calculate_fps(s, detector);
      rv_detector_calculate_n_packet(s, detector);
      rv_detector_calculate_packing(detector);
      detector->frame_num = 0;
    }
    if (meta->fps != ST_FPS_MAX && meta->packing != ST20_PACKING_MAX) {
      if (!meta->height) {
        rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
        err("%s(%d,%d): st20 failed to detect dimension, max_line: %d\n", __func__,
            s->idx, s_port, line1_number);
      } else { /* detected */
        ops->width = meta->width;
        ops->height = meta->height;
        ops->fps = meta->fps;
        ops->packing = meta->packing;
        ops->interlaced = meta->interlaced;
        if (ops->notify_detected && (ops->flags & ST20_RX_FLAG_AUTO_DETECT)) {
          struct st20_detect_reply reply = {0};
          ret = ops->notify_detected(ops->priv, meta, &reply);
          if (ret < 0) {
            err("%s(%d), notify_detected return fail %d\n", __func__, s->idx, ret);
            rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
            return ret;
          }
          s->slice_lines = reply.slice_lines;
          s->st20_uframe_size = reply.uframe_size;
          info("%s(%d), detected, slice_lines %u, uframe_size %" PRIu64 "\n", __func__,
               s->idx, s->slice_lines, s->st20_uframe_size);
        }
        if (!s->slice_lines) s->slice_lines = ops->height / 32;
        s->slice_size =
            ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
        s->st20_frames_cnt = ops->framebuff_cnt;
        s->st20_frame_size =
            ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
        if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
        /* Calculate bytes per line, rounding up if there's a remainder */
        size_t raw_bytes_size = (size_t)ops->width * s->st20_pg.size;
        s->st20_bytes_in_line =
            (raw_bytes_size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
        s->st20_linesize = s->st20_bytes_in_line;
        if (ops->linesize > s->st20_linesize)
          s->st20_linesize = ops->linesize;
        else if (ops->linesize) {
          err("%s(%d), invalid linesize %u\n", __func__, s->idx, ops->linesize);
          return -EINVAL;
        }
        s->st20_fb_size = s->st20_linesize * ops->height;
        if (ops->interlaced) s->st20_fb_size = s->st20_fb_size >> 1;
        /* at least 1000 byte for each packet */
        s->st20_frame_bitmap_size = s->st20_frame_size / 1000 / 8;
        /* one line at line 2 packets for all the format */
        if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
          s->st20_frame_bitmap_size = ops->height * 2 / 8;
        ret = rv_init_sw(rv_get_impl(s), s->parent, s, NULL);
        if (ret < 0) {
          err("%s(%d), rv_init_sw fail %d\n", __func__, s->idx, ret);
          rv_detect_change_status(s, ST20_DETECT_STAT_FAIL);
          return ret;
        }
        rvs_mgr_update(s->parent); /* update mgr since we has new advice sleep us */
        rv_detect_change_status(s, ST20_DETECT_STAT_SUCCESS);
        info("st20 detected(%d,%d): width: %d, height: %d, fps: %f\n", s->idx, s_port,
             meta->width, meta->height, st_frame_rate(meta->fps));
        info("st20 detected(%d,%d): packing: %d, field: %s, pkts per %s: %d\n", s->idx,
             s_port, meta->packing, meta->interlaced ? "interlaced" : "progressive",
             meta->interlaced ? "field" : "frame", detector->pkt_per_frame);
      }
    }
  }

  ST_SESSION_STAT_INC(s, port_user_stats.common, stat_pkts_received);
  return 0;
}

static bool rv_simulate_pkt_loss(struct st_rx_video_session_impl* s) {
  if (s->burst_loss_cnt == 0) {
    /* create a burst of pkt loss at fixed rate */
    if (((float)rand() / (float)RAND_MAX) < s->sim_loss_rate) {
      /* burst_loss_cnt at least 1 to prevent underflow */
      s->burst_loss_cnt = rand() % s->burst_loss_max + 1;
    } else {
      return false;
    }
  }
  /* continue drop pkt in current burst */
  s->burst_loss_cnt--;
  dbg("%s(%d,%d), drop as simulate pkt loss\n", __func__, s->idx,
      s->stat_pkts_simulate_loss);
  ST_SESSION_STAT_INC(s, port_user_stats, stat_pkts_simulate_loss);
  return true;
}

static int rv_handle_mbuf(void* priv, struct rte_mbuf** mbuf, uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_video_session_impl* s = s_priv->session;
  enum mtl_session_port s_port = s_priv->s_port;

  if (!s->attached) {
    dbg("%s(%d,%d), session not ready\n", __func__, s->idx, s_port);
    return -EIO;
  }

  struct rte_ring* pkt_ring = s->pkt_lcore_ring;
  bool ctl_thread = pkt_ring ? false : true;
  int ret = 0;

  struct mt_rx_pcap* pcap = &s->pcap[s_port];
  if (pcap->required_pkts) {
    if (pcap->dumped_pkts < pcap->required_pkts) {
      rv_dump_pcap(s, mbuf, RTE_MIN(nb, pcap->required_pkts - pcap->dumped_pkts), s_port);
    } else { /* got enough packets, stop dumping */
      rv_stop_pcap(s, s_port);
    }
  }

  if (pkt_ring) {
    /* first pass to the pkt ring if it has pkt handling lcore */
    unsigned int n =
        rte_ring_sp_enqueue_bulk(s->pkt_lcore_ring, (void**)&mbuf[0], nb, NULL);
    for (uint16_t i = 0; i < (uint16_t)n; i++) rte_mbuf_refcnt_update(mbuf[i], 1);
    nb -= n; /* n is zero or nb */
    ST_SESSION_STAT_ADD(s, port_user_stats, stat_pkts_enqueue_fallback, nb);
  }
  if (!nb) return 0;

  /* now dispatch the pkts to handler */
  for (uint16_t i = 0; i < nb; i++) {
    if ((s->ops.flags & ST20_RX_FLAG_SIMULATE_PKT_LOSS) && rv_simulate_pkt_loss(s))
      continue;
    if (s->rtcp_rx[s_port]) {
      struct st_rfc3550_rtp_hdr* rtp = rte_pktmbuf_mtod_offset(
          mbuf[i], struct st_rfc3550_rtp_hdr*, sizeof(struct mt_udp_hdr));
      mt_rtcp_rx_parse_rtp_packet(s->rtcp_rx[s_port], rtp);
    }
    int handler_ret = s->pkt_handler(s, mbuf[i], s_port, ctl_thread);
    if (handler_ret < 0) {
      s->port_user_stats.common.port[s_port].err_packets++;
    } else {
      s->stat_bytes_received += mbuf[i]->pkt_len;
      s->port_user_stats.stat_bytes_received += mbuf[i]->pkt_len;
      s->port_user_stats.common.port[s_port].packets++;
      s->port_user_stats.common.port[s_port].bytes += mbuf[i]->pkt_len;
    }
    ret += handler_ret;
  }
  return ret;
}

static int rv_pkt_rx_tasklet(struct st_rx_video_session_impl* s) {
  struct rte_mbuf* mbuf[s->rx_burst_size];
  uint16_t rv;
  int num_port = s->ops.num_port;

  int check_err_status;

  bool done = true;

  if (s->dma_dev) {
    rv_dma_dequeue(s);
    /* check if has pending pkts in dma */
    if (!mt_dma_empty(s->dma_dev)) done = false;
  }
  s->dma_copy = false;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->rxq[s_port]) continue;

    struct mt_rx_pcap* pcap = &s->pcap[s_port];
    /* if any pcap progress */
    if (MT_USDT_ST20_RX_PCAP_DUMP_ENABLED()) {
      if (!pcap->usdt_dump) {
        int estimated_total_pkts = s->st20_frame_size / ST_VIDEO_BPM_SIZE;
        /* dump 5 frames */
        rv_start_pcap(s, s_port, estimated_total_pkts * 5, false, NULL);
        pcap->usdt_dump = true;
      }
    } else {
      if (pcap->usdt_dump) {
        rv_stop_pcap(s, s_port);
        pcap->usdt_dump = false;
      }
    }

    rv = mt_rxq_burst(s->rxq[s_port], &mbuf[0], s->rx_burst_size);
    s->cur_succ_burst_cnt = rv;
    if (rv) {
      ST_SESSION_STAT_INC(s, port_user_stats, stat_burst_succ_cnt);
      ST_SESSION_STAT_ADD(s, port_user_stats, stat_burst_pkts_sum, rv);
      if (rv > s->stat_burst_pkts_max) {
        s->stat_burst_pkts_max = rv;
        s->port_user_stats.stat_burst_pkts_max = rv;
      }
      if (rv >= (s->rx_burst_size / 2)) {
        s->in_continuous_burst[s_port] = true;
      }

      check_err_status = rv_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);

      if (check_err_status < 0) {
        dbg("%s(%d,%d), handle mbuf fail %d\n", __func__, s->idx, s_port,
            check_err_status);
      }

      rte_pktmbuf_free_bulk(&mbuf[0], rv);

      done = false;
    } else {
      s->in_continuous_burst[s_port] = false;
    }
  }

  /* submit if any */
  if (s->dma_copy && s->dma_dev) mt_dma_submit(s->dma_dev);

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int rv_uinit_hw(struct st_rx_video_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->rxq[i]) {
      mt_rxq_put(s->rxq[i]);
      s->rxq[i] = NULL;
    }
  }

  return 0;
}

static int rv_init_hw(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  int idx = s->idx, num_port = ops->num_port;
  struct mt_rxq_flow flow;
  enum mtl_port port;
  uint64_t bps;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].s_port = i;

    memset(&flow, 0, sizeof(flow));
    bps = 0;
    st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, ops->interlaced,
                           &bps);
    flow.bytes_per_sec = bps / 8;
    rte_memcpy(flow.dip_addr, ops->ip_addr[i], MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow.dip_addr))
      rte_memcpy(flow.sip_addr, ops->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    else
      rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st20_dst_port[i];
    if (rv_is_hdr_split(s)) {
      flow.flags |= MT_RXQ_FLOW_F_HDR_SPLIT;
#ifdef ST_HAS_DPDK_HDR_SPLIT
      flow.hdr_split_mbuf_cb_priv = s;
      flow.hdr_split_mbuf_cb = rv_hdrs_mbuf_callback_fn;
#else
      err("%s(%d), no hdr_split support on this build\n", __func__, idx);
      rv_uinit_hw(s);
      return -ENOTSUP;
#endif
    }
    if (mt_has_cni_rx(impl, port)) flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;

    /* no flow for data path only */
    if (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), rxq get without flow for port %d as data path only\n", __func__,
           s->idx, i);
      s->rxq[i] = mt_rxq_get(impl, port, NULL);
    } else {
      s->rxq[i] = mt_rxq_get(impl, port, &flow);
    }
    if (!s->rxq[i]) {
      rv_uinit_hw(s);
      return -EIO;
    }
    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         rv_queue_id(s, i), flow.dst_port);
  }

  return 0;
}

static int rv_uinit_mcast(struct mtl_main_impl* impl,
                          struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!s->mcast_joined[i]) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    mt_mcast_leave(impl, mt_ip_to_u32(ops->ip_addr[i]),
                   mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
  }

  return 0;
}

static int rv_init_mcast(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st20_rx_ops* ops = &s->ops;
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

static int rv_init_rtcp_uhdr(struct mtl_main_impl* impl,
                             struct st_rx_video_session_impl* s,
                             enum mtl_session_port s_port, struct mt_udp_hdr* uhdr) {
  int idx = s->idx;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  int ret;
  struct rte_ether_hdr* eth = &uhdr->eth;
  struct rte_ipv4_hdr* ipv4 = &uhdr->ipv4;
  struct rte_udp_hdr* udp = &uhdr->udp;
  struct st20_rx_ops* ops = &s->ops;
  uint8_t* dip = ops->ip_addr[s_port];
  uint8_t* sip = mt_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = mt_eth_d_addr(eth);

  /* ether hdr */
  ret = mt_dst_ip_mac(impl, dip, d_addr, port, impl->arp_timeout_ms);
  if (ret < 0) {
    err("%s(%d), get mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0], dip[1],
        dip[2], dip[3]);
    return ret;
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
  ipv4->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ipv4->next_proto_id = IPPROTO_UDP;
  mtl_memcpy(&ipv4->src_addr, sip, MTL_IP_ADDR_LEN);
  mtl_memcpy(&ipv4->dst_addr, dip, MTL_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st20_dst_port[s_port] + 1);
  udp->dst_port = htons(s->st20_dst_port[s_port] + 1);
  udp->dgram_cksum = 0;

  return 0;
}

static int rv_init_rtcp(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                        struct st_rx_video_session_impl* s) {
  int idx = s->idx;
  int mgr_idx = mgr->idx;
  struct st20_rx_ops* ops = &s->ops;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);
    struct mt_udp_hdr uhdr;
    memset(&uhdr, 0x0, sizeof(uhdr));
    int ret = rv_init_rtcp_uhdr(impl, s, i, &uhdr);
    if (ret < 0) return ret;
    char name[MT_RTCP_MAX_NAME_LEN];
    snprintf(name, sizeof(name), ST_RX_VIDEO_PREFIX "M%dS%dP%d", mgr_idx, idx, i);
    if (!ops->rtcp.nack_interval_us)
      ops->rtcp.nack_interval_us = 250; /* default 250 us */
    if (!ops->rtcp.seq_bitmap_size) ops->rtcp.seq_bitmap_size = 64;
    struct mt_rtcp_rx_ops rtcp_ops = {
        .port = port,
        .name = name,
        .udp_hdr = &uhdr,
        .nacks_send_interval = ops->rtcp.nack_interval_us * NS_PER_US,
        .seq_bitmap_size = ops->rtcp.seq_bitmap_size,
        .seq_skip_window = ops->rtcp.seq_skip_window, /* this can be 0 */
    };
    s->rtcp_rx[i] = mt_rtcp_rx_create(impl, &rtcp_ops);
    if (!s->rtcp_rx[i]) {
      err("%s(%d,%d), mt_rtcp_rx_create fail on port %d\n", __func__, mgr_idx, idx, i);
      return -EIO;
    }
  }

  return 0;
}

static int rv_uinit_rtcp(struct st_rx_video_session_impl* s) {
  for (int i = 0; i < s->ops.num_port; i++) {
    if (s->rtcp_rx[i]) {
      mt_rtcp_rx_free(s->rtcp_rx[i]);
      s->rtcp_rx[i] = NULL;
    }
  }

  return 0;
}

static int rv_init_pkt_handler(struct st_rx_video_session_impl* s) {
  if (st20_is_frame_type(s->ops.type)) {
    enum st20_detect_status detect_status = s->detector.status;
    if (detect_status == ST20_DETECT_STAT_DETECTING) {
      s->pkt_handler = rv_handle_detect_pkt;
    } else if ((detect_status != ST20_DETECT_STAT_SUCCESS) &&
               (detect_status != ST20_DETECT_STAT_DISABLED)) {
      s->pkt_handler = rv_handle_detect_err;
    } else {
      if (s->st22_info)
        s->pkt_handler = rv_handle_st22_pkt;
      else if (rv_is_hdr_split(s))
        s->pkt_handler = rv_handle_hdr_split_pkt;
      else
        s->pkt_handler = rv_handle_frame_pkt;
    }
  } else {
    s->pkt_handler = rv_handle_rtp_pkt;
  }

  return 0;
}

static int rv_uinit(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  rv_stop_pcap_dump(s);
  rv_uinit_mcast(impl, s);
  rv_uinit_rtcp(s);
  rv_uinit_sw(impl, s);
  rv_uinit_hw(s);
  return 0;
}

static void rv_reset_slot(struct st_rx_video_session_impl* s,
                          struct st_rx_video_slot_impl* slot) {
  if (!slot) return;

  if (slot->frame) {
    rte_atomic32_set(&slot->frame->refcnt, 0);
    slot->frame = NULL;
  }

  rv_slot_init_frame_size(slot);
  slot->tmstamp = -1;
  slot->seq_id_base = 0;
  slot->seq_id_base_u32 = 0;
  slot->seq_id_got = false;
  slot->pkts_received = 0;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) slot->pkts_recv_per_port[i] = 0;
  slot->timestamp_first_pkt = 0;
  slot->second_field = false;
  slot->st22_payload_length = 0;
  slot->st22_box_hdr_length = 0;
  slot->last_pkt_idx = -1;
  memset(&slot->meta, 0, sizeof(slot->meta));
  memset(&slot->st22_meta, 0, sizeof(slot->st22_meta));
  if (slot->frame_bitmap && s->st20_frame_bitmap_size)
    memset(slot->frame_bitmap, 0, s->st20_frame_bitmap_size);
  if (slot->slice_info) memset(slot->slice_info, 0, sizeof(*slot->slice_info));
}

static void rv_session_reset(struct st_rx_video_session_impl* s,
                             bool init_stat_time_now) {
  if (!s) return;

  s->slot_idx = -1;
  s->dma_slot = NULL;
  s->dma_copy = false;
  s->st22_expect_frame_size = 0;
  s->st22_expect_size_per_frame = 0;
  s->usdt_frame_cnt = 0;
  s->stat_bytes_received = 0;
  s->stat_pkts_received = 0;
  s->stat_pkts_out_of_order = 0;
  s->stat_pkts_redundant_dropped = 0;
  s->stat_pkts_idx_dropped = 0;
  s->stat_pkts_idx_oo_bitmap = 0;
  s->stat_pkts_offset_dropped = 0;
  s->stat_pkts_wrong_len_dropped = 0;
  s->stat_frames_dropped = 0;
  s->stat_frames_pks_missed = 0;
  s->stat_slot_get_frame_fail = 0;
  s->stat_burst_succ_cnt = 0;
  s->stat_burst_pkts_sum = 0;
  s->stat_burst_pkts_max = 0;
  s->stat_pkts_no_slot = 0;
  s->stat_pkts_retransmit = 0;
  s->stat_pkts_dma = 0;
  s->stat_pkts_rtp_ring_full = 0;
  s->stat_pkts_simulate_loss = 0;
  s->burst_loss_cnt = 0;
  s->stat_last_time = init_stat_time_now ? mt_get_monotonic_time() : 0;
  s->cpu_busy_score = 0;
  s->dma_busy_score = 0;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    s->redundant_error_cnt[i] = 0;
    s->in_continuous_burst[i] = false;
  }

  rte_atomic32_set(&s->stat_frames_received, 0);
  rte_atomic32_set(&s->cbs_incomplete_frame_cnt, 0);
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  if (s->tp) memset(s->tp, 0, sizeof(*s->tp));
  mt_stat_u64_init(&s->stat_time);

  if (s->st20_frames) {
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      struct st_frame_trans* frame = &s->st20_frames[i];
      rte_atomic32_set(&frame->refcnt, 0);
      frame->user_meta_data_size = 0;
    }
  }

  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    rv_reset_slot(s, &s->slots[i]);
    s->slots[i].idx = i;
  }
}

static int rv_attach(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                     struct st_rx_video_session_impl* s, struct st20_rx_ops* ops,
                     struct st22_rx_ops* st22_ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];
  struct st_fps_timing fps_tm;

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  ret = st20_get_pgroup(ops->fmt, &s->st20_pg);
  if (ret < 0) {
    err("%s(%d), get pgroup fail %d\n", __func__, idx, ret);
    return ret;
  }
  ret = st_get_fps_timing(ops->fps, &fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  if (st20_is_frame_type(ops->type) && (ops->flags & ST20_RX_FLAG_HDR_SPLIT)) {
    s->is_hdr_split = true;
    info("%s(%d), hdr_split enabled in ops\n", __func__, idx);
  }

  s->impl = impl;
  s->frame_time = (double)1000000000.0 * fps_tm.den / fps_tm.mul;
  s->frame_time_sampling = (double)(fps_tm.sampling_clock_rate) * fps_tm.den / fps_tm.mul;
  /* Calculate bytes per line, rounding up if there's a remainder */
  size_t raw_bytes_size = (size_t)ops->width * s->st20_pg.size;
  s->st20_bytes_in_line =
      (raw_bytes_size + s->st20_pg.coverage - 1) / s->st20_pg.coverage;
  s->st20_linesize = s->st20_bytes_in_line;
  if (ops->linesize > s->st20_linesize)
    s->st20_linesize = ops->linesize;
  else if (ops->linesize) {
    err("%s(%d), invalid linesize %u\n", __func__, idx, ops->linesize);
    return -EINVAL;
  }

  s->st20_fb_size = s->st20_linesize * ops->height;
  if (ops->interlaced) s->st20_fb_size = s->st20_fb_size >> 1;
  s->slice_lines = ops->slice_lines;
  if (!s->slice_lines) s->slice_lines = ops->height / 32;
  s->slice_size = ops->width * s->slice_lines * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_frames_cnt = ops->framebuff_cnt;
  if (st22_ops) {
    s->st20_frame_size = st22_ops->framebuff_max_size;
    s->st20_fb_size = s->st20_frame_size;
    s->st22_ops_flags = st22_ops->flags;
  } else
    s->st20_frame_size = ops->width * ops->height * s->st20_pg.size / s->st20_pg.coverage;
  s->st20_uframe_size = ops->uframe_size;
  if (ops->interlaced) s->st20_frame_size = s->st20_frame_size >> 1;
  /* at least 800 byte for each packet */
  s->st20_frame_bitmap_size = s->st20_frame_size / 800 / 8;
  /* one line at line 2 packets for all the format */
  if (s->st20_frame_bitmap_size < ops->height * 2 / 8)
    s->st20_frame_bitmap_size = ops->height * 2 / 8;
  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "RX_VIDEO_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  for (int i = 0; i < num_port; i++) {
    s->st20_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx * 2);
  }

  /* init estimated trs */
  int estimated_total_pkts = s->st20_frame_size / ST_VIDEO_BPM_SIZE;
  s->trs = s->frame_time / estimated_total_pkts;

  if (ops->rx_burst_size) {
    s->rx_burst_size = ops->rx_burst_size;
    info("%s(%d), user customized rx_burst_size %u\n", __func__, idx, s->rx_burst_size);
  } else {
    s->rx_burst_size = 128;
  }

  /* init simulated packet loss for test usage */
  if (s->ops.flags & ST20_RX_FLAG_SIMULATE_PKT_LOSS) {
    uint16_t burst_loss_max = 1;
    float sim_loss_rate = 0.1;
    if (ops->rtcp.burst_loss_max) burst_loss_max = ops->rtcp.burst_loss_max;
    if (ops->rtcp.sim_loss_rate > 0.0 && ops->rtcp.sim_loss_rate < 1.0)
      sim_loss_rate = ops->rtcp.sim_loss_rate;
    s->burst_loss_max = burst_loss_max;
    s->sim_loss_rate = sim_loss_rate;
    info("%s(%d), simulated packet loss max burst %u rate %f\n", __func__, idx,
         burst_loss_max, sim_loss_rate);
  }

  rv_session_reset(s, true);

  s->dma_nb_desc = 128;
  s->dma_slot = NULL;
  s->dma_dev = NULL;
  if (ops->flags & ST20_RX_FLAG_TIMING_PARSER_STAT) {
    info("%s(%d), enable the timing analyze stat\n", __func__, idx);
    s->enable_timing_parser = true;
    s->enable_timing_parser_stat = true;
  }
  if (ops->flags & ST20_RX_FLAG_TIMING_PARSER_META) {
    info("%s(%d), enable the timing analyze meta\n", __func__, idx);
    s->enable_timing_parser = true;
    s->enable_timing_parser_meta = true;
  }

  ret = rv_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rv_init_hw fail %d\n", __func__, idx, ret);
    rv_uinit(impl, s);
    return ret;
  }

  if (st20_is_frame_type(ops->type) && (!st22_ops) &&
      ((ops->flags & ST20_RX_FLAG_AUTO_DETECT) || s->enable_timing_parser)) {
    /* init sw after detected */
    ret = rv_detector_init(s);
    if (ret < 0) {
      err("%s(%d), rv_detector_init fail %d\n", __func__, idx, ret);
      rv_uinit(impl, s);
      return ret;
    }
  } else {
    ret = rv_init_sw(impl, mgr, s, st22_ops);
    if (ret < 0) {
      err("%s(%d), rv_init_sw fail %d\n", __func__, idx, ret);
      rv_uinit_hw(s);
      return ret;
    }
  }

  ret = rv_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rv_init_mcast fail %d\n", __func__, idx, ret);
    rv_uinit(impl, s);
    return ret;
  }

  if (ops->flags & ST20_RX_FLAG_ENABLE_RTCP) {
    ret = rv_init_rtcp(impl, mgr, s);
    if (ret < 0) {
      rv_uinit(impl, s);
      err("%s(%d), rv_init_rtcp fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  ret = rv_init_pkt_handler(s);
  if (ret < 0) {
    err("%s(%d), init pkt handler fail %d\n", __func__, idx, ret);
    rv_uinit(impl, s);
    return -EIO;
  }

  s->attached = true;
  info("%s(%d), %d frames with size %" PRIu64 "(%" PRIu64 ",%" PRIu64 "), type %d, %s\n",
       __func__, idx, s->st20_frames_cnt, s->st20_frame_size, s->st20_frame_bitmap_size,
       s->st20_uframe_size, ops->type, ops->interlaced ? "interlace" : "progressive");
  info("%s(%d), w %u h %u fmt %s packing %d pt %d flags 0x%x frame time %fms fps %f\n",
       __func__, idx, ops->width, ops->height, st20_fmt_name(ops->fmt), ops->packing,
       ops->payload_type, ops->flags, s->frame_time / NS_PER_MS,
       st_frame_rate(s->ops.fps));
  return 0;
}

#if defined(MTL_ENABLE_FUZZING_ST20) || defined(MTL_ENABLE_FUZZING_ST22)
void st_rx_video_session_fuzz_reset(struct st_rx_video_session_impl* s) {
  rv_session_reset(s, true);
}

int st_rx_video_session_fuzz_handle_pkt(struct st_rx_video_session_impl* s,
                                        struct rte_mbuf* mbuf,
                                        enum mtl_session_port s_port) {
  if (!s || !mbuf) return -EINVAL;

  bool ctrl_thread = true;

  if (s->pkt_handler) return s->pkt_handler(s, mbuf, s_port, ctrl_thread);

  if (s->st22_info) return rv_handle_st22_pkt(s, mbuf, s_port, ctrl_thread);

  if (st20_is_frame_type(s->ops.type))
    return rv_handle_frame_pkt(s, mbuf, s_port, ctrl_thread);

  return rv_handle_rtp_pkt(s, mbuf, s_port, ctrl_thread);
}
#endif

static int rv_poll_vsync(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct st_vsync_info* vsync = &s->vsync;
  uint64_t cur_tsc = mt_get_tsc(impl);

  if (!vsync->init) return 0;

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

static int rv_send_nack(struct st_rx_video_session_impl* s) {
  for (int i = 0; i < s->ops.num_port; i++) {
    if (s->rtcp_rx[i]) mt_rtcp_rx_send_nack_packet(s->rtcp_rx[i]);
  }
  return 0;
}

static int rvs_pkt_rx_tasklet_handler(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_rx_video_session_impl* s;
  int sidx;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_video_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    pending += rv_pkt_rx_tasklet(s);

    /* check vsync if it has vsync flag enabled */
    if (s->ops.flags & ST20_RX_FLAG_ENABLE_VSYNC) rv_poll_vsync(impl, s);
    if (s->ops.flags & ST20_RX_FLAG_ENABLE_RTCP) rv_send_nack(s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }

    rx_video_session_put(mgr, sidx);
  }

  return pending;
}

void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s) {
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  rte_atomic32_set(&s->cbs_incomplete_frame_cnt, 0);
  s->cpu_busy_score = 0;
  s->dma_busy_score = 0;
  s->imiss_busy_score = 0;
}

void rx_video_session_cal_cpu_busy(struct mtl_sch_impl* sch,
                                   struct st_rx_video_session_impl* s) {
  uint64_t avg_ns_per_loop = mt_sch_avg_ns_loop(sch);
  /* assume one taskelt can bulk 3 pkts */
  s->cpu_busy_score = (double)avg_ns_per_loop / 3 / s->trs * 100.0;
  dbg("%s(%d), avg_ns_per_loop %" PRIu64 ", trs %f, busy %f\n", __func__, s->idx,
      avg_ns_per_loop, s->trs, s->cpu_busy_score);
  s->stat_cpu_busy_score = s->cpu_busy_score;

  /* update imiss busy */
  int incomplete_frame_cnt = rte_atomic32_read(&s->cbs_incomplete_frame_cnt);
  rte_atomic32_set(&s->cbs_incomplete_frame_cnt, 0);
  if ((incomplete_frame_cnt > 0) && (s->cpu_busy_score > 8.0)) {
    enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
    struct mtl_port_status stats;
    memset(&stats, 0, sizeof(stats));
    mt_read_admin_port_stats(s->impl, port, &stats);
    if (stats.rx_hw_dropped_packets) {
      dbg("%s(%d,%d), incomplete %d and hw_dropped_pkts %" PRIu64 "\n", __func__,
          sch->idx, s->idx, incomplete_frame_cnt, stats.rx_hw_dropped_packets);
      s->imiss_busy_score += 40.0;
    }
    if (s->imiss_busy_score > 95.0) {
      notice("%s(%d,%d), imiss busy, incomplete %d and hw_dropped_pkts %" PRIu64 "\n",
             __func__, sch->idx, s->idx, incomplete_frame_cnt,
             stats.rx_hw_dropped_packets);
    }
  } else {
    s->imiss_busy_score = 0;
  }

  /* update dma busy */
  int dma_previous_busy_cnt = rte_atomic32_read(&s->dma_previous_busy_cnt);
  rte_atomic32_set(&s->dma_previous_busy_cnt, 0);
  if (dma_previous_busy_cnt) {
    s->dma_busy_score += 40.0;
    if (s->dma_busy_score > 100.0) {
      notice("%s(%d,%d), dma busy, cnt %d\n", __func__, sch->idx, s->idx,
             dma_previous_busy_cnt);
    }
  } else {
    s->dma_busy_score = 0;
  }
}

static int rv_migrate_dma(struct mtl_main_impl* impl,
                          struct st_rx_video_session_impl* s) {
  rv_free_dma(impl, s);
  rv_init_dma(impl, s);
  return 0;
}

static void rv_stat(struct st_rx_video_sessions_mgr* mgr,
                    struct st_rx_video_session_impl* s) {
  int m_idx = mgr->idx;
  int idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->stat_frames_received);
  double framerate = frames_received / time_sec;
  double dump_period_s = mt_stat_dump_period_s(mgr->parent);

  rte_atomic32_set(&s->stat_frames_received, 0);

  char extra_info[128] = "";
  if (s->stat_slices_received || s->stat_pkts_redundant_dropped) {
    int offset = 0;
    if (s->stat_slices_received) {
      offset += snprintf(extra_info + offset, sizeof(extra_info) - offset, " slices %d",
                         s->stat_slices_received);
    }
    if (s->stat_pkts_redundant_dropped) {
      offset +=
          snprintf(extra_info + offset, sizeof(extra_info) - offset, "%sredundant %d",
                   s->stat_slices_received ? " + " : " ", s->stat_pkts_redundant_dropped);
    }
  }
  notice("RX_VIDEO_SESSION(%d,%d:%s): fps %f frames %d pkts %d%s\n", m_idx, idx,
         s->ops_name, framerate, frames_received, s->stat_pkts_received, extra_info);

  notice("RX_VIDEO_SESSION(%d,%d): throughput %f Mb/s, cpu busy %f\n", m_idx, idx,
         (double)s->stat_bytes_received * 8 / dump_period_s / MTL_STAT_M_UNIT,
         s->stat_cpu_busy_score);
  s->stat_pkts_received = 0;
  s->stat_bytes_received = 0;
  s->stat_slices_received = 0;
  s->stat_last_time = cur_time_ns;

  if (s->stat_frames_dropped || s->stat_pkts_idx_dropped || s->stat_pkts_offset_dropped) {
    notice(
        "RX_VIDEO_SESSION(%d,%d): incomplete frames %d, pkts (idx error: %d, offset "
        "error: %d, idx out of bitmap: %d, missed: %d)\n",
        m_idx, idx, s->stat_frames_dropped, s->stat_pkts_idx_dropped,
        s->stat_pkts_offset_dropped, s->stat_pkts_idx_oo_bitmap,
        s->stat_frames_pks_missed);
    s->stat_frames_dropped = 0;
    s->stat_pkts_idx_dropped = 0;
    s->stat_pkts_idx_oo_bitmap = 0;
    s->stat_frames_pks_missed = 0;
  }
  if (s->stat_pkts_rtp_ring_full) {
    notice("RX_VIDEO_SESSION(%d,%d): rtp dropped pkts %d as ring full\n", m_idx, idx,
           s->stat_pkts_rtp_ring_full);
    s->stat_pkts_rtp_ring_full = 0;
  }
  if (s->stat_pkts_no_slot) {
    notice("RX_VIDEO_SESSION(%d,%d): dropped pkts %d as no slot\n", m_idx, idx,
           s->stat_pkts_no_slot);
    s->stat_pkts_no_slot = 0;
  }
  /* TODO tracing out of order per port */
  if (s->stat_pkts_out_of_order) {
    notice("RX_VIDEO_SESSION(%d,%d): out of order pkts %d\n", m_idx, idx,
           s->stat_pkts_out_of_order);
    s->stat_pkts_out_of_order = 0;
  }
  if (s->stat_pkts_wrong_pt_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong hdr payload type dropped pkts %d\n", m_idx,
           idx, s->stat_pkts_wrong_pt_dropped);
    s->stat_pkts_wrong_pt_dropped = 0;
  }
  if (s->stat_pkts_wrong_ssrc_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong hdr ssrc dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_ssrc_dropped);
    s->stat_pkts_wrong_ssrc_dropped = 0;
  }
  if (s->stat_pkts_wrong_interlace_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong hdr interlace dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_interlace_dropped);
    s->stat_pkts_wrong_interlace_dropped = 0;
  }
  if (s->stat_pkts_wrong_len_dropped) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong len dropped pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_len_dropped);
    s->stat_pkts_wrong_len_dropped = 0;
  }
  if (s->stat_pkts_enqueue_fallback) {
    notice("RX_VIDEO_SESSION(%d,%d): lcore enqueue fallback pkts %d\n", m_idx, idx,
           s->stat_pkts_enqueue_fallback);
    s->stat_pkts_enqueue_fallback = 0;
  }
  if (s->dma_dev) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d by dma copy, dma busy %f\n", m_idx, idx,
           s->stat_pkts_dma, s->dma_busy_score);
    s->stat_pkts_dma = 0;
  }
  if (s->stat_pkts_slice_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d drop as slice add fail\n", m_idx, idx,
           s->stat_pkts_slice_fail);
    s->stat_pkts_slice_fail = 0;
  }
  if (s->stat_pkts_slice_merged) {
    notice("RX_VIDEO_SESSION(%d,%d): pkts %d merged as slice\n", m_idx, idx,
           s->stat_pkts_slice_merged);
    s->stat_pkts_slice_merged = 0;
  }
  if (s->stat_pkts_multi_segments_received) {
    notice("RX_VIDEO_SESSION(%d,%d): multi segments pkts %d\n", m_idx, idx,
           s->stat_pkts_multi_segments_received);
    s->stat_pkts_multi_segments_received = 0;
  }
  if (s->stat_pkts_not_bpm) {
    notice("RX_VIDEO_SESSION(%d,%d): not bpm hdr split pkts %d\n", m_idx, idx,
           s->stat_pkts_not_bpm);
    s->stat_pkts_not_bpm = 0;
  }
  if (s->stat_pkts_wrong_payload_hdr_split) {
    notice("RX_VIDEO_SESSION(%d,%d): wrong payload hdr split pkts %d\n", m_idx, idx,
           s->stat_pkts_wrong_payload_hdr_split);
    s->stat_pkts_wrong_payload_hdr_split = 0;
  }
  if (s->stat_mismatch_hdr_split_frame) {
    notice("RX_VIDEO_SESSION(%d,%d): hdr split mismatch frames %d\n", m_idx, idx,
           s->stat_mismatch_hdr_split_frame);
    s->stat_mismatch_hdr_split_frame = 0;
  }
  if (s->stat_pkts_copy_hdr_split) {
    notice("RX_VIDEO_SESSION(%d,%d): hdr split copied pkts %d\n", m_idx, idx,
           s->stat_pkts_copy_hdr_split);
    s->stat_pkts_copy_hdr_split = 0;
  }
  if (s->stat_vsync_mismatch) {
    notice("RX_VIDEO_SESSION(%d,%d): vsync mismatch cnt %u\n", m_idx, idx,
           s->stat_vsync_mismatch);
    s->stat_vsync_mismatch = 0;
  }
  if (s->stat_slot_get_frame_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): slot get frame fail %u\n", m_idx, idx,
           s->stat_slot_get_frame_fail);
    s->stat_slot_get_frame_fail = 0;
  }
  if (s->stat_slot_query_ext_fail) {
    notice("RX_VIDEO_SESSION(%d,%d): slot query ext fail %u\n", m_idx, idx,
           s->stat_slot_query_ext_fail);
    s->stat_slot_query_ext_fail = 0;
  }
  if (s->stat_pkts_simulate_loss) {
    notice("RX_VIDEO_SESSION(%d,%d): simulate loss drop %u\n", m_idx, idx,
           s->stat_pkts_simulate_loss);
    s->stat_pkts_simulate_loss = 0;
  }
  if (s->stat_pkts_user_meta) {
    notice("RX_VIDEO_SESSION(%d,%d): user meta pkts %d invalid %d\n", m_idx, idx,
           s->stat_pkts_user_meta, s->stat_pkts_user_meta_err);
    s->stat_pkts_user_meta = 0;
    s->stat_pkts_user_meta_err = 0;
  }
  if (s->stat_pkts_retransmit) {
    notice("RX_VIDEO_SESSION(%d,%d): retransmit pkts %d\n", m_idx, idx,
           s->stat_pkts_retransmit);
    s->stat_pkts_retransmit = 0;
  }
  if (s->ops.interlaced) {
    notice("RX_VIDEO_SESSION(%d,%d): interlace first field %u second field %u\n", m_idx,
           idx, s->stat_interlace_first_field, s->stat_interlace_second_field);
    s->stat_interlace_first_field = 0;
    s->stat_interlace_second_field = 0;
  }
  if (s->stat_st22_boxes) {
    notice("RX_VIDEO_SESSION(%d,%d): st22 video support boxes received %u \n", m_idx, idx,
           s->stat_st22_boxes);
    s->stat_st22_boxes = 0;
  }
  if (s->stat_burst_succ_cnt) {
    notice("RX_VIDEO_SESSION(%d,%d): succ burst max %u, avg %f\n", m_idx, idx,
           s->stat_burst_pkts_max,
           (float)s->stat_burst_pkts_sum / s->stat_burst_succ_cnt);
    s->stat_burst_pkts_max = 0;
    s->stat_burst_succ_cnt = 0;
    s->stat_burst_pkts_sum = 0;
  }

  struct st_rx_video_tp* tp = s->tp;
  if (tp && tp->stat_untrusted_pkts) {
    info("%s(%d), untrusted %u pkts time for timing parser\n", __func__, idx,
         tp->stat_untrusted_pkts);
    tp->stat_untrusted_pkts = 0;
  }
  if (s->enable_timing_parser_stat) rv_tp_stat(s);

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("RX_VIDEO_SESSION(%d,%d): tasklet time avg %.2fus max %.2fus min %.2fus\n",
           m_idx, idx, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_notify_frame_us > 8) {
    notice("RX_VIDEO_SESSION(%d,%d): notify frame max %uus\n", m_idx, idx,
           s->stat_max_notify_frame_us);
  }
  s->stat_max_notify_frame_us = 0;

  for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
    struct mt_rx_pcap* pcap = &s->pcap[s_port];
    if (pcap->pcap) {
      MT_USDT_ST20_RX_PCAP_DUMP(m_idx, idx, s_port, pcap->file_name, pcap->dumped_pkts);
    }
  }
}

static int rvs_pkt_rx_tasklet_start(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  int idx = mgr->idx;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_rx_video_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_video_session_try_get(mgr, sidx);
    if (!s) continue;
    /* re-calculate the vsync */
    st_vsync_calculate(impl, &s->vsync);
    rx_video_session_put(mgr, sidx);
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rv_detach(struct mtl_main_impl* impl, struct st_rx_video_sessions_mgr* mgr,
                     struct st_rx_video_session_impl* s) {
  if (!mgr || !s) return -EINVAL;
  s->attached = false;
  rv_stat(mgr, s);
  rv_uinit(impl, s);
  return 0;
}

static int rv_update_src(struct st_rx_video_sessions_mgr* mgr,
                         struct st_rx_video_session_impl* s,
                         struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st20_rx_ops* ops = &s->ops;
  struct mtl_main_impl* impl = mgr->parent;

  rv_uinit_rtcp(s);
  rv_uinit_mcast(impl, s);
  rv_uinit_hw(s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->ip_addr[i], src->ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops->mcast_sip_addr[i], src->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st20_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx * 2);
  }

  ret = rv_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rv_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    rv_uinit_hw(s);
    return ret;
  }

  if (ops->flags & ST20_RX_FLAG_ENABLE_RTCP) {
    ret = rv_init_rtcp(impl, mgr, s);
    if (ret < 0) {
      rv_uinit_mcast(impl, s);
      rv_uinit_hw(s);
      err("%s(%d), init rtcp fail %d\n", __func__, idx, ret);
      return ret;
    }
  }

  return 0;
}

static int rv_mgr_update_src(struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s,
                             struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }
  ret = rv_update_src(mgr, s, src);
  rx_video_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int rvs_mgr_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                        struct st_rx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  mgr->parent = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rvs_pkt_rx";
  ops.start = rvs_pkt_rx_tasklet_start;
  ops.handler = rvs_pkt_rx_tasklet_handler;

  mgr->pkt_rx_tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->pkt_rx_tasklet) {
    err("%s(%d), pkt_rx_tasklet register fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rvs_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                          struct st_rx_video_session_impl* s, int idx) {
  rv_detach(mgr->parent, mgr, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);
  return 0;
}

static int rvs_mgr_uinit(struct st_rx_video_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_video_session_impl* s;

  if (mgr->pkt_rx_tasklet) {
    mtl_sch_unregister_tasklet(mgr->pkt_rx_tasklet);
    mgr->pkt_rx_tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = rx_video_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rvs_mgr_detach(mgr, s, i);
    rx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static struct st_rx_video_session_impl* rv_mgr_attach(struct mtl_sch_impl* sch,
                                                      struct st20_rx_ops* ops,
                                                      struct st22_rx_ops* st22_ops) {
  struct st_rx_video_sessions_mgr* mgr = &sch->rx_video_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_rx_video_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (!rx_video_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = rv_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rv_attach(mgr->parent, mgr, s, ops, st22_ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_video_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_video_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int st_rvs_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                             struct st_rx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rvs_mgr_detach(mgr, s, idx);

  rx_video_session_put(mgr, idx);

  return 0;
}

static int rvs_mgr_update(struct st_rx_video_sessions_mgr* mgr) {
  int max_idx = 0;
  struct mtl_main_impl* impl = mgr->parent;
  uint64_t sleep_us = mt_sch_default_sleep_us(impl);
  struct st_rx_video_session_impl* s;

  for (int i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    s = mgr->sessions[i];
    if (!s) continue;
    max_idx = i + 1;
    sleep_us = RTE_MIN(s->advice_sleep_us, sleep_us);
  }
  dbg("%s(%d), sleep us %" PRIu64 ", max_idx %d\n", __func__, mgr->idx, sleep_us,
      max_idx);
  mgr->max_idx = max_idx;
  if (mgr->pkt_rx_tasklet) mt_tasklet_set_sleep(mgr->pkt_rx_tasklet, sleep_us);
  return 0;
}

static int rv_sessions_stat(void* priv) {
  struct st_rx_video_sessions_mgr* mgr = priv;
  struct st_rx_video_session_impl* s;

  if (!mgr) return -EINVAL;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_video_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    rv_stat(mgr, s);
    rx_video_session_put(mgr, j);
  }

  return 0;
}

int st_rx_video_sessions_sch_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->rx_video_init) return 0;

  struct st_rx_video_sessions_mgr* rx_video_mgr = &sch->rx_video_mgr;

  /* create video context */
  ret = rvs_mgr_init(impl, sch, rx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_rvs_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  mt_stat_register(impl, rv_sessions_stat, rx_video_mgr, "rx_video");
  sch->rx_video_init = true;
  return 0;
}

int st_rx_video_sessions_sch_uinit(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  if (!sch->rx_video_init) return 0;

  struct st_rx_video_sessions_mgr* rx_video_mgr = &sch->rx_video_mgr;

  mt_stat_unregister(impl, rv_sessions_stat, rx_video_mgr);
  rvs_mgr_uinit(rx_video_mgr);
  sch->rx_video_init = false;

  return 0;
}

int st_rx_video_session_migrate(struct mtl_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx) {
  rv_init(mgr, s, idx);
  if (s->dma_dev) rv_migrate_dma(impl, s);
  return 0;
}

/* Remove any session ports that map to a down physical port.
 * When a down port is found at index i, all further entries are shifted down
 * one slot (port[i] = port[i+1], ...) and num_port is decremented.
 * Only ops->port[] is shifted — all other per-port arrays (ip_addr, udp_port, etc.)
 * are left untouched; with the reduced num_port they are simply never indexed.
 * Returns -EIO if every port is down (caller must abort). */
static int rv_ops_prune_down_ports(struct mtl_main_impl* impl, struct st20_rx_ops* ops) {
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
      rte_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      rte_memcpy(ops->ip_addr[j], ops->ip_addr[j + 1], MTL_IP_ADDR_LEN);
      rte_memcpy(ops->mcast_sip_addr[j], ops->mcast_sip_addr[j + 1], MTL_IP_ADDR_LEN);
      ops->udp_port[j] = ops->udp_port[j + 1];
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

static int rv_ops_check(struct st20_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;
  enum st20_type type = ops->type;

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

  if (st20_is_frame_type(type)) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
    if (ops->type == ST20_TYPE_SLICE_LEVEL) {
      if (!ops->notify_slice_ready) {
        err("%s, pls set notify_slice_ready\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->flags & ST20_RX_FLAG_AUTO_DETECT) {
      if (!ops->notify_detected) {
        err("%s, pls set notify_detected\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->query_ext_frame) {
      if (!(ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
        err("%s, pls enable incomplete frame flag for query ext mode\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->flags & ST20_RX_FLAG_HDR_SPLIT) {
      if (num_ports > 1) {
        /* only 1 port allowed since the pkt payload is assigned to frame directly */
        err("%s, hdr split only support 1 port, num_ports %d\n", __func__, num_ports);
        return -EINVAL;
      }
    }
  }

  if (ops->uframe_size) {
    if (!ops->uframe_pg_callback) {
      err("%s, pls set uframe_pg_callback\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    if (!(ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
      err("%s, pls enable ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME for silce mode\n",
          __func__);
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

static int rv_st22_ops_prune_down_ports(struct mtl_main_impl* impl,
                                        struct st22_rx_ops* ops) {
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
      rte_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      rte_memcpy(ops->ip_addr[j], ops->ip_addr[j + 1], MTL_IP_ADDR_LEN);
      rte_memcpy(ops->mcast_sip_addr[j], ops->mcast_sip_addr[j + 1], MTL_IP_ADDR_LEN);
      ops->udp_port[j] = ops->udp_port[j + 1];
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

static int rv_st22_ops_check(struct st22_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

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
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
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

st20_rx_handle st20_rx_create_with_mask(struct mtl_main_impl* impl,
                                        struct st20_rx_ops* ops, mt_sch_mask_t sch_mask) {
  struct mtl_sch_impl* sch;
  struct st_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret, quota_mbs_wo_dma = 0;
  uint64_t bps;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rv_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, rv_ops_prune_down_ports fail %d\n", __func__, ret);
    return NULL;
  }

  ret = rv_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ops->flags & ST20_RX_FLAG_AUTO_DETECT) {
    ret = st20_get_bandwidth_bps(1920, 1080, ops->fmt, ST_FPS_P59_94, false, &bps);
  } else {
    ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps,
                                 ops->interlaced, &bps);
  }
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }

  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;
  if (!mt_user_quota_active(impl)) {
    if (ST20_TYPE_RTP_LEVEL == ops->type) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_RTP_PER_SCH;
    } else {
      quota_mbs_wo_dma =
          quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_NO_DMA_PER_SCH;
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_PER_SCH;
    }
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST20_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST20_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  enum mt_sch_type type =
      mt_user_rxv_separate_sch(impl) ? MT_SCH_TYPE_RX_VIDEO_ONLY : MT_SCH_TYPE_DEFAULT;
  sch = mt_sch_get_by_socket(impl, quota_mbs, type, sch_mask, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = rv_mgr_attach(sch, ops, NULL);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), rv_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  if (!mt_user_quota_active(impl) && st20_is_frame_type(ops->type) && !s->dma_dev) {
    int extra_quota_mbs = quota_mbs_wo_dma - quota_mbs;
    ret = mt_sch_add_quota(sch, extra_quota_mbs);
    if (ret >= 0) quota_mbs += extra_quota_mbs;
  }

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st20_handle = s_impl;

  rte_atomic32_inc(&impl->st20_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

st20_rx_handle st20_rx_create(mtl_handle mt, struct st20_rx_ops* ops) {
  return st20_rx_create_with_mask(mt, ops, MT_SCH_MASK_ALL);
}

int st20_rx_update_source(st20_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rv_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st20_rx_get_sch_idx(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st20_rx_pcapng_dump(st20_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return rv_start_pcap_dump(s, max_dump_packets, sync, meta);
}

int st20_rx_get_session_stats(st20_rx_handle handle, struct st20_rx_user_stats* stats) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_video_session_impl* s = s_impl->impl;

  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  return 0;
}

int st20_rx_reset_session_stats(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_video_session_impl* s = s_impl->impl;

  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  return 0;
}

int st20_rx_free(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct mtl_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rvs_mgr_detach(&sch->rx_video_mgr, s);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_rx_sessions_cnt);
  notice("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

int st20_rx_put_framebuff(st20_rx_handle handle, void* framebuff) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct st_frame_trans* st20_frame;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];
    if (st20_frame->addr == framebuff) {
      dbg("%s(%d), put frame at %d\n", __func__, s->idx, i);
      return rv_put_frame(s, st20_frame);
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, s->idx, framebuff);
  return -EIO;
}

size_t st20_rx_get_framebuffer_size(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_fb_size;
}

int st20_rx_get_framebuffer_count(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  s = s_impl->impl;
  return s->st20_frames_cnt;
}

void* st20_rx_get_mbuf(st20_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->rtps_ring;
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

void st20_rx_put_mbuf(st20_rx_handle handle, void* mbuf) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_HANDLE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

bool st20_rx_dma_enabled(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return s->dma_dev ? true : false;
}

int st20_rx_get_queue_meta(st20_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    meta->queue_id[i] = rv_queue_id(s, i);
  }

  return 0;
}

int st20_rx_timing_parser_critical(st20_rx_handle handle, struct st20_rx_tp_pass* pass) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  struct st_rx_video_session_impl* s = s_impl->impl;
  int idx = s->idx;
  if (!s->enable_timing_parser) {
    err("%s(%d), timing parser control not enabled\n", __func__, idx);
    return -EIO;
  }
  if (!s->tp) {
    err("%s(%d), timing parser info not init\n", __func__, idx);
    return -EBUSY;
  }

  mtl_memcpy(pass, &s->tp->pass, sizeof(*pass));
  return 0;
}

st22_rx_handle st22_rx_create(mtl_handle mt, struct st22_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st22_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_rx_ops st20_ops;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rv_st22_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, rv_st22_ops_prune_down_ports fail %d\n", __func__, ret);
    return NULL;
  }

  ret = rv_st22_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    ret = st20_get_bandwidth_bps(ops->width, ops->height, ST20_FMT_YUV_422_10BIT,
                                 ops->fps, false, &bps);
    if (ret < 0) {
      err("%s, get_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    bps /= 4; /* default compress ratio 1/4 */
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
    quota_mbs *= 2; /* double quota for RTP path */
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

  if (ops->flags & ST22_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST22_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  enum mt_sch_type type =
      mt_user_rxv_separate_sch(impl) ? MT_SCH_TYPE_RX_VIDEO_ONLY : MT_SCH_TYPE_DEFAULT;
  sch = mt_sch_get_by_socket(impl, quota_mbs, type, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  /* reuse st20 type */
  memset(&st20_ops, 0, sizeof(st20_ops));
  st20_ops.name = ops->name;
  st20_ops.priv = ops->priv;
  st20_ops.num_port = ops->num_port;
  for (int i = 0; i < ops->num_port; i++) {
    memcpy(st20_ops.ip_addr[i], ops->ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(st20_ops.mcast_sip_addr[i], ops->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(st20_ops.port[i], MTL_PORT_MAX_LEN, "%s", ops->port[i]);
    st20_ops.udp_port[i] = ops->udp_port[i];
  }
  if (ops->flags & ST22_RX_FLAG_DATA_PATH_ONLY)
    st20_ops.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST22_RX_FLAG_ENABLE_VSYNC) st20_ops.flags |= ST20_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    st20_ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  if (ops->flags & ST22_RX_FLAG_ENABLE_RTCP) {
    st20_ops.flags |= ST20_RX_FLAG_ENABLE_RTCP;
    st20_ops.rtcp = ops->rtcp;
  }
  if (ops->flags & ST22_RX_FLAG_SIMULATE_PKT_LOSS)
    st20_ops.flags |= ST20_RX_FLAG_SIMULATE_PKT_LOSS;
  st20_ops.pacing = ops->pacing;
  if (ops->type == ST22_TYPE_RTP_LEVEL)
    st20_ops.type = ST20_TYPE_RTP_LEVEL;
  else
    st20_ops.type = ST20_TYPE_FRAME_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.interlaced = ops->interlaced;
  st20_ops.fmt = ST20_FMT_YUV_422_10BIT;
  st20_ops.payload_type = ops->payload_type;
  st20_ops.ssrc = ops->ssrc;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.notify_rtp_ready = ops->notify_rtp_ready;
  st20_ops.framebuff_cnt = ops->framebuff_cnt;
  st20_ops.notify_event = ops->notify_event;
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = rv_mgr_attach(sch, &st20_ops, ops);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), rv_mgr_attach fail\n", __func__, sch->idx);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_ST22_HANDLE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st22_handle = s_impl;

  rte_atomic32_inc(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st22_rx_update_source(st22_rx_handle handle, struct st_rx_source_info* src) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rv_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st22_rx_get_sch_idx(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st22_rx_pcapng_dump(st22_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return rv_start_pcap_dump(s, max_dump_packets, sync, meta);
}

int st22_rx_free(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct mtl_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct mtl_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;

  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rvs_mgr_detach(&sch->rx_video_mgr, s);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update mgr status */
  mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  rvs_mgr_update(&sch->rx_video_mgr);
  mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

void* st22_rx_get_mbuf(st22_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->rtps_ring;
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

void st22_rx_put_mbuf(st22_rx_handle handle, void* mbuf) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st22_rx_put_framebuff(st22_rx_handle handle, void* framebuff) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct st_frame_trans* st20_frame;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    st20_frame = &s->st20_frames[i];
    if (st20_frame->addr == framebuff) {
      dbg("%s(%d), put frame at %d\n", __func__, s->idx, i);
      return rv_put_frame(s, st20_frame);
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, s->idx, framebuff);
  return -EIO;
}

void* st22_rx_get_fb_addr(st22_rx_handle handle, uint16_t idx) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st20_frames[idx].addr;
}

int st22_rx_get_queue_meta(st22_rx_handle handle, struct st_queue_meta* meta) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != MT_ST22_HANDLE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    meta->queue_id[i] = rv_queue_id(s, i);
  }

  return 0;
}
