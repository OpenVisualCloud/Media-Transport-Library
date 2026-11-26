/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

#include <algorithm>
#include <cmath>

uint16_t udp_port_for_idx(int idx, bool hdr_split, int base) {
  return (hdr_split ? 6970 : base) + idx * 2;
}

tests_context* init_test_ctx(struct st_tests_context* global_ctx, int idx,
                             uint16_t fb_cnt, bool check_sha) {
  auto* tctx = new tests_context();
  tctx->idx = idx;
  tctx->ctx = global_ctx;
  tctx->fb_cnt = fb_cnt;
  tctx->fb_idx = 0;
  tctx->check_sha = check_sha;
  tctx->ext_fb = NULL;
  tctx->ext_fb_malloc = NULL;
  tctx->ext_fb_iova = MTL_BAD_IOVA;
  tctx->ext_fb_iova_map_sz = 0;
  tctx->ext_frames = NULL;
  return tctx;
}

bool st20_alloc_ext_frames(mtl_handle m_handle, tests_context* tctx, size_t frame_size) {
  tctx->ext_frames =
      (struct st20_ext_frame*)st_test_zmalloc(sizeof(*tctx->ext_frames) * tctx->fb_cnt);
  if (!tctx->ext_frames) return false;
  size_t pg_sz = mtl_page_size(m_handle);
  size_t fb_size = frame_size * tctx->fb_cnt;
  tctx->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz);
  tctx->ext_fb_malloc = st_test_zmalloc(tctx->ext_fb_iova_map_sz + pg_sz);
  if (!tctx->ext_fb_malloc) return false;
  tctx->ext_fb = (uint8_t*)MTL_ALIGN((uint64_t)tctx->ext_fb_malloc, pg_sz);
  tctx->ext_fb_iova = mtl_dma_map(m_handle, tctx->ext_fb, tctx->ext_fb_iova_map_sz);
  info("%s, session %d ext_fb %p\n", __func__, tctx->idx, tctx->ext_fb);
  if (tctx->ext_fb_iova == MTL_BAD_IOVA) return false;

  for (int j = 0; j < tctx->fb_cnt; j++) {
    tctx->ext_frames[j].buf_addr = tctx->ext_fb + j * frame_size;
    tctx->ext_frames[j].buf_iova = tctx->ext_fb_iova + j * frame_size;
    tctx->ext_frames[j].buf_len = frame_size;
  }
  return true;
}

bool st20_alloc_dma_ext_frames(mtl_handle m_handle, tests_context* tctx, size_t fb_size) {
  tctx->ext_frames =
      (struct st20_ext_frame*)st_test_zmalloc(sizeof(*tctx->ext_frames) * tctx->fb_cnt);
  if (!tctx->ext_frames) return false;
  mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(m_handle, fb_size * tctx->fb_cnt);
  if (!dma_mem) return false;
  tctx->dma_mem = dma_mem;

  for (int j = 0; j < tctx->fb_cnt; j++) {
    tctx->ext_frames[j].buf_addr = (uint8_t*)mtl_dma_mem_addr(dma_mem) + j * fb_size;
    tctx->ext_frames[j].buf_iova = mtl_dma_mem_iova(dma_mem) + j * fb_size;
    tctx->ext_frames[j].buf_len = fb_size;
  }
  return true;
}

void tx_feed_packet_ex(tests_context* ctx, bool incomplete_odd_frames,
                       std::function<uint64_t(tests_context*)> tai_fn) {
  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st20_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st20_tx_get_mbuf(handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    uint64_t tai = (tai_fn && !ctx->pkt_idx) ? tai_fn(ctx) : 0;
    tx_video_build_rtp_packet(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len,
                              incomplete_odd_frames);
    if (tai_fn)
      st20_tx_put_mbuf_tai(handle, mbuf, mbuf_len, tai);
    else
      st20_tx_put_mbuf(handle, mbuf, mbuf_len);
  }
}

void tx_feed_packet(void* args) {
  tx_feed_packet_ex((tests_context*)args);
}

