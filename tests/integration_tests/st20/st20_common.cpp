/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

#include <cmath>

uint16_t udp_port_for_idx(int idx, bool hdr_split, int base) {
  return (hdr_split ? 6970 : base) + idx * 2;
}

std::vector<St20SessionConfig> build_sessions(int sessions, enum st20_type type[],
                                              enum st20_packing packing[],
                                              enum st_fps fps[], int width[],
                                              int height[], bool interlaced[],
                                              enum st20_fmt fmt[]) {
  std::vector<St20SessionConfig> cfgs(sessions);
  for (int i = 0; i < sessions; ++i) {
    cfgs[i].type = type ? type[i] : ST20_TYPE_FRAME_LEVEL;
    cfgs[i].packing = packing ? packing[i] : ST20_PACKING_BPM;
    cfgs[i].fps = fps ? fps[i] : ST_FPS_P59_94;
    cfgs[i].width = width ? width[i] : 1920;
    cfgs[i].height = height ? height[i] : 1080;
    cfgs[i].interlaced = interlaced ? interlaced[i] : false;
    cfgs[i].fmt = fmt ? fmt[i] : ST20_FMT_YUV_422_10BIT;
  }
  return cfgs;
}

tests_context* init_test_ctx(struct st_tests_context* global_ctx, int idx,
                             uint16_t fb_cnt, bool check_sha) {
  auto* tctx = new tests_context();
  EXPECT_TRUE(tctx != NULL);
  tctx->idx = idx;
  tctx->ctx = global_ctx;
  tctx->fb_cnt = fb_cnt;
  tctx->fb_idx = 0;
  tctx->check_sha = check_sha;
  return tctx;
}

int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                              uint16_t* pkt_len);

