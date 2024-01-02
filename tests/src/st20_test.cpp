/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.h"

#define ST20_TRAIN_TIME_S (0) /* 0 for runtime rl */

#define ST20_TEST_PAYLOAD_TYPE (112)

#define DUMP_INCOMPLITE_SLICE (0)

static int tx_next_video_frame(void* priv, uint16_t* next_frame_idx,
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

static int tx_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                         struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;

  if (ctx->user_pacing) {
    meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
    meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 25 * 1000 * 1000;
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

static int tx_next_ext_video_frame(void* priv, uint16_t* next_frame_idx,
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

static int tx_next_ext_video_field(void* priv, uint16_t* next_frame_idx,
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

static int tx_notify_ext_frame_done(void* priv, uint16_t frame_idx,
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

static int tx_notify_timestamp_frame_done(void* priv, uint16_t frame_idx,
                                          struct st20_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (ctx->user_timestamp && !ctx->user_pacing) {
    dbg("%s, timestamp %u %u\n", __func__, (uint32_t)meta->timestamp, ctx->pre_timestamp);
  }

  ctx->pre_timestamp = meta->timestamp;
  return 0;
}

static enum st_fps tmstamp_delta_to_fps(int delta) {
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

static int tx_notify_frame_done_check_tmstamp(void* priv, uint16_t frame_idx,
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

static int tx_next_video_field(void* priv, uint16_t* next_frame_idx,
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

static int tx_frame_lines_ready(void* priv, uint16_t frame_idx,
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

static int tx_video_build_ooo_mapping(tests_context* s) {
  int* ooo_mapping = s->ooo_mapping;
  int total_pkts = s->total_pkts_in_frame;
  int ooo_cnt = 0;

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

static int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
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

static void tx_feed_packet(void* args) {
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

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;
  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;
  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void rx_handle_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* hdr,
                          bool newframe) {
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

static void rx_get_packet(void* args) {
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

static int st20_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
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

static void st20_tx_ops_init(tests_context* st20, struct st20_tx_ops* ops) {
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

static void st20_rx_ops_init(tests_context* st20, struct st20_rx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->sip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st20->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 10000 + st20->idx;
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

static void st20_tx_assert_cnt(int expect_s20_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st20_tx_sessions_cnt, expect_s20_tx_cnt);
}

static void st20_rx_assert_cnt(int expect_s20_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st20_rx_sessions_cnt, expect_s20_rx_cnt);
}

TEST(St20_tx, create_free_single) { create_free_test(st20_tx, 0, 1, 1); }
TEST(St20_tx, create_free_multi) { create_free_test(st20_tx, 0, 1, 6); }
TEST(St20_tx, create_free_mix) { create_free_test(st20_tx, 2, 3, 4); }
TEST(St20_tx, create_free_max) { create_free_max(st20_tx, TEST_CREATE_FREE_MAX); }
TEST(St20_tx, create_expect_fail) { expect_fail_test(st20_tx); }
TEST(St20_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
}
TEST(St20_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
}
TEST(St20_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}

TEST(St20_rx, create_free_single) { create_free_test(st20_rx, 0, 1, 1); }
TEST(St20_rx, create_free_multi) { create_free_test(st20_rx, 0, 1, 6); }
TEST(St20_rx, create_free_mix) { create_free_test(st20_rx, 2, 3, 4); }
TEST(St20_rx, create_free_max) { create_free_max(st20_rx, TEST_CREATE_FREE_MAX); }
TEST(St20_rx, create_expect_fail) { expect_fail_test(st20_rx); }
TEST(St20_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 0;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
}
TEST(St20_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
}

static void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx) {
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

static void st20_tx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, enum st_test_level level,
                             int sessions = 1, bool ext_buf = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<st20_tx_handle> handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext_buf) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext_buf test as it's PA iova mode\n", __func__);
      return;
    }
  }

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx[i] = new tests_context();
    ASSERT_TRUE(test_ctx[i] != NULL);

    test_ctx[i]->idx = i;
    test_ctx[i]->ctx = ctx;
    test_ctx[i]->fb_cnt = 3;
    test_ctx[i]->fb_idx = 0;
    st20_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];
    ops.width = width[i];
    ops.height = height[i];
    ops.fmt = fmt;
    ops.packing = ST20_PACKING_BPM;
    if (ext_buf) {
      ops.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops.get_next_frame = tx_next_ext_video_frame;
      ops.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops.notify_frame_done = tx_notify_frame_done_check_tmstamp;
    }
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);

    size_t frame_size = st20_tx_get_framebuffer_size(handle[i]);
    test_ctx[i]->frame_size = frame_size;

    if (ext_buf) {
      test_ctx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx[i]->ext_frames) * test_ctx[i]->fb_cnt);
      size_t pg_sz = mtl_page_size(m_handle);
      size_t fb_size = test_ctx[i]->frame_size * test_ctx[i]->fb_cnt;
      test_ctx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx[i]->ext_fb_malloc != NULL);
      test_ctx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx[i]->ext_fb_malloc, pg_sz);
      test_ctx[i]->ext_fb_iova =
          mtl_dma_map(m_handle, test_ctx[i]->ext_fb, test_ctx[i]->ext_fb_iova_map_sz);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx[i]->ext_fb);
      ASSERT_TRUE(test_ctx[i]->ext_fb_iova != MTL_BAD_IOVA);

      for (int j = 0; j < test_ctx[i]->fb_cnt; j++) {
        test_ctx[i]->ext_frames[j].buf_addr = test_ctx[i]->ext_fb + j * frame_size;
        test_ctx[i]->ext_frames[j].buf_iova = test_ctx[i]->ext_fb_iova + j * frame_size;
        test_ctx[i]->ext_frames[j].buf_len = frame_size;
      }
    }

    ASSERT_TRUE(handle[i] != NULL);
    test_ctx[i]->handle = handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  if (ctx->para.num_ports > 1)
    sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(5);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx[i]->mtx);
        test_ctx[i]->cv.notify_all();
      }
      rtp_thread[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    EXPECT_LE(test_ctx[i]->tx_tmstamp_delta_fail_cnt, 1);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    if (ext_buf) {
      mtl_dma_unmap(m_handle, test_ctx[i]->ext_fb, test_ctx[i]->ext_fb_iova,
                    test_ctx[i]->ext_fb_iova_map_sz);
    }
    tests_context_unit(test_ctx[i]);
    delete test_ctx[i];
  }
}