int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;
  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_video_frame(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->ready.load(std::memory_order_acquire)) return -EIO;
  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO; /* not ready */

  if (ctx->slice) {
    uint8_t* fb = (uint8_t*)st20_tx_get_framebuffer(handle, ctx->fb_idx);
    memset(fb, 0x0, ctx->frame_size);
    ctx->lines_ready[ctx->fb_idx] = 0;
  }

  *next_frame_idx = ctx->fb_idx;
  int fb_send = __atomic_load_n(&ctx->fb_send, __ATOMIC_RELAXED);
  if (ctx->field_period_ns > 0.0) meta->second_field = (fb_send % 2) != 0;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                  struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  if (!ctx->ptp_time_first_frame) {
    ctx->ptp_time_first_frame = mtl_ptp_read_time(ctx->ctx->handle);
  }

  *next_frame_idx = ctx->fb_idx;
  int fb_send = __atomic_load_n(&ctx->fb_send, __ATOMIC_RELAXED);

  if (ctx->user_pacing) {
    meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
    meta->timestamp = ctx->ptp_time_first_frame + ctx->frame_time * fb_send * 2;
  } else if (ctx->user_timestamp) {
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = fb_send;
  }
  dbg("%s, next_frame_idx %u timestamp %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->timestamp);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_ext_video_frame(void* priv, uint16_t* next_frame_idx,
                            struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->ready.load(std::memory_order_acquire)) return -EIO;
  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO; /* not ready */

  constexpr int max_slots =
      static_cast<int>(sizeof(ctx->ext_fb_in_use) / sizeof(ctx->ext_fb_in_use[0]));
  if (static_cast<int>(ctx->fb_idx) >= max_slots) {
    err("%s, fb_idx %d exceeds ext_fb_in_use capacity %d\n", __func__, ctx->fb_idx,
        max_slots);
    return -EIO;
  }
  if (__atomic_load_n(&ctx->ext_fb_in_use[ctx->fb_idx], __ATOMIC_ACQUIRE)) {
    err("%s, ext frame %d not available\n", __func__, ctx->fb_idx);
    return -EIO;
  }

  int ret = st20_tx_set_ext_frame(handle, ctx->fb_idx, &ctx->ext_frames[ctx->fb_idx]);
  if (ret < 0) {
    err("%s, set ext framebuffer fail %d fb_idx %d\n", __func__, ret, ctx->fb_idx);
    return -EIO;
  }
  __atomic_store_n(&ctx->ext_fb_in_use[ctx->fb_idx], true, __ATOMIC_RELEASE);

  *next_frame_idx = ctx->fb_idx;
  int fb_send = __atomic_load_n(&ctx->fb_send, __ATOMIC_RELAXED);
  if (ctx->field_period_ns > 0.0) meta->second_field = (fb_send % 2) != 0;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_notify_ext_frame_done(void* priv, uint16_t frame_idx,
                             struct st20_tx_frame_meta* /*meta*/) {
  auto ctx = (tests_context*)priv;

  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO; /* not ready */
  if (!ctx->ext_frames) return -EIO;

  constexpr int max_slots =
      static_cast<int>(sizeof(ctx->ext_fb_in_use) / sizeof(ctx->ext_fb_in_use[0]));
  const int cnt = std::min(ctx->fb_cnt, max_slots);
  void* frame_addr = st20_tx_get_framebuffer(handle, frame_idx);
  for (int i = 0; i < cnt; ++i) {
    if (frame_addr == ctx->ext_frames[i].buf_addr) {
      __atomic_store_n(&ctx->ext_fb_in_use[i], false, __ATOMIC_RELEASE);
      return 0;
    }
  }

  err("%s, unknown frame_addr %p\n", __func__, frame_addr);
  return -EIO;
}

int tx_notify_timestamp_frame_done(void* priv, uint16_t /*frame_idx*/,
                                   struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  if (ctx->user_timestamp && !ctx->user_pacing) {
    dbg("%s, timestamp %u %u\n", __func__, (uint32_t)meta->timestamp, ctx->pre_timestamp);
  }

  ctx->pre_timestamp = meta->timestamp;
  return 0;
}

enum st_fps tmstamp_delta_to_fps(int delta) {
  switch (delta) {
    case 1500:
      return ST_FPS_P60;
    case 1501:
    case 1502:
      return ST_FPS_P59_94;
    case 1800:
      return ST_FPS_P50;
    case 3000:
      return ST_FPS_P30;
    case 3003:
      return ST_FPS_P29_97;
    case 3600:
      return ST_FPS_P25;
    default:
      dbg("%s, err delta %d\n", __func__, delta);
      break;
  }
  return ST_FPS_MAX;
}

int tx_notify_frame_done_check_tmstamp(void* priv, uint16_t /*frame_idx*/,
                                       struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    if (ctx->rtp_tmstamp == 0)
      ctx->rtp_tmstamp = meta->timestamp;
    else {
      int delta = meta->timestamp - ctx->rtp_tmstamp;
      if (tmstamp_delta_to_fps(delta) != meta->fps) {
        dbg("fail delta: %d\n", delta);
        ctx->tx_tmstamp_delta_fail_cnt++;
      }
      ctx->rtp_tmstamp = meta->timestamp;
    }
  }

  return 0;
}

int tx_frame_lines_ready(void* priv, uint16_t frame_idx,
                         struct st20_tx_slice_meta* meta) {
  auto ctx = (tests_context*)priv;

  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO; /* not ready */

  uint8_t* fb = (uint8_t*)st20_tx_get_framebuffer(handle, frame_idx);
  int offset = ctx->lines_ready[frame_idx] * ctx->stride;
  uint16_t lines = ctx->lines_per_slice;
  if (ctx->lines_ready[frame_idx] + lines > ctx->height)
    lines = ctx->height - ctx->lines_ready[frame_idx];
  if (lines)
    mtl_memcpy(fb + offset, ctx->frame_buf[frame_idx] + offset,
               (size_t)lines * ctx->stride);

  ctx->lines_ready[frame_idx] += lines;
  meta->lines_ready = ctx->lines_ready[frame_idx];

  dbg("%s(%d), lines ready %d\n", __func__, ctx->idx, meta->lines_ready);
  return 0;
}

int tx_video_build_ooo_mapping(tests_context* s) {
  int* ooo_mapping = s->ooo_mapping;
  int total_pkts = s->total_pkts_in_frame;
  int ooo_cnt = 0;

  MTL_MAY_UNUSED(ooo_cnt);

  for (int i = 0; i < total_pkts; i++) {
    ooo_mapping[i] = i;
  }

  int ooo_pkts = rand() % 4;
  if (ooo_pkts <= 0) ooo_pkts = 4;
  int ooo_start = rand() % 10;
  if (ooo_start <= 0) ooo_start = 10;
  int ooo_end = ooo_start + ooo_pkts;
  int ooo_step = total_pkts / 40;
  while (ooo_end < total_pkts) {
    int s_idx = ooo_start, e_idx = ooo_end, temp;
    while (s_idx <= e_idx) {
      temp = ooo_mapping[s_idx];
      ooo_mapping[s_idx] = ooo_mapping[e_idx];
      ooo_mapping[e_idx] = temp;
      s_idx++;
      e_idx--;
      ooo_cnt++;
    }
    ooo_start += ooo_step;
    ooo_end += ooo_step;
  }

  dbg("%s(%d), ooo_cnt %d\n", __func__, s->idx, ooo_cnt);
  return 0;
}