void tx_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_video_build_rtp_packet(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf((st20_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;
  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_video_frame(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (ctx->slice) {
    uint8_t* fb =
        (uint8_t*)st20_tx_get_framebuffer((st20_tx_handle)ctx->handle, ctx->fb_idx);
    memset(fb, 0x0, ctx->frame_size);
    ctx->lines_ready[ctx->fb_idx] = 0;
  }

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                  struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (!ctx->ptp_time_first_frame) {
    ctx->ptp_time_first_frame = mtl_ptp_read_time(ctx->ctx->handle);
  }

  *next_frame_idx = ctx->fb_idx;

  if (ctx->user_pacing) {
    meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
    meta->timestamp = ctx->ptp_time_first_frame + ctx->frame_time * ctx->fb_send * 2;
  } else if (ctx->user_timestamp) {
    meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    meta->timestamp = ctx->fb_send;
  }
  dbg("%s, next_frame_idx %u timestamp %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->timestamp);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_ext_video_frame(void* priv, uint16_t* next_frame_idx,
                            struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (ctx->ext_fb_in_use[ctx->fb_idx]) {
    err("%s, ext frame %d not available\n", __func__, ctx->fb_idx);
    return -EIO;
  }

  int ret = st20_tx_set_ext_frame((st20_tx_handle)ctx->handle, ctx->fb_idx,
                                  &ctx->ext_frames[ctx->fb_idx]);
  if (ret < 0) {
    err("%s, set ext framebuffer fail %d fb_idx %d\n", __func__, ret, ctx->fb_idx);
    return -EIO;
  }
  ctx->ext_fb_in_use[ctx->fb_idx] = true;

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_next_ext_video_field(void* priv, uint16_t* next_frame_idx,
                            struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (ctx->ext_fb_in_use[ctx->fb_idx]) {
    err("%s, ext frame %d not available\n", __func__, ctx->fb_idx);
    return -EIO;
  }

  int ret = st20_tx_set_ext_frame((st20_tx_handle)ctx->handle, ctx->fb_idx,
                                  &ctx->ext_frames[ctx->fb_idx]);
  if (ret < 0) {
    err("%s, set ext framebuffer fail %d fb_idx %d\n", __func__, ret, ctx->fb_idx);
    return -EIO;
  }
  ctx->ext_fb_in_use[ctx->fb_idx] = true;

  *next_frame_idx = ctx->fb_idx;
  meta->second_field = ctx->fb_send % 2 ? true : false;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_notify_ext_frame_done(void* priv, uint16_t frame_idx,
                             struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  void* frame_addr = st20_tx_get_framebuffer((st20_tx_handle)ctx->handle, frame_idx);
  for (int i = 0; i < ctx->fb_cnt; ++i) {
    if (frame_addr == ctx->ext_frames[i].buf_addr) {
      ctx->ext_fb_in_use[i] = false;
      return 0;
    }
  }

  err("%s, unknown frame_addr %p\n", __func__, frame_addr);
  return -EIO;
}

int tx_notify_timestamp_frame_done(void* priv, uint16_t frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

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

int tx_notify_frame_done_check_tmstamp(void* priv, uint16_t frame_idx,
                                       struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

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

int tx_next_video_field(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  meta->second_field = ctx->fb_send % 2 ? true : false;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

int tx_frame_lines_ready(void* priv, uint16_t frame_idx,
                         struct st20_tx_slice_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  uint8_t* fb = (uint8_t*)st20_tx_get_framebuffer((st20_tx_handle)ctx->handle, frame_idx);
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
    int s = ooo_start, e = ooo_end, temp;
    while (s <= e) {
      temp = ooo_mapping[s];
      ooo_mapping[s] = ooo_mapping[e];
      ooo_mapping[e] = temp;
      s++;
      e--;
      ooo_cnt++;
    }
    ooo_start += ooo_step;
    ooo_end += ooo_step;
  }

  // ooo_mapping[100] = 99;

  dbg("%s(%d), ooo_cnt %d\n", __func__, s->idx, ooo_cnt);
  return 0;
}

int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                              uint16_t* pkt_len) {
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
  if (s->out_of_order_pkt)
    rtp->base.seq_number = htons(s->frame_base_seq_id + pkt_idx);
  else
    rtp->base.seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->seq_id++;
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
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->fb_idx++;
    s->rtp_tmstamp++;
    s->fb_send++;
    if (s->out_of_order_pkt) {
      tx_video_build_ooo_mapping(s);
      s->frame_base_seq_id += s->total_pkts_in_frame;
    }
  }

  return 0;
}

int rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;
  if (!ctx->handle) return -EIO; /* not ready */

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

  return;
}

void rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st20_rfc4175_rtp_hdr* hdr;
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_rx_get_mbuf((st20_rx_handle)ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_rx_get_mbuf((st20_rx_handle)ctx->handle, &usrptr, &mbuf_len);
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
      /* new frame received */
      ctx->rtp_tmstamp = tmstamp;
      ctx->fb_rec++;
      ctx->packet_rec = 0;
    }
    if (ctx->check_sha) {
      rx_handle_rtp(ctx, hdr, newframe);
    }
    st20_rx_put_mbuf((st20_rx_handle)ctx->handle, mbuf);
  }
}

int st20_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  if (st_is_frame_complete(meta->status)) {
    ctx->fb_rec++;
    if (!ctx->start_time) {
      ctx->rtp_delta = meta->timestamp - ctx->rtp_tmstamp;
      ctx->start_time = st_test_get_monotonic_time();
    }
  }
  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) ctx->rtp_tmstamp = meta->timestamp;
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
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
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st20_tx_sessions_cnt, expect_s20_tx_cnt);
}