static void st20_rx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, enum st_test_level level,
                             int sessions = 1, bool ext_buf = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext_buf) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext_buf test as it's PA iova mode\n", __func__);
      return;
    }
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;

    if (ext_buf) {
      test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
      size_t frame_size = st20_frame_size(fmt, width[i], height[i]);
      size_t pg_sz = mtl_page_size(m_handle);
      size_t fb_size = frame_size * test_ctx_rx[i]->fb_cnt;
      test_ctx_rx[i]->ext_fb_iova_map_sz =
          mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx_rx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx_rx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_malloc != NULL);
      test_ctx_rx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_rx[i]->ext_fb_malloc, pg_sz);
      test_ctx_rx[i]->ext_fb_iova = mtl_dma_map(m_handle, test_ctx_rx[i]->ext_fb,
                                                test_ctx_rx[i]->ext_fb_iova_map_sz);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_rx[i]->ext_fb);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_iova != MTL_BAD_IOVA);

      for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
        test_ctx_rx[i]->ext_frames[j].buf_addr = test_ctx_rx[i]->ext_fb + j * frame_size;
        test_ctx_rx[i]->ext_frames[j].buf_iova =
            test_ctx_rx[i]->ext_fb_iova + j * frame_size;
        test_ctx_rx[i]->ext_frames[j].buf_len = frame_size;
      }
    }

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (ext_buf) {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
      rtp_thread_rx[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    if (ext_buf) {
      mtl_dma_unmap(m_handle, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova,
                    test_ctx_rx[i]->ext_fb_iova_map_sz);
    }
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_tx, rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps30_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P30};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps60_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P60};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, rtp_720p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_yuv422_8bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_8BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_yuv420_10bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, mix_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_tx, mix_720p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_tx, mix_1080p_fps50_fps29_97) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_tx, mix_1080p_fps50_fps59_94) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_tx, ext_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL, 3,
                   true);
}

TEST(St20_rx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, mix_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, rtp_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, frame_1080p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, mix_1080p_fps29_97_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, mix_1080p_fps59_94_fp50) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, mix_1080p_fps29_97_720p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, ext_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL, 3,
                   true);
}

TEST(St20_tx, mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P29_97};
  int width[3] = {1920, 1280, 1920};
  int height[3] = {1080, 720, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 3);
}
TEST(St20_tx, ext_frame_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 3, true);
}
TEST(St20_rx, frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, mix_s2) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 2);
}
TEST(St20_rx, frame_mix_4k_s2) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 3840};
  int height[2] = {720, 2160};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, ext_frame_mix_s2) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1280, 1920};
  int height[3] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 2, true);
}

static void st20_rx_update_src_test(enum st20_type type, int tx_sessions,
                                    enum st_test_level level = ST_TEST_LEVEL_ALL) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }
  ASSERT_TRUE(tx_sessions >= 1);
  bool tx_update_dst = (tx_sessions == 1);

  /* return if level small than global */
  if (level < ctx->level) return;

  int rx_sessions = 1;
  // 1501/1502 for one frame, max two frames.
  int max_rtp_delta = 3003;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  expect_framerate.resize(rx_sessions);
  framerate.resize(rx_sessions);
  rtp_thread_tx.resize(tx_sessions);
  rtp_thread_rx.resize(rx_sessions);

  for (int i = 0; i < rx_sessions; i++)
    expect_framerate[i] = st_frame_rate(ST_FPS_P59_94);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (2 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.flags = ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME | ST20_RX_FLAG_DMA_OFFLOAD;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * tx_sessions); /* time for train_pacing */
  sleep(5);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 10000 + 2;
  memcpy(src.sip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    test_ctx_tx[0]->seq_id = 0; /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 10000 + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    ret = st20_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  } else {
    test_ctx_tx[1]->seq_id = 0; /* reset seq id */
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_LE(test_ctx_rx[i]->rtp_delta, max_rtp_delta);
    }
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 10000 + 2;
    memcpy(src.sip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    test_ctx_tx[2]->seq_id = rand(); /* random seq id */
    for (int i = 0; i < rx_sessions; i++) {
      ret = st20_rx_update_source(rx_handle[i], &src);
      EXPECT_GE(ret, 0);
      test_ctx_rx[i]->start_time = 0;
      test_ctx_rx[i]->fb_rec = 0;
    }
    sleep(10);
    /* check rx fps */
    for (int i = 0; i < rx_sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      if (type == ST20_TYPE_FRAME_LEVEL) {
        EXPECT_LE(test_ctx_rx[i]->rtp_delta, max_rtp_delta);
      }
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 10000 + 0;
  memcpy(src.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  test_ctx_tx[0]->seq_id = rand(); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 10000 + 0;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    ret = st20_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_LE(test_ctx_rx[i]->rtp_delta, max_rtp_delta);
    }
  }

  /* stop rtp thread */
  for (int i = 0; i < rx_sessions; i++) {
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_rx[i].join();
    }
  }
  for (int i = 0; i < tx_sessions; i++) {
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  /* free all tx and rx */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    delete test_ctx_tx[i];
  }
}