int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                              uint16_t* pkt_len, bool incomplete_odd_frames) {
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  int offset;
  int frame_size = s->frame_size;
  uint16_t row_number, row_offset;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
  int pkt_idx = s->pkt_idx;
  if (s->out_of_order_pkt) pkt_idx = s->ooo_mapping[s->pkt_idx];

  if (s->single_line) {
    row_number = pkt_idx / s->pkts_in_line;
    int pixels_in_pkt = s->pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
    row_offset = pixels_in_pkt * (pkt_idx % s->pkts_in_line);
    offset = (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->pkt_data_len * pkt_idx;
    row_number = offset / s->bytes_in_line;
    row_offset = (offset % s->bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->pkt_data_len > (row_number + 1) * s->bytes_in_line) &&
        (offset + s->pkt_data_len < frame_size)) {
      e_rtp = (struct st20_rfc4175_extra_rtp_hdr*)payload;
      payload += sizeof(*e_rtp);
    }
  }

  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = ST20_TEST_PAYLOAD_TYPE;
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  uint32_t seq_id =
      static_cast<uint32_t>(__atomic_fetch_add(&s->seq_id, 1, __ATOMIC_RELAXED));
  if (s->out_of_order_pkt)
    rtp->base.seq_number = htons(s->frame_base_seq_id + pkt_idx);
  else
    rtp->base.seq_number = htons(static_cast<uint16_t>(seq_id));
  rtp->seq_number_ext = htons(static_cast<uint16_t>(seq_id >> 16));
  int temp = s->single_line
                 ? ((s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size)
                 : (frame_size - offset);
  uint16_t data_len = s->pkt_data_len > temp ? temp : s->pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);
  if (e_rtp) {
    uint16_t row_length_0 = (row_number + 1) * s->bytes_in_line - offset;
    uint16_t row_length_1 = s->pkt_data_len - row_length_0;
    rtp->row_length = htons(row_length_0);
    e_rtp->row_length = htons(row_length_1);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons(row_number + 1);
    rtp->row_offset = htons(row_offset | ST20_SRD_OFFSET_CONTINUATION);
    *pkt_len += sizeof(*e_rtp);
  }
  if (s->check_sha) {
    mtl_memcpy(payload, s->frame_buf[s->fb_idx % TEST_SHA_HIST_NUM] + offset, data_len);
  }

  s->pkt_idx++;
  bool marker = (s->pkt_idx >= s->total_pkts_in_frame);
  if (incomplete_odd_frames && (__atomic_load_n(&s->fb_send, __ATOMIC_RELAXED) % 2) &&
      s->pkt_idx >= (s->total_pkts_in_frame / 2)) {
    marker = true;
  }
  if (marker) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->fb_idx++;
    s->rtp_tmstamp++;
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
    if (s->out_of_order_pkt) {
      tx_video_build_ooo_mapping(s);
      s->frame_base_seq_id += s->total_pkts_in_frame;
    }
  }

  return 0;
}

int rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;
  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

void rx_handle_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* hdr, bool newframe) {
  int idx = s->idx;
  struct st20_rfc4175_extra_rtp_hdr* e_hdr = NULL;
  uint16_t row_number; /* 0 to 1079 for 1080p */
  uint16_t row_offset; /* [0, 480, 960, 1440] for 1080p */
  uint16_t row_length; /* 1200 for 1080p */
  uint8_t* frame;
  uint8_t* payload;

  if (newframe) {
    if (s->frame_buf[0]) {
      std::unique_lock<std::mutex> lck(s->mtx);
      s->buf_q.push(s->frame_buf[0]);
      s->cv.notify_all();
    }
    s->frame_buf[0] = (uint8_t*)st_test_zmalloc(s->frame_size);
    ASSERT_TRUE(s->frame_buf[0] != NULL);
  }

  frame = s->frame_buf[0];
  if (!frame) return;
  payload = (uint8_t*)hdr + sizeof(*hdr);
  row_number = ntohs(hdr->row_number);
  row_offset = ntohs(hdr->row_offset);
  row_length = ntohs(hdr->row_length);
  dbg("%s(%d), row: %d %d %d\n", __func__, idx, row_number, row_offset, row_length);
  if (row_offset & ST20_SRD_OFFSET_CONTINUATION) {
    /* additional Sample Row Data */
    row_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    e_hdr = (struct st20_rfc4175_extra_rtp_hdr*)payload;
    payload += sizeof(*e_hdr);
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  if ((offset + row_length) > s->frame_size) {
    err("%s(%d: invalid offset %u frame size %" PRIu64 "\n", __func__, idx, offset,
        s->frame_size);
    return;
  }
  mtl_memcpy(frame + offset, payload, row_length);
  if (e_hdr) {
    uint16_t row2_number = ntohs(e_hdr->row_number);
    uint16_t row2_offset = ntohs(e_hdr->row_offset);
    uint16_t row2_length = ntohs(e_hdr->row_length);

    dbg("%s(%d), row: %d %d %d\n", __func__, idx, row2_number, row2_offset, row2_length);
    uint32_t offset2 =
        (row2_number * s->width + row2_offset) / s->st20_pg.coverage * s->st20_pg.size;
    if ((offset2 + row2_length) > s->frame_size) {
      err("%s(%d: invalid offset %u frame size %" PRIu64 " for extra hdr\n", __func__,
          idx, offset2, s->frame_size);
      return;
    }
    mtl_memcpy(frame + offset2, payload + row_length, row2_length);
  }
}

void rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st20_rfc4175_rtp_hdr* hdr;
  while (!ctx->stop) {
    mbuf = st20_rx_get_mbuf(handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      mbuf = st20_rx_get_mbuf(handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    uint32_t tmstamp = ntohl(hdr->base.tmstamp);
    bool newframe = false;
    ctx->packet_rec++;
    if (tmstamp != ctx->rtp_tmstamp) {
      if (ctx->packet_rec == ctx->total_pkts_in_frame || ctx->rtp_tmstamp == 0)
        newframe = true;
      ctx->rtp_tmstamp = tmstamp;
      __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
      ctx->packet_rec = 0;
    }
    if (ctx->check_sha) {
      rx_handle_rtp(ctx, hdr, newframe);
    }
    st20_rx_put_mbuf(handle, mbuf);
  }
}

int st20_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto handle = (st20_rx_handle)st20_get_handle(ctx);

  if (!handle) return -EIO;

  if (st_is_frame_complete(meta->status)) {
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&ctx->start_time, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&ctx->rtp_delta,
                       static_cast<int>(meta->timestamp - ctx->rtp_tmstamp),
                       __ATOMIC_RELAXED);
      __atomic_store_n(&ctx->start_time, st_test_get_monotonic_time(), __ATOMIC_RELEASE);
    }
  }
  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) ctx->rtp_tmstamp = meta->timestamp;
  st20_rx_put_framebuff(handle, frame);
  return 0;
}

