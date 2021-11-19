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

static int tx_anc_build_rtp_packet(struct tests_context* s,
                                   struct st40_rfc8331_rtp_hdr* rtp, uint16_t* pkt_len) {
  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->marker = 1;
  rtp->anc_count = 0;
  rtp->payload_type = 113;
  rtp->version = 2;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->csrc_count = 0;
  rtp->f = 0b00;
  rtp->tmstamp = s->rtp_tmstamp;
  rtp->ssrc = htonl(0x88888888 + s->idx);
  /* update rtp seq*/
  rtp->seq_number = htons((uint16_t)s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->rtp_tmstamp++;
  s->seq_id++;
  *pkt_len = sizeof(struct st40_rfc8331_rtp_hdr);
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
    mbuf = st40_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st40_tx_get_mbuf(ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_anc_build_rtp_packet(ctx, (struct st40_rfc8331_rtp_hdr*)usrptr, &mbuf_len);
    st40_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (struct tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  ctx->fb_send++;
  return 0;
}

static int rx_rtp_ready(void* priv) {
  auto ctx = (struct tests_context*)priv;
  void* useptr;
  void* mbuf;
  uint16_t len;
  mbuf = st40_rx_get_mbuf((st40_rx_handle)ctx->handle, &useptr, &len);
  st40_rx_put_mbuf((st40_rx_handle)ctx->handle, mbuf);
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void st40_rx_ops_init(struct tests_context* st40, struct st40_rx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 30000 + st40->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 30000 + st40->idx;
  }
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st40_tx_ops_init(struct tests_context* st40, struct st40_tx_ops* ops) {
  auto ctx = st40->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st40_test";
  ops->priv = st40;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 30000 + st40->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 30000 + st40->idx;
  }
  ops->type = ST40_TYPE_FRAME_LEVEL;
  ops->fps = ST_FPS_P59_94;

  ops->framebuff_cnt = st40->fb_cnt;
  ops->get_next_frame = tx_next_frame;
  ops->rtp_ring_size = 1024;
  ops->notify_rtp_done = tx_rtp_done;
}

static void st40_tx_assert_cnt(int expect_s40_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st40_tx_sessions_cnt, expect_s40_tx_cnt);
}

static void st40_rx_assert_cnt(int expect_s40_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st40_rx_sessions_cnt, expect_s40_rx_cnt);
}

TEST(St40_tx, create_free_single) { create_free_test(st40_tx, 0, 1, 1); }
TEST(St40_tx, create_free_multi) { create_free_test(st40_tx, 0, 1, 6); }
TEST(St40_tx, create_free_mix) { create_free_test(st40_tx, 2, 3, 4); }
TEST(St40_tx, create_free_max) { create_free_max(st40_tx, 100); }
TEST(St40_tx, create_expect_fail) { expect_fail_test(st40_tx); }
TEST(St40_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st40_tx, ST40_TYPE_RTP_LEVEL, ring_size);
}
TEST(St40_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  test_get_framebuffer(st40_tx, fbcnt);
}
TEST(St40_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
  fbcnt = 1000;
  expect_fail_test_get_framebuffer(st40_tx, fbcnt);
}

TEST(St40_rx, create_free_single) { create_free_test(st40_rx, 0, 1, 1); }
TEST(St40_rx, create_free_multi) { create_free_test(st40_rx, 0, 1, 6); }
TEST(St40_rx, create_free_mix) { create_free_test(st40_rx, 2, 3, 4); }
TEST(St40_rx, create_free_max) { create_free_max(st40_rx, 100); }
TEST(St40_rx, create_expect_fail) { expect_fail_test(st40_rx); }
TEST(St40_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring_2(st40_rx, ring_size);
}

static void st40_tx_fps_test(enum st40_type type[], enum st_fps fps[], int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops;
  struct tests_context* test_ctx[sessions];
  st40_tx_handle handle[sessions];
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
    st40_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];

    handle[i] = st40_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);
    test_ctx[i]->handle = handle[i];
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
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
    ret = st40_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx[i]);
  }
}

static void st40_rx_fps_test(enum st40_type type[], enum st_fps fps[], int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st40_tx_ops ops_tx;
  struct st40_rx_ops ops_rx;
  std::thread rtp_thread_tx[sessions];
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  struct tests_context* test_ctx_tx[sessions];
  struct tests_context* test_ctx_rx[sessions];
  st40_tx_handle tx_handle[sessions];
  st40_rx_handle rx_handle[sessions];
  double expect_framerate[sessions];
  double framerate[sessions];

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
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

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st40_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 30000 + i;
    ops_tx.type = type[i];
    ops_tx.fps = fps[i];
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.notify_rtp_done = tx_rtp_done;

    tx_handle[i] = st40_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
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
    ops_rx.name = "st40_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 30000 + i;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st40_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (type[i] == ST40_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      rtp_thread_tx[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st40_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st40_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx_tx[i]);
    st_test_free(test_ctx_rx[i]);
  }
}

TEST(St40_tx, frame_fps59_94_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  st40_tx_fps_test(type, fps);
}
TEST(St40_tx, frame_fps29_97_s1) {
  enum st40_type type[1] = {ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  st40_tx_fps_test(type, fps);
}
TEST(St40_tx, frame_fps50_s1) {
  enum st40_type type[1] = {ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  st40_tx_fps_test(type, fps);
}
TEST(St40_tx, frame_fps59_94_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, 3);
}
TEST(St40_tx, frame_fps29_97_s3) {
  enum st40_type type[3] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, 3);
}
TEST(St40_tx, frame_fps50_s3) {
  enum st40_type type[3] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL,
                            ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  st40_tx_fps_test(type, fps, 3);
}

TEST(St40_tx, frame_fps50_fps29_97) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  st40_tx_fps_test(type, fps, 2);
}
TEST(St40_tx, frame_fps50_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, 2);
}
TEST(St40_tx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_FRAME_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, 2);
}
TEST(St40_rx, frame_fps29_97_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  st40_rx_fps_test(type, fps, 2);
}
TEST(St40_rx, frame_fps50_fps59_94) {
  enum st40_type type[2] = {ST40_TYPE_RTP_LEVEL, ST40_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  st40_tx_fps_test(type, fps, 2);
}