TEST(St20_rx, update_source_frame) { st20_rx_update_src_test(ST20_TYPE_FRAME_LEVEL, 3); }
TEST(St20_rx, update_source_rtp) { st20_rx_update_src_test(ST20_TYPE_RTP_LEVEL, 2); }
TEST(St20_tx, update_dest_frame) { st20_rx_update_src_test(ST20_TYPE_FRAME_LEVEL, 1); }
TEST(St20_tx, update_dest_rtp) { st20_rx_update_src_test(ST20_TYPE_RTP_LEVEL, 1); }

static int st20_digest_rx_frame_ready(void* priv, void* frame,
                                      struct st20_rx_frame_meta* meta) {
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
static void dump_slice_meta(struct st20_rx_slice_meta* meta) {
  info("%s, width %u height %u fps %d fmd %d field %d\n", __func__, meta->width,
       meta->height, meta->fps, meta->fmt, meta->second_field);
  info("%s, frame total size %" PRIu64 " recv size %" PRIu64 " recv lines %u\n", __func__,
       meta->frame_total_size, meta->frame_recv_size, meta->frame_recv_lines);
}
#endif

static int st20_digest_rx_slice_ready(void* priv, void* frame,
                                      struct st20_rx_slice_meta* meta) {
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

static int st20_digest_rx_field_ready(void* priv, void* frame,
                                      struct st20_rx_frame_meta* meta) {
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

static void st20_digest_rx_frame_check(void* args) {
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

static void st20_digest_rx_field_check(void* args) {
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

static void st20_rx_digest_test(enum st20_type tx_type[], enum st20_type rx_type[],
                                enum st20_packing packing[], enum st_fps fps[],
                                int width[], int height[], bool interlaced[],
                                enum st20_fmt fmt[], bool check_fps,
                                enum st_test_level level, int sessions = 1,
                                bool out_of_order = false, bool hdr_split = false,
                                bool enable_rtcp = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> sha_check;
  int slices_per_frame = 32;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_digest_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    if (hdr_split)
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 6970 + i * 2;
    else
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = tx_type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;
    if (tx_type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    if (enable_rtcp) {
      ops_tx.flags |= ST20_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 1024;
    }

    // out of order
    if (out_of_order) {
      test_ctx_tx[i]->ooo_mapping =
          (int*)st_test_zmalloc(sizeof(int) * test_ctx_tx[i]->total_pkts_in_frame);
      ASSERT_TRUE(test_ctx_tx[i]->ooo_mapping != NULL);
      tx_video_build_ooo_mapping(test_ctx_tx[i]);
    }
    test_ctx_tx[i]->out_of_order_pkt = out_of_order;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    if (tx_type[i] == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
      EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);
    }
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->slice = (tx_type[i] == ST20_TYPE_SLICE_LEVEL);
    test_ctx_tx[i]->lines_per_slice = ops_tx.height / 30;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      if (tx_type[i] == ST20_TYPE_FRAME_LEVEL) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      } else {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
      }
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
    test_ctx_tx[i]->handle = tx_handle[i];
    if (tx_type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_digest_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    if (hdr_split)
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 6970 + i * 2;
    else
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = rx_type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = height[i] / slices_per_frame;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (hdr_split) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
    if (enable_rtcp) {
      ops_rx.flags |= ST20_RX_FLAG_ENABLE_RTCP | ST20_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 32;
      ops_rx.rtcp.seq_skip_window = 10;
      ops_rx.rtcp.burst_loss_max = 32;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      /* set expect meta data to private */
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = ops_rx.width;
      meta->height = ops_rx.height;
      meta->fps = ops_rx.fps;
      meta->fmt = ops_rx.fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = 0;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->frame_time = (double)NS_PER_S / st_frame_rate(ops_rx.fps);
    dbg("%s(%d), frame_time %f\n", __func__, i, test_ctx_rx[i]->frame_time);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    if (rx_type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    } else {
      test_ctx_rx[i]->stop = false;
      if (interlaced[i]) {
        rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
      } else {
        rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
      }
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma && (rx_type[i] != ST20_TYPE_RTP_LEVEL)) {
      EXPECT_TRUE(dma_enabled);
    } else {
      EXPECT_FALSE(dma_enabled);
    }
    struct st_queue_meta meta;
    ret = st20_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (tx_type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    rtp_thread_rx[i].join();
    if (rx_type[i] == ST20_TYPE_RTP_LEVEL) {
      sha_check[i].join();
      while (!test_ctx_rx[i]->buf_q.empty()) {
        void* frame = test_ctx_rx[i]->buf_q.front();
        st_test_free(frame);
        test_ctx_rx[i]->buf_q.pop();
      }
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL)
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    if (check_fps && !enable_rtcp) {
      EXPECT_LT(test_ctx_rx[i]->meta_timing_fail_cnt, 4);
      EXPECT_LT(test_ctx_tx[i]->tx_tmstamp_delta_fail_cnt, 4);
    }
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (rx_type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * slices_per_frame;
      if (interlaced[i]) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps59_94_s1_gpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_720p_fps59_94_s1_gpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps29_97_s1_bpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_720p_fps29_97_s1_bpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest20_field_720p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  bool interlaced[3] = {true, false, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P60, ST_FPS_P30};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s4_8bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_BPM, ST20_PACKING_GPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P50};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_8BIT, ST20_FMT_YUV_420_8BIT,
                          ST20_FMT_YUV_444_8BIT, ST20_FMT_RGB_8BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 4);
}

TEST(St20_rx, digest20_field_4320p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_frame_field_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_rtp_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_frame_s4_8bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_BPM, ST20_PACKING_GPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P119_88};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_8BIT, ST20_FMT_YUV_420_8BIT,
                          ST20_FMT_YUV_444_8BIT, ST20_FMT_RGB_8BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 4);
}

TEST(St20_rx, digest_frame_s4_10bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM, ST20_PACKING_BPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P50};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_420_10BIT,
                          ST20_FMT_YUV_444_10BIT, ST20_FMT_RGB_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 4);
}

TEST(St20_rx, digest_rtp_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                               ST20_TYPE_RTP_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_ooo_frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, true);
}

TEST(St20_rx, digest_tx_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                            ST20_TYPE_SLICE_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3, false);
}