void st20_tx_ops_init(tests_context* st20, struct st20_tx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st20->idx * 2;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 10000 + st20->idx * 2;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;
  ops->payload_type = ST20_TEST_PAYLOAD_TYPE;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->get_next_frame = tx_next_video_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

void st20_rx_ops_init(tests_context* st20, struct st20_rx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(st20->idx);
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = udp_port_for_idx(st20->idx);
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;
  ops->payload_type = ST20_TEST_PAYLOAD_TYPE;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->notify_frame_ready = st20_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

void st20_tx_assert_cnt(int expect_s20_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st20_tx_sessions_cnt, expect_s20_tx_cnt);
}

void st20_rx_assert_cnt(int expect_s20_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st20_rx_sessions_cnt, expect_s20_rx_cnt);
}

void init_single_port_tx(struct st20_tx_ops& ops, tests_context* tctx, const char* name,
                         uint16_t udp_port) {
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = tctx;
  ops.num_port = 1;
  if (tctx->ctx->mcast_only)
    memcpy(ops.dip_addr[MTL_SESSION_PORT_P], tctx->ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops.dip_addr[MTL_SESSION_PORT_P], tctx->ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           tctx->ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops.pacing = ST21_PACING_NARROW;
}

void init_single_port_rx(struct st20_rx_ops& ops, tests_context* tctx, const char* name,
                         uint16_t udp_port) {
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = tctx;
  ops.num_port = 1;
  if (tctx->ctx->mcast_only)
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], tctx->ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], tctx->ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           tctx->ctx->para.port[MTL_PORT_R]);
  ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops.pacing = ST21_PACING_NARROW;
  ops.flags = ST20_RX_FLAG_DMA_OFFLOAD;
  ops.rtp_ring_size = 1024;
}

void st20_fill_loopback_tx_ops(struct st20_tx_ops& ops, tests_context* tctx,
                               const St20StreamSpec& st, const char* name,
                               uint16_t udp_port, uint16_t rtp_pkt_size) {
  tctx->field_period_ns =
      st.interlaced ? static_cast<double>(NS_PER_S) / st_frame_rate(st.fps) : 0.0;
  init_single_port_tx(ops, tctx, name, udp_port);
  ops.type = st.tx_type;
  ops.packing = st.packing;
  ops.width = st.width;
  ops.height = st.height;
  ops.linesize = st.linesize;
  ops.interlaced = st.interlaced;
  ops.fps = st.fps;
  ops.fmt = st.fmt;
  ops.payload_type = ST20_TEST_PAYLOAD_TYPE;
  ops.framebuff_cnt = tctx->fb_cnt;
  ops.get_next_frame = tx_next_video_frame;
  if (st.tx_type == ST20_TYPE_RTP_LEVEL) {
    rtp_tx_specific_init(&ops, tctx, rtp_pkt_size);
  }
}

void st20_fill_loopback_rx_ops(struct st20_rx_ops& ops, tests_context* rctx,
                               const St20StreamSpec& st, const char* name,
                               uint16_t udp_port) {
  init_single_port_rx(ops, rctx, name, udp_port);
  ops.type = st.rx_type;
  ops.width = st.width;
  ops.height = st.height;
  ops.linesize = st.linesize;
  ops.interlaced = st.interlaced;
  ops.fps = st.fps;
  ops.fmt = st.fmt;
  ops.payload_type = ST20_TEST_PAYLOAD_TYPE;
  ops.framebuff_cnt = rctx->fb_cnt;
  ops.notify_frame_ready = st20_rx_frame_ready;
  ops.notify_rtp_ready = rx_rtp_ready;
}

void st20_publish_tx_sessions(const std::vector<tests_context*>& tx_ctx,
                              const std::vector<st20_tx_handle>& tx_h,
                              const std::vector<St20StreamSpec>& streams,
                              std::vector<std::thread>* tx_threads) {
  for (size_t i = 0; i < tx_ctx.size(); i++) {
    st20_set_handle(tx_ctx[i], tx_h[i]);
    tx_ctx[i]->ready.store(true, std::memory_order_release);
    if (tx_threads && streams[i].tx_type == ST20_TYPE_RTP_LEVEL) {
      (*tx_threads)[i] = std::thread(tx_feed_packet, tx_ctx[i]);
    }
  }
}

void st20_stop_tx_ready(const std::vector<tests_context*>& tx_ctx) {
  for (auto* c : tx_ctx) {
    if (c) c->ready.store(false, std::memory_order_release);
  }
}