void st20_rx_assert_cnt(int expect_s20_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
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

void st20_rx_drain_bufq_put_framebuff(tests_context* ctx) {
  if (!ctx) return;
  auto handle = (st20_rx_handle)ctx->handle;
  while (!ctx->buf_q.empty()) {
    void* frame = ctx->buf_q.front();
    ctx->buf_q.pop();
    if (!ctx->second_field_q.empty()) ctx->second_field_q.pop();
    if (handle) {
      st20_rx_put_framebuff(handle, frame);
    }
  }
}

void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx) {
  int ret;
  ret = st20_get_pgroup(ops->fmt, &test_ctx->st20_pg);
  ASSERT_TRUE(ret == 0);

  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * test_ctx->st20_pg.size / test_ctx->st20_pg.coverage;

  if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line for rtp */
    size_t bytes_in_pkt = MTL_PKT_MAX_RTP_BYTES - sizeof(struct st20_rfc4175_rtp_hdr);
    int pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
    test_ctx->total_pkts_in_frame = ops->height * pkts_in_line;
    int pixels_in_pkts = (ops->width + pkts_in_line - 1) / pkts_in_line;
    test_ctx->pkt_data_len = (pixels_in_pkts + test_ctx->st20_pg.coverage - 1) /
                             test_ctx->st20_pg.coverage * test_ctx->st20_pg.size;
    test_ctx->pkts_in_line = pkts_in_line;
  } else if (ops->packing == ST20_PACKING_BPM) {
    test_ctx->pkt_data_len = 1260;
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
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void stop_and_wake_only(std::vector<tests_context*>& contexts) {
  for (size_t i = 0; i < contexts.size(); ++i) {
    auto* ctx = contexts[i];
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
      cleaned_(false),
      ext_buf_(false),
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

void St20DeinitGuard::set_ext_buf(bool ext_buf) {
  ext_buf_ = ext_buf;
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
    if (!group) continue;
    join_threads(*group);
  }

  if (started_ && m_handle_) {
    mtl_stop(m_handle_);
    started_ = false;
  }

  stopped_ = true;
}

void St20DeinitGuard::cleanup() {
  if (cleaned_) return;

  stop();

  /* Some tests queue in-flight framebuffers that must be returned while session handles
   * are still valid (e.g. via st20_rx_put_framebuff). Run cleanup hooks before freeing
   * session handles.
   */
  for (auto* c : rx_ctx_) {
    if (!c) continue;
    if (rx_ctx_cleanup_) rx_ctx_cleanup_(c);
  }
  for (auto* c : tx_ctx_) {
    if (!c) continue;
    if (tx_ctx_cleanup_) tx_ctx_cleanup_(c);
  }

  for (auto& h : tx_handle_) {
    if (h) {
      st20_tx_free(h);
      h = NULL;
    }
  }

  for (auto& h : rx_handle_) {
    if (h) {
      st20_rx_free(h);
      h = NULL;
    }
  }

  for (auto*& c : rx_ctx_) {
    if (!c) continue;
    if (ext_buf_ && m_handle_ && c->ext_fb && c->ext_fb_iova_map_sz &&
        c->ext_fb_iova != MTL_BAD_IOVA) {
      mtl_dma_unmap(m_handle_, c->ext_fb, c->ext_fb_iova, c->ext_fb_iova_map_sz);
    }
    tests_context_unit(c);
    delete c;
    c = NULL;
  }

  for (auto*& c : tx_ctx_) {
    if (!c) continue;
    if (ext_buf_ && m_handle_ && c->ext_fb && c->ext_fb_iova_map_sz &&
        c->ext_fb_iova != MTL_BAD_IOVA) {
      mtl_dma_unmap(m_handle_, c->ext_fb, c->ext_fb_iova, c->ext_fb_iova_map_sz);
    }
    tests_context_unit(c);
    delete c;
    c = NULL;
  }

  cleaned_ = true;
}

int st20_digest_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  dbg("%s(%d), frame %p, opaque %p\n", __func__, ctx->idx, frame, meta->opaque);

  if (meta->opaque) {
    /* free dynamic ext frame */
    bool* in_use = (bool*)meta->opaque;
    EXPECT_TRUE(*in_use);
    *in_use = false;
  }

  if (!ctx->handle) return -EIO;

  ctx->slice_recv_timestamp = 0;
  ctx->slice_recv_lines = 0;

  if (!st_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != ctx->frame_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->uframe_total_size != ctx->uframe_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != meta->frame_recv_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
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

  if (ctx->user_timestamp && !ctx->user_pacing) {
    dbg("%s, timestamp %u %u\n", __func__, (uint32_t)meta->timestamp, ctx->pre_timestamp);
    if (ctx->pre_timestamp) {
      if ((uint32_t)meta->timestamp != (ctx->pre_timestamp + 1)) {
        ctx->incomplete_frame_cnt++;
      }
    }
    ctx->pre_timestamp = (uint32_t)meta->timestamp;
  }

  std::unique_lock<std::mutex> lck(ctx->mtx);
  if (ctx->buf_q.empty()) {
    ctx->buf_q.push(frame);
    ctx->cv.notify_all();
  } else {
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  }
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();

  return 0;
}

#if DUMP_INCOMPLITE_SLICE
void dump_slice_meta(struct st20_rx_slice_meta* meta) {
  info("%s, width %u height %u fps %d fmd %d field %d\n", __func__, meta->width,
       meta->height, meta->fps, meta->fmt, meta->second_field);
  info("%s, frame total size %" PRIu64 " recv size %" PRIu64 " recv lines %u\n", __func__,
       meta->frame_total_size, meta->frame_recv_size, meta->frame_recv_lines);
}
#endif

int st20_digest_rx_slice_ready(void* priv, void* frame, struct st20_rx_slice_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;
#if DUMP_INCOMPLITE_SLICE
  int old_incomplete_slice_cnt = ctx->incomplete_slice_cnt;
#endif
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
  } else {
    if (ctx->slice_recv_timestamp != meta->timestamp) {
      ctx->incomplete_slice_cnt++;
      dbg("%s, time stamp err %" PRIu64 " %" PRIu64 "\n", __func__, meta->timestamp,
          ctx->slice_recv_timestamp);
    }
  }
#if DUMP_INCOMPLITE_SLICE
  if (old_incomplete_slice_cnt != ctx->incomplete_slice_cnt) {
    dbg("%s, incomplete_slice detected\n", __func__);
    dump_slice_meta(meta);
    dump_slice_meta(expect_meta);
  }
#endif
  return 0;
}

int st20_digest_rx_field_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  ctx->slice_recv_timestamp = 0;
  ctx->slice_recv_lines = 0;

  if (!st_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != ctx->frame_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->uframe_total_size != ctx->uframe_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != meta->frame_recv_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }

  std::unique_lock<std::mutex> lck(ctx->mtx);
  if (ctx->buf_q.empty()) {
    ctx->buf_q.push(frame);
    ctx->second_field_q.push(meta->second_field);
    ctx->cv.notify_all();
  } else {
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  }
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  dbg("%s, frame %p\n", __func__, frame);
  return 0;
}