TEST(St20_rx, digest_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, false);
}

TEST(St20_rx, digest20_field_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3, false);
}

TEST(St20_rx, digest_ooo_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, true);
}

TEST(St20_rx, digest_frame_4320p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_4096_2160_fps59_94_12bit_yuv444_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {4096};
  int height[1] = {2160};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_444_12BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 1);
}

TEST(St20_rx, digest_slice_4320p) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_dma_available(st_test_ctx())) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_ALL, 1);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_ooo_slice_4320p) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P25};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_dma_available(st_test_ctx())) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_MANDATORY, 1, true);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_hdr_split) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 1};
  int height[1] = {1080 * 1};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_ctx()->hdr_split) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_MANDATORY, 1, false, true);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_rtcp_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  /* check fps */
  st20_rx_digest_test(type, type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL, 1, false, false, true);
}

TEST(St20_rx, digest_rtcp_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1280};
  int height[3] = {1080, 1080, 720};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  /* no fps check */
  st20_rx_digest_test(type, type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, false, false, true);
}

static int st20_tx_meta_build_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                                  uint16_t* pkt_len) {
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  int offset;
  int frame_size = s->frame_size;
  uint16_t row_number, row_offset;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
  int pkt_idx = s->pkt_idx;

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
  bool marker = false;

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

  s->pkt_idx++;

  /* build incomplete frame */
  if (s->pkt_idx >= s->total_pkts_in_frame) marker = true;
  if (s->fb_send % 2) {
    if (s->pkt_idx >= (s->total_pkts_in_frame / 2)) marker = true;
  }
  if (marker) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void st20_rx_meta_feed_packet(void* args) {
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
    st20_tx_meta_build_rtp(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf((st20_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static int st20_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st20_rx_frame_meta*)ctx->priv;

  if (!ctx->handle) return -EIO;

  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  if (expect_meta->width != meta->width) ctx->rx_meta_fail_cnt++;
  if (expect_meta->height != meta->height) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fps != meta->fps) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->rx_meta_fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->rx_meta_fail_cnt++;
  expect_meta->timestamp = meta->timestamp;
  if (!st_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    if (meta->frame_total_size <= meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  } else {
    if (meta->frame_total_size != meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  }
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);

  return 0;
}

static void st20_rx_meta_test(enum st_fps fps[], int width[], int height[],
                              enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_meta_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st20_rx_meta_feed_packet, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_meta_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.flags = ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME | ST20_RX_FLAG_DMA_OFFLOAD;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_meta_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->stop = false;

    /* set expect meta data to private */
    auto meta =
        (struct st20_rx_frame_meta*)st_test_zmalloc(sizeof(struct st20_rx_frame_meta));
    ASSERT_TRUE(meta != NULL);
    meta->width = ops_rx.width;
    meta->height = ops_rx.height;
    meta->fps = ops_rx.fps;
    meta->fmt = ops_rx.fmt;
    test_ctx_rx[i]->priv = meta;

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    /* stop all thread */
    test_ctx_tx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
      test_ctx_tx[i]->cv.notify_all();
    }
    rtp_thread_tx[i].join();

    test_ctx_rx[i]->stop = true;
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    float expect_incomplete_frame_cnt = (float)test_ctx_rx[i]->fb_rec / 2;
    EXPECT_NEAR(test_ctx_rx[i]->incomplete_frame_cnt, expect_incomplete_frame_cnt,
                expect_incomplete_frame_cnt * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->rx_meta_fail_cnt, 0);
    info("%s, session %d fb_rec %d fb_incomplete %d framerate %f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, frame_meta_1080p_fps59_94_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_meta_test(fps, width, height, ST20_FMT_YUV_422_10BIT);
}

static void st20_rx_after_start_test(enum st20_type type[], enum st_fps fps[],
                                     int width[], int height[], enum st20_fmt fmt,
                                     int sessions, int repeat, enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  for (int r = 0; r < repeat; r++) {
    for (int i = 0; i < sessions; i++) {
      expect_framerate[i] = st_frame_rate(fps[i]);
      test_ctx_tx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_tx[i] != NULL);

      test_ctx_tx[i]->idx = i;
      test_ctx_tx[i]->ctx = ctx;
      test_ctx_tx[i]->fb_cnt = 3;
      test_ctx_tx[i]->fb_idx = 0;
      memset(&ops_tx, 0, sizeof(ops_tx));
      ops_tx.name = "st20_test";
      ops_tx.priv = test_ctx_tx[i];
      ops_tx.num_port = 1;
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_P]);
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
      ops_tx.pacing = ST21_PACING_NARROW;
      ops_tx.type = type[i];
      ops_tx.width = width[i];
      ops_tx.height = height[i];
      ops_tx.fps = fps[i];
      ops_tx.fmt = fmt;
      ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_next_video_frame;
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
      }
      tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);
      test_ctx_tx[i]->handle = tx_handle[i];
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = false;
        rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
      }
    }

    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_rx[i] != NULL);

      test_ctx_rx[i]->idx = i;
      test_ctx_rx[i]->ctx = ctx;
      test_ctx_rx[i]->fb_cnt = 3;
      test_ctx_rx[i]->fb_idx = 0;
      memset(&ops_rx, 0, sizeof(ops_rx));
      ops_rx.name = "st20_test";
      ops_rx.priv = test_ctx_rx[i];
      ops_rx.num_port = 1;
      memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_R]);
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
      ops_rx.pacing = ST21_PACING_NARROW;
      ops_rx.type = type[i];
      ops_rx.width = width[i];
      ops_rx.height = height[i];
      ops_rx.fps = fps[i];
      ops_rx.fmt = fmt;
      ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
      ops_rx.notify_frame_ready = st20_rx_frame_ready;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;
      ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
      rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

      test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
      ASSERT_TRUE(rx_handle[i] != NULL);
      test_ctx_rx[i]->handle = rx_handle[i];
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = false;
        rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      }
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      /* stop rx rtp if */
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = true;
        {
          std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
          test_ctx_rx[i]->cv.notify_all();
        }
        rtp_thread_rx[i].join();
      }
    }

    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      ret = st20_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
      tests_context_unit(test_ctx_rx[i]);
      delete test_ctx_rx[i];
    }

    /* stop tx rtp if */
    for (int i = 0; i < sessions; i++) {
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = true;
        {
          std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
          test_ctx_tx[i]->cv.notify_all();
        }
        rtp_thread_tx[i].join();
      }
    }

    /* free all tx */
    for (int i = 0; i < sessions; i++) {
      ret = st20_tx_free(tx_handle[i]);
      EXPECT_GE(ret, 0);
      tests_context_unit(test_ctx_tx[i]);
      delete test_ctx_tx[i];
    }

    sleep(1);
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
}