void st20_setup_basic_loopback(struct st_tests_context* ctx,
                               const std::vector<St20StreamSpec>& streams,
                               const char* name, std::vector<tests_context*>& tx_ctx,
                               std::vector<tests_context*>& rx_ctx,
                               std::vector<st20_tx_handle>& tx_handle,
                               std::vector<st20_rx_handle>& rx_handle,
                               std::vector<std::thread>& rtp_thread_tx,
                               std::vector<std::thread>& rtp_thread_rx) {
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  for (int i = 0; i < sessions; i++) {
    tx_ctx[i] = init_test_ctx(ctx, i, 3);
    tx_ctx[i]->ready.store(false, std::memory_order_relaxed);
    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, tx_ctx[i], streams[i], name, udp_port_for_idx(i));
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
  }

  for (int i = 0; i < sessions; i++) {
    rx_ctx[i] = init_test_ctx(ctx, i, 3);
    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, rx_ctx[i], streams[i], name, udp_port_for_idx(i));
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    rx_ctx[i]->total_pkts_in_frame = tx_ctx[i]->total_pkts_in_frame;
    st20_set_handle(rx_ctx[i], rx_handle[i]);
    if (streams[i].rx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, rx_ctx[i]);
    }
  }

  st20_publish_tx_sessions(tx_ctx, tx_handle, streams, &rtp_thread_tx);
}

void st20_init_tx_digest_frames(mtl_handle m_handle, tests_context* tctx,
                                st20_tx_handle tx_h, const St20StreamSpec& st,
                                St20TxBufMode mode) {
  struct st20_pgroup st20_pg;
  st20_get_pgroup(st.fmt, &st20_pg);
  size_t frame_size =
      static_cast<size_t>(st.width) * st.height * st20_pg.size / st20_pg.coverage;
  if (st.interlaced) frame_size >>= 1;

  int raw_stride = st.width / st20_pg.coverage * st20_pg.size;
  int stride = raw_stride;
  size_t fb_size = frame_size;
  if (st.linesize > raw_stride) {
    stride = st.linesize;
    fb_size = static_cast<size_t>(st.linesize) * st.height;
    if (st.interlaced) fb_size >>= 1;
  }

  tctx->frame_size = frame_size;
  tctx->fb_size = fb_size;
  tctx->height = st.height;
  tctx->stride = stride;
  tctx->slice = (st.tx_type == ST20_TYPE_SLICE_LEVEL);
  tctx->lines_per_slice = st.height / 30;

  if (mode == St20TxBufMode::kUserFrame) {
    size_t uframe_size = static_cast<size_t>(st.width) * st.height * 2 * sizeof(uint16_t);
    if (st.interlaced) uframe_size >>= 1;
    tctx->uframe_size = uframe_size;
    tctx->slice = false;
    const uint32_t data_h = st.interlaced ? st.height / 2 : st.height;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      tctx->frame_buf[frame] = (uint8_t*)st_test_zmalloc(uframe_size);
      uint8_t* fb = tctx->frame_buf[frame];
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, uframe_size, frame);
      uint16_t* p10_u16 = (uint16_t*)fb;
      for (size_t k = 0; k < (uframe_size / 2); k++) p10_u16[k] &= 0x3ff;
      SHA256((unsigned char*)fb, uframe_size, tctx->shas[frame]);
      test_sha_dump("st20_rx", tctx->shas[frame]);

      auto* pg = (struct st20_rfc4175_422_10_pg2_be*)st20_tx_get_framebuffer(tx_h, frame);
      st20_yuv422p10le_to_rfc4175_422be10(
          p10_u16, p10_u16 + static_cast<size_t>(st.width) * data_h,
          p10_u16 + static_cast<size_t>(st.width) * data_h * 3 / 2, pg, st.width, data_h);
    }
    return;
  }

  if (st.tx_type == ST20_TYPE_FRAME_LEVEL) {
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_h), fb_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_h), tctx->fb_cnt);
  }
  if (mode == St20TxBufMode::kExtMap) {
    ASSERT_TRUE(st20_alloc_ext_frames(m_handle, tctx, frame_size));
  } else if (mode == St20TxBufMode::kExtDma) {
    ASSERT_TRUE(st20_alloc_dma_ext_frames(m_handle, tctx, fb_size));
  }

  const int total_lines = st.interlaced ? st.height / 2 : st.height;
  for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
    uint8_t* fb = nullptr;
    if (mode == St20TxBufMode::kExtMap) {
      fb = tctx->ext_fb + frame * frame_size;
    } else if (mode == St20TxBufMode::kExtDma) {
      fb = (uint8_t*)tctx->ext_frames[frame].buf_addr;
    } else if (st.tx_type == ST20_TYPE_FRAME_LEVEL) {
      fb = (uint8_t*)st20_tx_get_framebuffer(tx_h, frame);
    } else {
      tctx->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
      fb = tctx->frame_buf[frame];
    }
    ASSERT_TRUE(fb != NULL);
    if (st.linesize > raw_stride) {
      for (int line = 0; line < total_lines; line++) {
        st_test_rand_data(fb + stride * line, static_cast<size_t>(raw_stride), frame);
      }
    } else {
      st_test_rand_data(fb, frame_size, frame);
    }
    SHA256((unsigned char*)fb, fb_size, tctx->shas[frame]);
    test_sha_dump("st20_rx", tctx->shas[frame]);
  }
}

