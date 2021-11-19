/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <thread>

#include "log.h"
#include "tests.h"

static int tx_video_build_rtp_packet(struct tests_context* s,
                                     struct st20_rfc4175_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  uint16_t data_len = s->pkt_data_len;
  int pkts_in_line = s->pkts_in_line;
  int row_number = s->pkt_idx / pkts_in_line;
  int pixels_in_pkt = s->pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
  int row_offset = pixels_in_pkt * (s->pkt_idx % pkts_in_line);

  /* update hdr */
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = 96;
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->tmstamp = htonl(s->rtp_tmstamp);
  rtp->seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->seq_id++;
  int temp = (s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  data_len = s->pkt_data_len > temp ? temp : s->pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = (struct tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
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

    st20_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (struct tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int rx_rtp_ready(void* args) {
  auto ctx = (struct tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void rx_get_packet(void* args) {
  auto ctx = (struct tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st20_rfc4175_rtp_hdr* hdr;
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    if (hdr->marker) ctx->fb_rec++;
    st20_rx_put_mbuf(ctx->handle, mbuf);
  }
}

static int rx_frame_ready(void* priv, void* frame) {
  auto ctx = (struct tests_context*)priv;
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void st20_tx_ops_init(struct tests_context* st20, struct st20_tx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st20->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st20->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->get_next_frame = tx_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void st20_rx_ops_init(struct tests_context* st20, struct st20_rx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st20->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st20->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->notify_frame_ready = rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st20_tx_assert_cnt(int expect_s20_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st20_tx_sessions_cnt, expect_s20_tx_cnt);
}

static void st20_rx_assert_cnt(int expect_s20_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st20_rx_sessions_cnt, expect_s20_rx_cnt);
}

TEST(St20_tx, create_free_single) { create_free_test(st20_tx, 0, 1, 1); }
TEST(St20_tx, create_free_multi) { create_free_test(st20_tx, 0, 1, 6); }
TEST(St20_tx, create_free_mix) { create_free_test(st20_tx, 2, 3, 4); }
TEST(St20_tx, create_free_max) { create_free_max(st20_tx, 100); }
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

TEST(St20_rx, create_free_single) { create_free_test(st20_rx, 0, 1, 1); }
TEST(St20_rx, create_free_multi) { create_free_test(st20_rx, 0, 1, 6); }
TEST(St20_rx, create_free_mix) { create_free_test(st20_rx, 2, 3, 4); }
TEST(St20_rx, create_free_max) { create_free_max(st20_rx, 100); }
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

static void rtp_tx_specific_init(struct st20_tx_ops* ops,
                                 struct tests_context* test_ctx) {
  int ret;
  ret = st20_get_pgroup(ops->fmt, &test_ctx->st20_pg);
  ASSERT_TRUE(ret == 0);
  /* calculate pkts in line for rtp */
  size_t bytes_in_pkt = ST_PKT_MAX_RTP_BYTES - sizeof(struct st20_rfc4175_rtp_hdr);
  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * test_ctx->st20_pg.size / test_ctx->st20_pg.coverage;

  int pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
  test_ctx->total_pkts_in_frame = ops->height * pkts_in_line;
  test_ctx->pkt_idx = 0;
  test_ctx->seq_id = 1;
  test_ctx->pkts_in_line = pkts_in_line;
  test_ctx->width = ops->width;
  int pixels_in_pkts = (ops->width + pkts_in_line - 1) / pkts_in_line;
  int pkt_data_len = (pixels_in_pkts + test_ctx->st20_pg.coverage - 1) /
                     test_ctx->st20_pg.coverage * test_ctx->st20_pg.size;
  ops->rtp_frame_total_pkts = test_ctx->total_pkts_in_frame;
  ops->rtp_pkt_size = pkt_data_len + sizeof(struct st20_rfc4175_rtp_hdr);
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
  test_ctx->pkt_data_len = pkt_data_len;
}

static void st20_tx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops;
  struct tests_context* test_ctx[sessions];
  st20_tx_handle handle[sessions];
  double expect_framerate[sessions];
  double framerate[sessions];
  std::thread rtp_thread[sessions];

  for (int i = 0; i < sessions; i++) {
    switch (fps[i]) {
      case ST_FPS_P59_94:
        expect_framerate[i] = 59.94;
        break;
      case ST_FPS_P50:
        expect_framerate[i] = 50;
        break;
      case ST_FPS_P29_97:
        expect_framerate[i] = 29.97;
        break;
      default:
        expect_framerate[i] = 59.94;
        break;
    }
    test_ctx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
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
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);
    test_ctx[i]->handle = handle[i];
    ASSERT_TRUE(handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  int second = ctx->para.num_ports == 2 ? 10 : 5;
  sleep(second);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = true;
      rtp_thread[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx[i]->fb_send > 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx[i]);
  }
}

static void st20_rx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
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

  struct tests_context* test_ctx_tx[sessions];
  struct tests_context* test_ctx_rx[sessions];
  st20_tx_handle tx_handle[sessions];
  st20_rx_handle rx_handle[sessions];
  double expect_framerate[sessions];
  double framerate[sessions];
  std::thread rtp_thread_tx[sessions];
  std::thread rtp_thread_rx[sessions];

  for (int i = 0; i < sessions; i++) {
    switch (fps[i]) {
      case ST_FPS_P59_94:
        expect_framerate[i] = 59.94;
        break;
      case ST_FPS_P50:
        expect_framerate[i] = 50;
        break;
      case ST_FPS_P29_97:
        expect_framerate[i] = 29.97;
        break;
      default:
        expect_framerate[i] = 59.94;
        break;
    }
    test_ctx_tx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      test_ctx_rx[i]->stop = true;
      rtp_thread_tx[i].join();
      rtp_thread_rx[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx_tx[i]);
    st_test_free(test_ctx_rx[i]);
  }
}

TEST(St20_tx, frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_720p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_yuv422_8bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_8BIT);
}
TEST(St20_tx, frame_1080p_yuv420_10bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT);
}
TEST(St20_tx, frame_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_720p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_1080p_fps50_fps29_97) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_tx, frame_1080p_fps50_fps59_94) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps29_97_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps59_94_fp50) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps29_97_720p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_720p_fps59_94_1080p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_720p_fps59_94_4k_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 3840};
  int height[2] = {720, 2160};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