void st20_digest_rx_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  unsigned char result[SHA256_DIGEST_LENGTH];
  while (!ctx->stop) {
    if (ctx->buf_q.empty()) {
      lck.lock();
      if (!ctx->stop) ctx->cv.wait(lck);
      lck.unlock();
      continue;
    } else {
      void* frame = ctx->buf_q.front();
      ctx->buf_q.pop();
      dbg("%s, frame %p\n", __func__, frame);
      int i;
      SHA256((unsigned char*)frame, ctx->uframe_size ? ctx->uframe_size : ctx->fb_size,
             result);
      for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
        unsigned char* target_sha = ctx->shas[i];
        if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_SHA_HIST_NUM) {
        test_sha_dump("st20_rx_error_sha", result);
        ctx->sha_fail_cnt++;
      }
      ctx->check_sha_frame_cnt++;
      st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    }
  }
}

void st20_digest_rx_field_check(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  unsigned char result[SHA256_DIGEST_LENGTH];
  while (!ctx->stop) {
    if (ctx->buf_q.empty()) {
      lck.lock();
      if (!ctx->stop) ctx->cv.wait(lck);
      lck.unlock();
      continue;
    } else {
      void* frame = ctx->buf_q.front();
      bool second_field = ctx->second_field_q.front();
      ctx->buf_q.pop();
      ctx->second_field_q.pop();
      dbg("%s, frame %p\n", __func__, frame);
      int i;
      SHA256((unsigned char*)frame, ctx->uframe_size ? ctx->uframe_size : ctx->fb_size,
             result);
      for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
        unsigned char* target_sha = ctx->shas[i];
        if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_SHA_HIST_NUM) {
        test_sha_dump("st20_rx_error_sha", result);
        ctx->sha_fail_cnt++;
      }
      bool expect_second_field = i % 2 ? true : false;
      if (expect_second_field != second_field) {
        test_sha_dump("field split error", result);
        ctx->rx_field_fail_cnt++;
      }
      ctx->check_sha_frame_cnt++;
      st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    }
  }
}