void st20_init_rx_slice_meta(tests_context* rctx, struct st20_rx_ops& ops_rx,
                             const St20StreamSpec& st, size_t frame_size,
                             size_t uframe_size, int slices_per_frame) {
  ops_rx.slice_lines = st.height / slices_per_frame;
  ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
  if (st.rx_type != ST20_TYPE_SLICE_LEVEL) return;

  auto* meta =
      (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
  ASSERT_TRUE(meta != NULL);
  meta->width = st.width;
  meta->height = st.height;
  meta->fps = st.fps;
  meta->fmt = st.fmt;
  meta->frame_total_size = frame_size;
  meta->uframe_total_size = uframe_size;
  meta->second_field = false;
  rctx->priv = meta;
  ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
}

void st20_bind_rx_digest_ctx(tests_context* rctx, const tests_context* tctx,
                             st20_rx_handle rx_h, const St20StreamSpec& st,
                             size_t uframe_size) {
  rctx->frame_size = tctx->frame_size;
  rctx->fb_size = tctx->fb_size;
  rctx->width = st.width;
  rctx->height = st.height;
  rctx->uframe_size = uframe_size;
  st20_get_pgroup(st.fmt, &rctx->st20_pg);
  memcpy(rctx->shas, tctx->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
  rctx->total_pkts_in_frame = tctx->total_pkts_in_frame;
  st20_set_handle(rctx, rx_h);
}

void st20_verify_rx_dma_and_queue(st20_rx_handle rx_h, bool has_dma,
                                  enum st20_type rx_type) {
  bool dma_enabled = st20_rx_dma_enabled(rx_h);
  if (has_dma && (rx_type != ST20_TYPE_RTP_LEVEL)) {
    EXPECT_TRUE(dma_enabled);
  } else {
    EXPECT_FALSE(dma_enabled);
  }
  struct st_queue_meta meta;
  EXPECT_GE(st20_rx_get_queue_meta(rx_h, &meta), 0);
}

static void st20_release_rx_ext_frame(tests_context* ctx, void* frame,
                                      void* opaque = nullptr) {
  if (opaque) {
    bool* in_use = static_cast<bool*>(opaque);
    EXPECT_TRUE(__atomic_exchange_n(in_use, false, __ATOMIC_ACQ_REL));
    return;
  }
  if (!ctx->ext_frames) return;
  const int max_slots =
      static_cast<int>(sizeof(ctx->ext_fb_in_use) / sizeof(ctx->ext_fb_in_use[0]));
  const int cnt = std::min(ctx->fb_cnt, max_slots);
  for (int j = 0; j < cnt; j++) {
    if (ctx->ext_frames[j].buf_addr == frame) {
      __atomic_store_n(&ctx->ext_fb_in_use[j], false, __ATOMIC_RELEASE);
      return;
    }
  }
}

void st20_rx_drain_bufq_put_framebuff(tests_context* ctx) {
  if (!ctx) return;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  while (!ctx->buf_q.empty()) {
    void* frame = ctx->buf_q.front();
    ctx->buf_q.pop();
    if (!ctx->second_field_q.empty()) ctx->second_field_q.pop();
    if (handle) {
      st20_rx_put_framebuff(handle, frame);
    }
    st20_release_rx_ext_frame(ctx, frame);
  }
}

void st20_rx_drain_bufq_free(tests_context* ctx) {
  if (!ctx) return;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  while (!ctx->buf_q.empty()) {
    void* frame = ctx->buf_q.front();
    ctx->buf_q.pop();
    if (!ctx->second_field_q.empty()) ctx->second_field_q.pop();
    if (frame) st_test_free(frame);
  }
}

void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx,
                          uint16_t rtp_pkt_size) {
  int ret;
  ret = st20_get_pgroup(ops->fmt, &test_ctx->st20_pg);
  ASSERT_TRUE(ret == 0);

  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * test_ctx->st20_pg.size / test_ctx->st20_pg.coverage;

  if (ops->packing == ST20_PACKING_GPM_SL) {
    size_t bytes_in_pkt = MTL_PKT_MAX_RTP_BYTES - sizeof(struct st20_rfc4175_rtp_hdr);
    int pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
    test_ctx->total_pkts_in_frame = ops->height * pkts_in_line;
    int pixels_in_pkts = (ops->width + pkts_in_line - 1) / pkts_in_line;
    test_ctx->pkt_data_len = (pixels_in_pkts + test_ctx->st20_pg.coverage - 1) /
                             test_ctx->st20_pg.coverage * test_ctx->st20_pg.size;
    test_ctx->pkts_in_line = pkts_in_line;
  } else if (ops->packing == ST20_PACKING_BPM) {
    test_ctx->pkt_data_len = 1260;
    if (rtp_pkt_size) {
      /* whole pgroups in what is left once both RFC 4175 headers are on */
      size_t hdrs_len =
          sizeof(struct st20_rfc4175_rtp_hdr) + sizeof(struct st20_rfc4175_extra_rtp_hdr);
      test_ctx->pkt_data_len = static_cast<int>(
          (rtp_pkt_size - hdrs_len) / test_ctx->st20_pg.size * test_ctx->st20_pg.size);
    }
    int pixels_in_pkts =
        test_ctx->pkt_data_len * test_ctx->st20_pg.coverage / test_ctx->st20_pg.size;
    test_ctx->total_pkts_in_frame =
        ceil((double)ops->width * ops->height / pixels_in_pkts);
  } else if (ops->packing == ST20_PACKING_GPM) {
    int max_data_len = MTL_PKT_MAX_RTP_BYTES - sizeof(struct st20_rfc4175_rtp_hdr) -
                       sizeof(struct st20_rfc4175_extra_rtp_hdr);
    int pg_per_pkt = max_data_len / test_ctx->st20_pg.size;
    test_ctx->total_pkts_in_frame = (ceil)((double)ops->width * ops->height /
                                           (test_ctx->st20_pg.coverage * pg_per_pkt));
    test_ctx->pkt_data_len = pg_per_pkt * test_ctx->st20_pg.size;
  } else {
    err("%s, invalid packing mode: %d\n", __func__, ops->packing);
    return;
  }

  test_ctx->pkt_idx = 0;
  test_ctx->seq_id = 1;
  test_ctx->frame_base_seq_id = test_ctx->seq_id;
  test_ctx->bytes_in_line = bytes_in_line;
  test_ctx->width = ops->width;
  test_ctx->single_line = (ops->packing == ST20_PACKING_GPM_SL);
  test_ctx->frame_size =
      ops->width * ops->height * test_ctx->st20_pg.size / test_ctx->st20_pg.coverage;

  ops->rtp_frame_total_pkts = test_ctx->total_pkts_in_frame;
  ops->rtp_pkt_size = test_ctx->pkt_data_len + sizeof(struct st20_rfc4175_rtp_hdr);
  if (ops->packing != ST20_PACKING_GPM_SL) /* no extra for GPM_SL */
    ops->rtp_pkt_size += sizeof(struct st20_rfc4175_extra_rtp_hdr);
  if (rtp_pkt_size && ops->packing == ST20_PACKING_BPM) ops->rtp_pkt_size = rtp_pkt_size;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void stop_and_wake_only(std::vector<tests_context*>& contexts) {
  for (auto* ctx : contexts) {
    if (!ctx) continue;
    ctx->stop = true;
    std::unique_lock<std::mutex> lck(ctx->mtx);
    ctx->cv.notify_all();
  }
}

static void join_threads(std::vector<std::thread>& threads) {
  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }
}

St20DeinitGuard::St20DeinitGuard(mtl_handle handle, std::vector<tests_context*>& tx_ctx,
                                 std::vector<tests_context*>& rx_ctx,
                                 std::vector<st20_tx_handle>& tx_handle,
                                 std::vector<st20_rx_handle>& rx_handle,
                                 std::vector<std::thread>* tx_threads,
                                 std::vector<std::thread>* rx_threads)
    : m_handle_(handle),
      started_(false),
      stopped_(false),
      sessions_released_(false),
      cleaned_(false),
      tx_ctx_(tx_ctx),
      rx_ctx_(rx_ctx),
      tx_handle_(tx_handle),
      rx_handle_(rx_handle),
      tx_threads_(tx_threads),
      rx_threads_(rx_threads) {
}

St20DeinitGuard::~St20DeinitGuard() {
  cleanup();
}

void St20DeinitGuard::set_started(bool started) {
  started_ = started;
}

void St20DeinitGuard::add_thread_group(std::vector<std::thread>& threads) {
  extra_thread_groups_.push_back(&threads);
}

void St20DeinitGuard::set_tx_ctx_cleanup(CtxCleanupFn fn) {
  tx_ctx_cleanup_ = std::move(fn);
}

void St20DeinitGuard::set_rx_ctx_cleanup(CtxCleanupFn fn) {
  rx_ctx_cleanup_ = std::move(fn);
}

void St20DeinitGuard::stop() {
  if (stopped_) return;

  stop_and_wake_only(tx_ctx_);
  stop_and_wake_only(rx_ctx_);

  if (tx_threads_) join_threads(*tx_threads_);
  if (rx_threads_) join_threads(*rx_threads_);
  for (auto* group : extra_thread_groups_) {
    if (group) join_threads(*group);
  }

  if (started_ && m_handle_) {
    mtl_stop(m_handle_);
    started_ = false;
  }

  stopped_ = true;
}

void St20DeinitGuard::release_sessions() {
  if (sessions_released_) return;
  stop();

  /* Stop TX before draining RX so no new frame can enter the RX queues. */
  for (auto* c : tx_ctx_) {
    if (c && tx_ctx_cleanup_) tx_ctx_cleanup_(c);
  }

  for (auto& h : tx_handle_) {
    if (h) {
      st20_tx_free(h);
      h = NULL;
    }
  }

  /* Return queued framebuffers while the RX handles are still valid. */
  for (auto* c : rx_ctx_) {
    if (c && rx_ctx_cleanup_) rx_ctx_cleanup_(c);
  }

  for (auto& h : rx_handle_) {
    if (h) {
      st20_rx_free(h);
      h = NULL;
    }
  }

  sessions_released_ = true;
}

void St20DeinitGuard::cleanup() {
  if (cleaned_) return;

  release_sessions();

  auto free_ctxs = [this](std::vector<tests_context*>& ctxs) {
    for (auto*& c : ctxs) {
      if (!c) continue;
      if (m_handle_ && c->ext_fb && c->ext_fb_iova_map_sz &&
          c->ext_fb_iova != MTL_BAD_IOVA) {
        mtl_dma_unmap(m_handle_, c->ext_fb, c->ext_fb_iova, c->ext_fb_iova_map_sz);
      }
      tests_context_unit(c);
      delete c;
      c = NULL;
    }
  };
  free_ctxs(rx_ctx_);
  free_ctxs(tx_ctx_);

  cleaned_ = true;
}

static int st20_digest_rx_ready_common(void* priv, void* frame,
                                       struct st20_rx_frame_meta* meta, bool interlaced) {
  auto ctx = (tests_context*)priv;
  void* opaque = meta->opaque;
  dbg("%s(%d), frame %p, opaque %p\n", __func__, ctx->idx, frame, opaque);

  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  if (!handle) {
    if (opaque) st20_release_rx_ext_frame(ctx, frame, opaque);
    return -EIO;
  }

  auto put_and_release = [&]() {
    st20_rx_put_framebuff(handle, frame);
    if (opaque) st20_release_rx_ext_frame(ctx, frame, opaque);
  };

  ctx->slice_recv_timestamp = 0;
  ctx->slice_recv_lines = 0;

  if (!st_is_frame_complete(meta->status) || meta->frame_total_size != ctx->frame_size ||
      meta->uframe_total_size != ctx->uframe_size ||
      meta->frame_total_size != meta->frame_recv_size) {
    ctx->incomplete_frame_cnt++;
    put_and_release();
    return 0;
  }

  if (!interlaced) {
    if (ctx->frame_time > 0) {
      if (meta->fpt > (ctx->frame_time / 10)) {
        ctx->meta_timing_fail_cnt++;
        dbg("%s(%d), fpt %" PRId64 ", frame time %fms\n", __func__, ctx->idx, meta->fpt,
            ctx->frame_time / NS_PER_MS);
      }
      double rx_time = (double)meta->timestamp_last_pkt - meta->timestamp_first_pkt;
      if (rx_time > ctx->frame_time) {
        ctx->meta_timing_fail_cnt++;
        dbg("%s(%d), rx_time %fms\n", __func__, ctx->idx, rx_time / NS_PER_MS);
      }
    }

    if (ctx->user_timestamp && !ctx->user_pacing) {
      dbg("%s, timestamp %u %u\n", __func__, (uint32_t)meta->timestamp,
          ctx->pre_timestamp);
      if (ctx->pre_timestamp && (uint32_t)meta->timestamp != (ctx->pre_timestamp + 1)) {
        ctx->incomplete_frame_cnt++;
      }
      ctx->pre_timestamp = (uint32_t)meta->timestamp;
    }
  }

  bool pushed = false;
  {
    std::unique_lock<std::mutex> lck(ctx->mtx);
    if (!ctx->stop.load(std::memory_order_acquire) && ctx->buf_q.empty()) {
      /* Keep dynamic ext slot marked in_use until the SHA worker finishes hashing. */
      if (opaque) {
        EXPECT_TRUE(__atomic_load_n(static_cast<bool*>(opaque), __ATOMIC_ACQUIRE));
      }
      ctx->buf_q.push(frame);
      if (interlaced) ctx->second_field_q.push(meta->second_field);
      ctx->cv.notify_all();
      pushed = true;
    }
  }
  if (!pushed) put_and_release();
  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int st20_digest_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  return st20_digest_rx_ready_common(priv, frame, meta, false);
}

int st20_digest_rx_field_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  return st20_digest_rx_ready_common(priv, frame, meta, true);
}

