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

static int st22_tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void st22_tx_ops_init(tests_context* st22, struct st22_tx_ops* ops) {
  auto ctx = st22->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st22->idx;
  if (ops->num_port > 1) {
    memcpy(ops->dip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;
  ops->notify_rtp_done = st22_tx_rtp_done;
  ops->rtp_ring_size = 1024;
  ops->rtp_pkt_size = 1280 + sizeof(struct st_rfc3550_rtp_hdr);
}

static void st22_rx_ops_init(tests_context* st22, struct st22_rx_ops* ops) {
  auto ctx = st22->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st22->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;
  ops->notify_rtp_ready = st22_rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st22_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22_tx, create_free_single) { create_free_test(st22_tx, 0, 1, 1); }
TEST(St22_tx, create_free_multi) { create_free_test(st22_tx, 0, 1, 6); }
TEST(St22_tx, create_free_mix) { create_free_test(st22_tx, 2, 3, 4); }
TEST(St22_tx, create_free_max) { create_free_max(st22_tx, 100); }
TEST(St22_tx, create_expect_fail) { expect_fail_test(st22_tx); }
TEST(St22_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring_2(st22_tx, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring_2(st22_tx, ring_size);
}
TEST(St22_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size_2(st22_tx, rtp_pkt_size, false);
  rtp_pkt_size = ST_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size_2(st22_tx, rtp_pkt_size, true);
  rtp_pkt_size = ST_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size_2(st22_tx, rtp_pkt_size, false);
}

TEST(St22_rx, create_free_single) { create_free_test(st22_rx, 0, 1, 1); }
TEST(St22_rx, create_free_multi) { create_free_test(st22_rx, 0, 1, 6); }
TEST(St22_rx, create_free_mix) { create_free_test(st22_rx, 2, 3, 4); }
TEST(St22_rx, create_free_max) { create_free_max(st22_rx, 100); }
TEST(St22_rx, create_expect_fail) { expect_fail_test(st22_rx); }
TEST(St22_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring_2(st22_rx, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring_2(st22_rx, ring_size);
}

static int st22_tx_build_rtp_packet(tests_context* s, struct st_rfc3550_rtp_hdr* rtp,
                                    uint16_t* pkt_len) {
  /* update hdr */
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = 96;
  rtp->tmstamp = htonl(s->rtp_tmstamp);
  rtp->seq_number = htons(s->seq_id);
  s->seq_id++;

  uint16_t data_len = s->pkt_data_len;
  *pkt_len = data_len + sizeof(*rtp);
  /* todo: build the payload data */
  if (s->check_md5) {
    uint8_t* payload = (uint8_t*)rtp + sizeof(struct st_rfc3550_rtp_hdr);
    st_memcpy(payload,
              s->frame_buf[s->fb_idx % TEST_MD5_HIST_NUM] + s->pkt_idx * data_len,
              data_len);
  }

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->marker = 1;
    s->fb_idx++;
    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void st22_tx_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st22_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st22_tx_get_mbuf(ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    st22_tx_build_rtp_packet(ctx, (struct st_rfc3550_rtp_hdr*)usrptr, &mbuf_len);

    st22_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
  }
}

static void st22_rx_handle_rtp(tests_context* s, struct st_rfc3550_rtp_hdr* hdr,
                               bool newframe, int mbuf_len) {
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
  int index = ntohs(hdr->seq_number) - s->seq_id;
  if (index < 0) {
    index = index + 0x10000;
  }
  st_memcpy(frame + index * (mbuf_len - sizeof(*hdr)), payload, mbuf_len - sizeof(*hdr));
  return;
}

static void st22_rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st_rfc3550_rtp_hdr* hdr;
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st22_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st22_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st_rfc3550_rtp_hdr*)usrptr;
    bool newframe = false;
    int32_t tmstamp = ntohl(hdr->tmstamp);
    if (tmstamp != ctx->rtp_tmstamp) {
      /* new frame received */
      ctx->rtp_tmstamp = tmstamp;
      ctx->fb_rec++;
      newframe = true;
      ctx->seq_id = ntohs(hdr->seq_number);
    }
    if (ctx->check_md5) {
      st22_rx_handle_rtp(ctx, hdr, newframe, mbuf_len);
    }
    st22_rx_put_mbuf(ctx->handle, mbuf);
  }
}

static void st22_rx_fps_test(enum st_fps fps[], int width[], int height[],
                             int pkt_data_len[], int total_pkts[], int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
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

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 15000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */

    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st_rfc3550_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st22_tx_feed_packet, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 15000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->stop = false;
    rtp_thread_rx[i] = std::thread(st22_rx_get_packet, test_ctx_rx[i]);
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

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

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_rx, rtp_1080p_fps59_94_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  int pkt_data_len[1] = {1280};
  int total_pkts[1] = {540};
  st22_rx_fps_test(fps, width, height, pkt_data_len, total_pkts, 1);
}

TEST(St22_rx, rtp_mix_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  int pkt_data_len[2] = {1280, 1300};
  int total_pkts[2] = {540, 150};
  st22_rx_fps_test(fps, width, height, pkt_data_len, total_pkts, 2);
}

static void st22_rx_update_src_test(int tx_sessions) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
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
    ops_tx.name = "st22_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (2 == i)
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;

    test_ctx_tx[i]->pkt_data_len = 1280;
    test_ctx_tx[i]->total_pkts_in_frame = 520; /* compress ratio 1/8, 4320/8 */

    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st_rfc3550_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st22_tx_feed_packet, test_ctx_tx[i]);
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->stop = false;
    rtp_thread_rx[i] = std::thread(st22_rx_get_packet, test_ctx_rx[i]);
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(2);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[ST_PORT_P] = 10000 + 1;
  memcpy(src.sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  for (int i = 0; i < rx_sessions; i++) {
    ret = st22_rx_update_source(rx_handle[i], &src);
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

    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[ST_PORT_P] = 10000 + 2;
    memcpy(src.sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    for (int i = 0; i < rx_sessions; i++) {
      ret = st22_rx_update_source(rx_handle[i], &src);
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

      EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[ST_PORT_P] = 10000 + 0;
  memcpy(src.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  for (int i = 0; i < rx_sessions; i++) {
    ret = st22_rx_update_source(rx_handle[i], &src);
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

    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  /* stop rtp thread */
  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    rtp_thread_rx[i].join();
  }
  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
      test_ctx_tx[i]->cv.notify_all();
    }
    rtp_thread_tx[i].join();
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);

  /* free all tx and rx */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
  }
}

TEST(St22_rx, update_source) { st22_rx_update_src_test(2); }

static void st22_rx_after_start_test(enum st_fps fps[], int width[], int height[],
                                     int pkt_data_len[], int total_pkts[], int sessions,
                                     int repeat, bool check_md5 = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> md5_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  md5_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 15000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */

    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st_rfc3550_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    uint8_t* fb;
    if (check_md5) {
      for (int frame = 0; frame < TEST_MD5_HIST_NUM; frame++) {
        size_t frame_size = total_pkts[i] * pkt_data_len[i];
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
        ASSERT_TRUE(fb != NULL);
        for (size_t s = 0; s < frame_size; s++) fb[s] = rand() % 0xFF;
        unsigned char* result = test_ctx_tx[i]->md5s[frame];
        MD5((unsigned char*)fb, frame_size, result);
        test_md5_dump("st22_rx", result);
      }
    }
    test_ctx_tx[i]->check_md5 = check_md5;
    test_ctx_tx[i]->frame_size = total_pkts[i] * pkt_data_len[i];

    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st22_tx_feed_packet, test_ctx_tx[i]);
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);

  for (int r = 0; r < repeat; r++) {
    /* create rx */
    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_rx[i] != NULL);

      test_ctx_rx[i]->idx = i;
      test_ctx_rx[i]->ctx = ctx;
      memset(&ops_rx, 0, sizeof(ops_rx));
      ops_rx.name = "st22_test";
      ops_rx.priv = test_ctx_rx[i];
      ops_rx.num_port = 1;
      memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
      strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
      ops_rx.udp_port[ST_PORT_P] = 15000 + i;
      ops_rx.pacing = ST21_PACING_NARROW;
      ops_rx.width = width[i];
      ops_rx.height = height[i];
      ops_rx.fps = fps[i];
      ops_rx.fmt = ST20_FMT_YUV_422_10BIT;

      ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;

      rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
      test_ctx_rx[i]->handle = rx_handle[i];
      ASSERT_TRUE(rx_handle[i] != NULL);

      test_ctx_rx[i]->stop = false;
      test_ctx_rx[i]->check_md5 = check_md5;
      test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
      test_ctx_rx[i]->frame_size = total_pkts[i] * pkt_data_len[i];
      memcpy(test_ctx_rx[i]->md5s, test_ctx_tx[i]->md5s,
             TEST_MD5_HIST_NUM * MD5_DIGEST_LENGTH);
      rtp_thread_rx[i] = std::thread(st22_rx_get_packet, test_ctx_rx[i]);
      if (check_md5) {
        md5_check[i] = std::thread(md5_frame_check, test_ctx_rx[i]);
      }
    }

    sleep(10);

    /* check fps, stop rx */
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
      if (check_md5) {
        md5_check[i].join();
        while (!test_ctx_rx[i]->buf_q.empty()) {
          void* frame = test_ctx_rx[i]->buf_q.front();
          st_test_free(frame);
          test_ctx_rx[i]->buf_q.pop();
        }
      }
    }
    for (int i = 0; i < sessions; i++) {
      EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      EXPECT_TRUE(test_ctx_rx[i]->fail_cnt < 2);  // the first frame may be incompleted
      ret = st22_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
    }
    sleep(2);
  }

  /* stop tx thread */
  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
      test_ctx_tx[i]->cv.notify_all();
    }
    rtp_thread_tx[i].join();
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    if (check_md5) {
      for (int frame = 0; frame < TEST_MD5_HIST_NUM; frame++) {
        if (test_ctx_tx[i]->frame_buf[frame])
          st_test_free(test_ctx_tx[i]->frame_buf[frame]);
        if (test_ctx_rx[i]->frame_buf[frame])
          st_test_free(test_ctx_rx[i]->frame_buf[frame]);
      }
    }
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_rx, after_start_rtp_mix_s2_r2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  int pkt_data_len[2] = {1280, 1300};
  int total_pkts[2] = {540, 150};
  st22_rx_after_start_test(fps, width, height, pkt_data_len, total_pkts, 2, 2);
}

TEST(St22_rx, digest_rtp_s1) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  int pkt_data_len[2] = {1280, 1300};
  int total_pkts[2] = {540, 150};
  st22_rx_after_start_test(fps, width, height, pkt_data_len, total_pkts, 2, 2, true);
}