TEST(St20_rx, after_start_frame_720p_fps50_s1_r1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 1,
                           ST_TEST_LEVEL_MANDATORY);
}

TEST(St20_rx, after_start_frame_720p_fps29_97_s1_r2) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 2,
                           ST_TEST_LEVEL_ALL);
}

static int st20_rx_uframe_pg_callback(void* priv, void* frame,
                                      struct st20_rx_uframe_pg_meta* meta) {
  uint32_t w = meta->width;
  uint32_t h = meta->height;
  uint16_t* p10_u16 = (uint16_t*)frame;
  uint16_t* p10_u16_y = p10_u16;
  uint16_t* p10_u16_b = p10_u16 + w * h;
  uint16_t* p10_u16_r = p10_u16 + w * h * 3 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)meta->payload;
  uint32_t p10_offset = meta->row_number * w + meta->row_offset;
  p10_u16_y += p10_offset;
  p10_u16_b += p10_offset / 2;
  p10_u16_r += p10_offset / 2;

  st20_rfc4175_422be10_to_yuv422p10le(pg, p10_u16_y, p10_u16_b, p10_u16_r,
                                      meta->pg_cnt * 2, 1);
  return 0;
}

static void st20_rx_uframe_test(enum st20_type rx_type[], enum st20_packing packing[],
                                enum st_fps fps[], int width[], int height[],
                                bool interlaced[], enum st20_fmt fmt, bool check_fps,
                                enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> sha_check;
  std::vector<std::thread> digest_thread_rx;
  int slices_per_frame = 32;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  sha_check.resize(sessions);
  digest_thread_rx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_uframe_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    /* uframe fmt: yuv422 10bit planar */
    size_t uframe_size = (size_t)ops_tx.width * ops_tx.height * 2 * sizeof(uint16_t);
    if (interlaced[i]) uframe_size = uframe_size >> 1;
    test_ctx_tx[i]->uframe_size = uframe_size;
    test_ctx_tx[i]->slice = false;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(uframe_size);
      fb = test_ctx_tx[i]->frame_buf[frame];
      ASSERT_TRUE(fb != NULL);
      uint16_t* p10_u16 = (uint16_t*)fb;
      for (size_t i = 0; i < (uframe_size / 2); i++) {
        p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
      }
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, uframe_size, result);
      test_sha_dump("st20_rx", result);

      struct st20_rfc4175_422_10_pg2_be* pg =
          (struct st20_rfc4175_422_10_pg2_be*)st20_tx_get_framebuffer(tx_handle[i],
                                                                      frame);
      st20_yuv422p10le_to_rfc4175_422be10(
          p10_u16, (p10_u16 + ops_tx.width * ops_tx.height),
          (p10_u16 + ops_tx.width * ops_tx.height * 3 / 2), pg, ops_tx.width,
          ops_tx.height);
    }

    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_uframe_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = rx_type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = height[i] / slices_per_frame;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    /* uframe fmt: yuv422 10bit planar */
    ops_rx.uframe_size = (size_t)ops_rx.width * ops_rx.height * 2 * sizeof(uint16_t);
    ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;

    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      /* set expect meta data to private */
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = ops_rx.width;
      meta->height = ops_rx.height;
      meta->fps = ops_rx.fps;
      meta->fmt = ops_rx.fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = ops_rx.uframe_size;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    test_ctx_rx[i]->uframe_size = ops_rx.uframe_size;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      digest_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      digest_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    digest_thread_rx[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (rx_type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * slices_per_frame;
      if (interlaced[i]) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, uframe_1080p_fps59_94_s1) {
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  st20_rx_uframe_test(rx_type, packing, fps, width, height, interlaced,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_ALL, 1);
}

TEST(St20_rx, uframe_mix_s2) {
  enum st20_type rx_type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[2] = {ST20_PACKING_BPM, ST20_PACKING_GPM};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  bool interlaced[2] = {false, false};
  st20_rx_uframe_test(rx_type, packing, fps, width, height, interlaced,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_MANDATORY, 1);
}

static int st20_rx_detected(void* priv, const struct st20_detect_meta* meta,
                            struct st20_detect_reply* reply) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  struct st20_rx_slice_meta* s_meta = (struct st20_rx_slice_meta*)ctx->priv;

  ctx->lines_per_slice = meta->height / 32;
  if (s_meta) reply->slice_lines = ctx->lines_per_slice;
  if (ctx->uframe_size != 0) {
    /* uframe fmt: yuv422 10bit planar */
    ctx->uframe_size = (size_t)meta->width * meta->height * 2 * sizeof(uint16_t);
    reply->uframe_size = ctx->uframe_size;
    if (s_meta) s_meta->uframe_total_size = ctx->uframe_size;
  }

  return 0;
}