int st20_digest_rx_slice_ready(void* priv, void* /*frame*/,
                               struct st20_rx_slice_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!st20_get_handle(ctx)) return -EIO;
  ctx->slice_cnt++;

  struct st20_rx_slice_meta* expect_meta = (struct st20_rx_slice_meta*)ctx->priv;
  if (expect_meta->width != meta->width) ctx->incomplete_slice_cnt++;
  if (expect_meta->height != meta->height) ctx->incomplete_slice_cnt++;
  if (expect_meta->fps != meta->fps) ctx->incomplete_slice_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->incomplete_slice_cnt++;
  if (expect_meta->frame_total_size != meta->frame_total_size) {
    ctx->incomplete_slice_cnt++;
  }

  struct st20_pgroup st20_pg;
  st20_get_pgroup(meta->fmt, &st20_pg);
  size_t frame_ready_size =
      meta->frame_recv_lines * meta->width * st20_pg.size / st20_pg.coverage;
  if (meta->frame_recv_size < frame_ready_size) {
    ctx->incomplete_slice_cnt++;
    dbg("%s, recv_size err %" PRIu64 " %" PRIu64 "\n", __func__, meta->frame_recv_size,
        frame_ready_size);
  }
  if (meta->frame_recv_lines < ctx->slice_recv_lines) {
    ctx->incomplete_slice_cnt++;
  }
  ctx->slice_recv_lines = meta->frame_recv_lines;
  if (!ctx->slice_recv_timestamp) {
    ctx->slice_recv_timestamp = meta->timestamp;
  } else if (ctx->slice_recv_timestamp != meta->timestamp) {
    ctx->incomplete_slice_cnt++;
    dbg("%s, time stamp err %" PRIu64 " %" PRIu64 "\n", __func__, meta->timestamp,
        ctx->slice_recv_timestamp);
  }
  return 0;
}