static void st20_rx_detect_test(enum st20_type tx_type[], enum st20_type rx_type[],
                                enum st20_packing packing[], enum st_fps fps[],
                                int width[], int height[], bool interlaced[],
                                bool user_frame, enum st20_fmt fmt, bool check_fps,
                                enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> sha_check;
  int slices_per_frame = 32;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_detect_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = tx_type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    if (user_frame) {
      /* uframe fmt: yuv422 10bit planar */
      size_t uframe_size = (size_t)ops_tx.width * ops_tx.height * 2 * sizeof(uint16_t);
      if (interlaced[i]) uframe_size = uframe_size >> 1;
      test_ctx_tx[i]->uframe_size = uframe_size;
      test_ctx_tx[i]->slice = false;
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(uframe_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
        ASSERT_TRUE(fb != NULL);
        uint16_t* p10_u16 = (uint16_t*)fb;
        for (size_t i = 0; i < (uframe_size / 2); i++) {
          p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
        }
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, uframe_size, result);
        test_sha_dump("st20_rx", result);

        struct st20_rfc4175_422_10_pg2_be* pg =
            (struct st20_rfc4175_422_10_pg2_be*)st20_tx_get_framebuffer(tx_handle[i],
                                                                        frame);
        st20_yuv422p10le_to_rfc4175_422be10(
            p10_u16, (p10_u16 + ops_tx.width * ops_tx.height),
            (p10_u16 + ops_tx.width * ops_tx.height * 3 / 2), pg, ops_tx.width,
            ops_tx.height);
      }
    } else {
      test_ctx_tx[i]->lines_per_slice = ops_tx.height / 30;
      test_ctx_tx[i]->slice = (tx_type[i] == ST20_TYPE_SLICE_LEVEL);
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
        ASSERT_TRUE(fb != NULL);
        st_test_rand_data(fb, frame_size, frame);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, frame_size, result);
        test_sha_dump("st20_rx", result);
      }
    }

    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_detect_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = rx_type[i];
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = height[i] / slices_per_frame;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_detected = st20_rx_detected;
    if (user_frame) {
      ops_rx.uframe_size = 1;
      ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    } else {
      ops_rx.uframe_size = 0;
    }
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD | ST20_RX_FLAG_AUTO_DETECT;

    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      /* set expect meta data to private */
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = width[i];
      meta->height = height[i];
      meta->fps = fps[i];
      meta->fmt = fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = 0;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->uframe_size = ops_rx.uframe_size;
    test_ctx_rx[i]->width = ops_tx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    rtp_thread_rx[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if ((rx_type[i] == ST20_TYPE_SLICE_LEVEL) && (height[i] >= (1080 * 4)))
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (rx_type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * slices_per_frame;
      if (interlaced[i]) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }

    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, detect_1080p_fps59_94_s1) {
  enum st20_type tx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_ALL, 1);
}

TEST(St20_rx, detect_uframe_mix_s2) {
  enum st20_type tx_type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[2] = {ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P29_97};
  int width[2] = {1280, 1280};
  int height[2] = {720, 720};
  bool interlaced[2] = {false, false};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, true,
                      ST20_FMT_YUV_422_10BIT, false, ST_TEST_LEVEL_MANDATORY, 2);
}

TEST(St20_rx, detect_mix_frame_s3) {
  enum st20_type tx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  bool interlaced[3] = {false, false, true};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, detect_mix_slice_s3) {
  enum st20_type tx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  bool interlaced[3] = {false, false, true};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_MANDATORY, 3);
}

static void st20_rx_dump_test(enum st20_type type[], enum st_fps fps[], int width[],
                              int height[], enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  if (!mtl_pmd_is_dpdk_based(m_handle, MTL_PORT_R)) {
    info("%s, MTL_PORT_R is not a DPDK based PMD, skip this case\n", __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_dump_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);

    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_dump_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */

  sleep(5);

  uint32_t max_dump_packets = 100;
  for (int i = 0; i < sessions; i++) {
    struct st_pcap_dump_meta meta;
    ret = st20_rx_pcapng_dump(rx_handle[i], max_dump_packets, true, &meta);
    EXPECT_GE(ret, 0);
    EXPECT_EQ(meta.dumped_packets, max_dump_packets);
    dbg("%s, file_name %s\n", __func__, meta.file_name);
    if (ret >= 0) remove(meta.file_name);
  }

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
      rtp_thread_rx[i].join();
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, pcap_dump) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  st20_rx_dump_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}

static int rx_query_ext_frame(void* priv, st20_ext_frame* ext_frame,
                              struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  if (!ctx->handle) return -EIO; /* not ready */
  int i = ctx->ext_idx;

  /* check ext_fb_in_use */
  if (ctx->ext_fb_in_use[i]) {
    err("%s(%d), ext frame %d in use\n", __func__, ctx->idx, i);
    return -EIO;
  }
  ext_frame->buf_addr = ctx->ext_frames[i].buf_addr;
  ext_frame->buf_iova = ctx->ext_frames[i].buf_iova;
  ext_frame->buf_len = ctx->ext_frames[i].buf_len;

  dbg("%s(%d), set ext frame %d(%p) to use\n", __func__, ctx->idx, i,
      ext_frame->buf_addr);
  ctx->ext_fb_in_use[i] = true;

  ext_frame->opaque = &ctx->ext_fb_in_use[i];

  if (++ctx->ext_idx >= ctx->fb_cnt) ctx->ext_idx = 0;
  return 0;
}

static void st20_tx_ext_frame_rx_digest_test(enum st20_packing packing[],
                                             enum st_fps fps[], int width[], int height[],
                                             bool interlaced[], enum st20_fmt fmt[],
                                             bool check_fps, enum st_test_level level,
                                             int sessions = 1, bool dynamic = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->iova == MTL_IOVA_MODE_PA) {
    info("%s, skip as it's PA iova mode\n", __func__);
    return;
  }

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> sha_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_ext_frame_digest_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame =
        interlaced[i] ? tx_next_ext_video_field : tx_next_ext_video_frame;
    ops_tx.notify_frame_done = tx_notify_ext_frame_done;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;

    test_ctx_tx[i]->ext_frames = (struct st20_ext_frame*)malloc(
        sizeof(*test_ctx_tx[i]->ext_frames) * test_ctx_tx[i]->fb_cnt);
    size_t pg_sz = mtl_page_size(m_handle);
    size_t fb_size = test_ctx_tx[i]->frame_size * test_ctx_tx[i]->fb_cnt;
    test_ctx_tx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
    size_t fb_size_malloc = test_ctx_tx[i]->ext_fb_iova_map_sz + pg_sz;
    test_ctx_tx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
    ASSERT_TRUE(test_ctx_tx[i]->ext_fb_malloc != NULL);
    test_ctx_tx[i]->ext_fb =
        (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_tx[i]->ext_fb_malloc, pg_sz);
    test_ctx_tx[i]->ext_fb_iova =
        mtl_dma_map(m_handle, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova_map_sz);
    ASSERT_TRUE(test_ctx_tx[i]->ext_fb_iova != MTL_BAD_IOVA);
    info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_tx[i]->ext_fb);

    for (int j = 0; j < test_ctx_tx[i]->fb_cnt; j++) {
      test_ctx_tx[i]->ext_frames[j].buf_addr = test_ctx_tx[i]->ext_fb + j * frame_size;
      test_ctx_tx[i]->ext_frames[j].buf_iova =
          test_ctx_tx[i]->ext_fb_iova + j * frame_size;
      test_ctx_tx[i]->ext_frames[j].buf_len = frame_size;
    }

    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)test_ctx_tx[i]->ext_fb + frame * frame_size;

      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }

    test_ctx_tx[i]->handle = tx_handle[i]; /* all ready now */
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;

    test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
        sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
    size_t frame_size = st20_frame_size(fmt[i], width[i], height[i]);
    size_t pg_sz = mtl_page_size(m_handle);
    size_t fb_size = frame_size * test_ctx_rx[i]->fb_cnt;
    test_ctx_rx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
    size_t fb_size_malloc = test_ctx_rx[i]->ext_fb_iova_map_sz + pg_sz;
    test_ctx_rx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
    ASSERT_TRUE(test_ctx_rx[i]->ext_fb_malloc != NULL);
    test_ctx_rx[i]->ext_fb =
        (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_rx[i]->ext_fb_malloc, pg_sz);
    test_ctx_rx[i]->ext_fb_iova =
        mtl_dma_map(m_handle, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova_map_sz);
    info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_rx[i]->ext_fb);
    ASSERT_TRUE(test_ctx_rx[i]->ext_fb_iova != MTL_BAD_IOVA);

    for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
      test_ctx_rx[i]->ext_frames[j].buf_addr = test_ctx_rx[i]->ext_fb + j * frame_size;
      test_ctx_rx[i]->ext_frames[j].buf_iova =
          test_ctx_rx[i]->ext_fb_iova + j * frame_size;
      test_ctx_rx[i]->ext_frames[j].buf_len = frame_size;
    }

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_ext_frame_digest_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (dynamic) {
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      ops_rx.query_ext_frame = rx_query_ext_frame;
    } else {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma) {
      EXPECT_TRUE(dma_enabled);
    } else {
      EXPECT_FALSE(dma_enabled);
    }
    struct st_queue_meta meta;
    ret = st20_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    rtp_thread_rx[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);

    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }

    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    mtl_dma_unmap(m_handle, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova,
                  test_ctx_tx[i]->ext_fb_iova_map_sz);
    mtl_dma_unmap(m_handle, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova,
                  test_ctx_rx[i]->ext_fb_iova_map_sz);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, ext_frame_digest_frame_1080p_fps59_94_s1) {
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_digest20_field_1080p_fps59_94_s1) {
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_digest_frame_720p_fps59_94_s1_gpm) {
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, ext_frame_s3_2) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  bool interlaced[3] = {true, false, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_12BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_8BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, dynamic_ext_frame_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1920};
  int height[3] = {720, 720, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3, true);
}