static void st20_digest_rx_check_common(tests_context* ctx, bool interlaced) {
  unsigned char result[SHA256_DIGEST_LENGTH];
  while (true) {
    void* frame;
    bool second_field = false;
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->cv.wait(lck, [ctx] { return ctx->stop || !ctx->buf_q.empty(); });
      if (ctx->stop) return;
      /* Keep entry in buf_q until hashing finishes so at most 1 RX buffer is held. */
      frame = ctx->buf_q.front();
      if (interlaced) second_field = ctx->second_field_q.front();
    }
    dbg("%s, frame %p\n", __func__, frame);
    int i;
    SHA256((unsigned char*)frame, ctx->uframe_size ? ctx->uframe_size : ctx->fb_size,
           result);
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      if (!memcmp(result, ctx->shas[i], SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st20_rx_error_sha", result);
      ctx->sha_fail_cnt++;
    } else if (interlaced && ((i % 2) != 0) != second_field) {
      test_sha_dump("field split error", result);
      __atomic_fetch_add(&ctx->rx_field_fail_cnt, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&ctx->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
    auto handle = (st20_rx_handle)st20_get_handle(ctx);
    if (handle) st20_rx_put_framebuff(handle, frame);
    st20_release_rx_ext_frame(ctx, frame);
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->buf_q.pop();
      if (interlaced) ctx->second_field_q.pop();
    }
  }
}

void st20_digest_rx_frame_check(void* args) {
  st20_digest_rx_check_common((tests_context*)args, false);
}

void st20_digest_rx_field_check(void* args) {
  st20_digest_rx_check_common((tests_context*)args, true);
}