static void st20_tx_user_pacing_test(int width[], int height[], enum st20_fmt fmt[],
                                     bool user_pacing[], bool user_timestamp[],
                                     enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> rx_framerate;
  std::vector<double> tx_framerate;
  std::vector<std::thread> sha_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  rx_framerate.resize(sessions);
  tx_framerate.resize(sessions);
  sha_thread_rx.resize(sessions);

  enum st_fps fps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    if (user_pacing[i])
      expect_framerate[i] = st_frame_rate(fps) / 2;
    else
      expect_framerate[i] = st_frame_rate(fps);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    test_ctx_tx[i]->user_pacing = user_pacing[i];
    test_ctx_tx[i]->user_timestamp = user_timestamp[i];

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_timestamp_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = false;
    ops_tx.fps = fps;
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame_timestamp;
    ops_tx.notify_frame_done = tx_notify_timestamp_frame_done;
    if (user_pacing[i]) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
    if (user_timestamp[i]) ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;
    test_ctx_rx[i]->user_pacing = user_pacing[i];
    test_ctx_rx[i]->user_timestamp = user_timestamp[i];

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_timestamp_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps;
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    sha_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    rx_framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    tx_framerate[i] = test_ctx_tx[i]->fb_send / time_sec;
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    sha_thread_rx[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);

    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         rx_framerate[i]);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         tx_framerate[i]);

    EXPECT_NEAR(tx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(rx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_tx, tx_user_pacing) {
  int width[3] = {1280, 1920, 1280};
  int height[3] = {720, 1080, 720};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  bool user_pacing[3] = {false, true, true};
  bool user_timestamp[3] = {true, false, true};
  st20_tx_user_pacing_test(width, height, fmt, user_pacing, user_timestamp,
                           ST_TEST_LEVEL_MANDATORY, 3);
}

static void st20_linesize_digest_test(enum st20_packing packing[], enum st_fps fps[],
                                      int width[], int height[], int linesize[],
                                      bool interlaced[], enum st20_fmt fmt[],
                                      bool check_fps, enum st_test_level level,
                                      int sessions = 1, bool ext = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext test as it's PA iova mode\n", __func__);
      return;
    }
  }

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> sha_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  sha_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_sha = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_linesize_digest_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.linesize = linesize[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;

    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (ext) {
      ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops_tx.get_next_frame =
          interlaced[i] ? tx_next_ext_video_field : tx_next_ext_video_frame;
      ops_tx.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;

    size_t fb_size = frame_size;
    if (linesize[i] > test_ctx_tx[i]->stride) {
      test_ctx_tx[i]->stride = linesize[i];
      fb_size = (size_t)linesize[i] * height[i];
      if (interlaced[i]) fb_size = fb_size >> 1;
    }
    test_ctx_tx[i]->fb_size = fb_size;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), fb_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    if (ext) {
      test_ctx_tx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_tx[i]->ext_frames) * test_ctx_tx[i]->fb_cnt);
      size_t fbs_size = fb_size * test_ctx_tx[i]->fb_cnt;
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(m_handle, fbs_size);
      ASSERT_TRUE(dma_mem != NULL);
      test_ctx_tx[i]->dma_mem = dma_mem;

      for (int j = 0; j < test_ctx_tx[i]->fb_cnt; j++) {
        test_ctx_tx[i]->ext_frames[j].buf_addr =
            (uint8_t*)mtl_dma_mem_addr(dma_mem) + j * fb_size;
        test_ctx_tx[i]->ext_frames[j].buf_iova = mtl_dma_mem_iova(dma_mem) + j * fb_size;
        test_ctx_tx[i]->ext_frames[j].buf_len = fb_size;
      }
    }

    uint8_t* fb;
    int total_lines = height[i];
    size_t bytes_per_line = (size_t)ops_tx.width / st20_pg.coverage * st20_pg.size;
    if (interlaced[i]) total_lines /= 2;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      if (ext) {
        fb = (uint8_t*)test_ctx_tx[i]->ext_frames[frame].buf_addr;
      } else {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      }

      ASSERT_TRUE(fb != NULL);

      for (int line = 0; line < total_lines; line++) {
        st_test_rand_data(fb + test_ctx_tx[i]->stride * line, bytes_per_line, frame);
      }
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, fb_size, result);
      test_sha_dump("st20_rx", result);
    }

    test_ctx_tx[i]->handle = tx_handle[i]; /* all ready now */
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_sha = true;

    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->fb_size;
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;

    if (ext) {
      test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
      size_t fbs_size = test_ctx_rx[i]->fb_size * test_ctx_rx[i]->fb_cnt;
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(m_handle, fbs_size);
      ASSERT_TRUE(dma_mem != NULL);
      test_ctx_rx[i]->dma_mem = dma_mem;

      for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
        test_ctx_rx[i]->ext_frames[j].buf_addr =
            (uint8_t*)mtl_dma_mem_addr(dma_mem) + j * test_ctx_rx[i]->fb_size;
        test_ctx_rx[i]->ext_frames[j].buf_iova =
            mtl_dma_mem_iova(dma_mem) + j * test_ctx_rx[i]->fb_size;
        test_ctx_rx[i]->ext_frames[j].buf_len = test_ctx_rx[i]->fb_size;
      }
    }

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_linesize_digest_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 10000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.linesize = linesize[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (ext) ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    test_ctx_rx[i]->width = ops_rx.width;
    test_ctx_rx[i]->height = ops_rx.height;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      sha_check[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      sha_check[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma) {
      EXPECT_TRUE(dma_enabled);
    } else {
      EXPECT_FALSE(dma_enabled);
    }
    struct st_queue_meta meta;
    ret = st20_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    sha_check[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);

    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }

    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    if (ext) {
      mtl_dma_mem_free(m_handle, test_ctx_tx[i]->dma_mem);
      mtl_dma_mem_free(m_handle, test_ctx_rx[i]->dma_mem);
    }
    tests_context_unit(test_ctx_tx[i]);
    tests_context_unit(test_ctx_rx[i]);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, linesize_digest_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {false, true, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, linesize_digest_crosslines_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, linesize_digest_ext_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3, true);
}