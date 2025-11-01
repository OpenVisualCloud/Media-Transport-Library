/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST22_TEST_PAYLOAD_TYPE (114)

static int st22_tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;

  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_next_video_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st22_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  meta->codestream_size = ctx->frame_size;
  dbg("%s, next_frame_idx %d frame_size %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->codestream_size);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                           struct st22_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  meta->codestream_size = ctx->frame_size;
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = mtl_ptp_read_time(ctx->ctx->handle) + 35 * 1000 * 1000;
  dbg("%s, next_frame_idx %d frame_size %" PRIu64 "\n", __func__, *next_frame_idx,
      meta->codestream_size);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int st22_frame_done(void* priv, uint16_t frame_idx,
                           struct st22_tx_frame_meta* meta) {
  return 0;
}

static int st22_rx_frame_ready(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  ctx->fb_rec++;
  if (!ctx->start_time) {
    ctx->rtp_delta = meta->timestamp - ctx->rtp_tmstamp;
    ctx->start_time = st_test_get_monotonic_time();
  }

  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) ctx->rtp_tmstamp = meta->timestamp;
  st22_rx_put_framebuff((st22_rx_handle)ctx->handle, frame);
  return 0;
}

static void st22_tx_ops_init(tests_context* st22, struct st22_tx_ops* ops) {
  auto ctx = st22->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st22->idx;
  if (ops->num_port > 1) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->notify_rtp_done = st22_tx_rtp_done;
  ops->type = ST22_TYPE_FRAME_LEVEL;
  ops->rtp_ring_size = 1024;
  ops->rtp_pkt_size = 1280 + sizeof(struct st22_rfc9134_rtp_hdr);
  ops->framebuff_cnt = st22->fb_cnt;
  ops->framebuff_max_size = 0x100000;
  ops->notify_frame_done = st22_frame_done;
  ops->get_next_frame = st22_next_video_frame;
}

static void st22_rx_ops_init(tests_context* st22, struct st22_rx_ops* ops) {
  auto ctx = st22->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st22_test";
  ops->priv = st22;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 10000 + st22->idx;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_PORT_R] = 10000 + st22->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->payload_type = ST22_TEST_PAYLOAD_TYPE;
  ops->notify_rtp_ready = st22_rx_rtp_ready;
  ops->rtp_ring_size = 1024;
  ops->type = ST22_TYPE_FRAME_LEVEL;
  ops->framebuff_cnt = st22->fb_cnt;
  ops->framebuff_max_size = 0x100000;
  ops->notify_frame_ready = st22_rx_frame_ready;
}

static void st22_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22_tx, create_free_single) {
  create_free_test(st22_tx, 0, 1, 1);
}
TEST(St22_tx, create_free_multi) {
  create_free_test(st22_tx, 0, 1, 6);
}
TEST(St22_tx, create_free_mix) {
  create_free_test(st22_tx, 2, 3, 4);
}
TEST(St22_tx, create_free_max) {
  create_free_max(st22_tx, TEST_CREATE_FREE_MAX);
}
TEST(St22_tx, create_expect_fail) {
  expect_fail_test(st22_tx);
}
TEST(St22_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st22_tx, ST22_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st22_tx, ST22_TYPE_RTP_LEVEL, ring_size);
}
TEST(St22_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size(st22_tx, ST22_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}
TEST(St22_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st22_tx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st22_tx, fbcnt);
}

TEST(St22_rx, create_free_single) {
  create_free_test(st22_rx, 0, 1, 1);
}
TEST(St22_rx, create_free_multi) {
  create_free_test(st22_rx, 0, 1, 6);
}
TEST(St22_rx, create_free_mix) {
  create_free_test(st22_rx, 2, 3, 4);
}
TEST(St22_rx, create_free_max) {
  create_free_max(st22_rx, TEST_CREATE_FREE_MAX);
}
TEST(St22_rx, create_expect_fail) {
  expect_fail_test(st22_rx);
}
TEST(St22_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st22_rx, ST22_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st22_rx, ST22_TYPE_RTP_LEVEL, ring_size);
}
TEST(St22_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st22_rx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st22_rx, fbcnt);
}

static int st22_tx_build_rtp_packet(tests_context* s, struct st22_rfc9134_rtp_hdr* rtp,
                                    uint16_t* pkt_len) {
  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = ST22_TEST_PAYLOAD_TYPE;
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  rtp->base.seq_number = htons(s->seq_id);
  s->seq_id++;

  uint16_t data_len = s->pkt_data_len;
  *pkt_len = data_len + sizeof(*rtp);
  /* todo: build the payload data */
  if (s->check_sha) {
    uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
    mtl_memcpy(payload,
               s->frame_buf[s->fb_idx % ST22_TEST_SHA_HIST_NUM] + s->pkt_idx * data_len,
               data_len);
  }

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->base.marker = 1;
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
    mbuf = st22_tx_get_mbuf((st22_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st22_tx_get_mbuf((st22_tx_handle)ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    st22_tx_build_rtp_packet(ctx, (struct st22_rfc9134_rtp_hdr*)usrptr, &mbuf_len);

    st22_tx_put_mbuf((st22_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static void st22_rx_handle_rtp(tests_context* s, struct st22_rfc9134_rtp_hdr* hdr,
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
  int index = ntohs(hdr->base.seq_number) - s->seq_id;
  if (index < 0) {
    index = index + 0x10000;
  }
  mtl_memcpy(frame + index * (mbuf_len - sizeof(*hdr)), payload, mbuf_len - sizeof(*hdr));
  return;
}

static void st22_rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st22_rfc9134_rtp_hdr* hdr;
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st22_rx_get_mbuf((st22_rx_handle)ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st22_rx_get_mbuf((st22_rx_handle)ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st22_rfc9134_rtp_hdr*)usrptr;
    bool newframe = false;
    uint32_t tmstamp = ntohl(hdr->base.tmstamp);
    if (tmstamp != ctx->rtp_tmstamp) {
      /* new frame received */
      ctx->rtp_tmstamp = tmstamp;
      ctx->fb_rec++;
      newframe = true;
      ctx->seq_id = ntohs(hdr->base.seq_number);
    }
    if (ctx->check_sha) {
      st22_rx_handle_rtp(ctx, hdr, newframe, mbuf_len);
    }
    st22_rx_put_mbuf((st22_rx_handle)ctx->handle, mbuf);
  }
}

static void st22_rx_fps_test(enum st22_type type[], enum st_fps fps[], int width[],
                             int height[], int pkt_data_len[], int total_pkts[],
                             enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level lower than global */
  if (level < ctx->level) return;

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
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_tx.type = type[i];
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_tx[i]->frame_size =
        (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_tx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;
    ops_tx.notify_frame_done = st22_frame_done;
    ops_tx.get_next_frame = st22_next_video_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);

    ASSERT_TRUE(tx_handle[i] != NULL);
    if (type[i] == ST22_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(st22_tx_feed_packet, test_ctx_tx[i]);
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

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_rx.type = type[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_rx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_rx[i]->frame_size =
        (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_rx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_rx.notify_frame_ready = st22_rx_frame_ready;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);

    if (type[i] == ST22_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(st22_rx_get_packet, test_ctx_rx[i]);
    }

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    if (type[i] == ST22_TYPE_RTP_LEVEL) {
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
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_rx, fps_1080p_s2) {
  enum st22_type type[2] = {ST22_TYPE_FRAME_LEVEL, ST22_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  int pkt_data_len[2] = {1280, 1280};
  int total_pkts[2] = {546, 540};
  st22_rx_fps_test(type, fps, width, height, pkt_data_len, total_pkts,
                   ST_TEST_LEVEL_MANDATORY, 2);
}

TEST(St22_rx, fps_mix_s2) {
  enum st22_type type[2] = {ST22_TYPE_RTP_LEVEL, ST22_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  int pkt_data_len[2] = {1280, 1300};
  int total_pkts[2] = {540, 150};
  st22_rx_fps_test(type, fps, width, height, pkt_data_len, total_pkts, ST_TEST_LEVEL_ALL,
                   2);
}

static void st22_rx_update_src_test(int tx_sessions, enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level lower than global */
  if (level < ctx->level) return;

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  expect_framerate.resize(rx_sessions);
  framerate.resize(rx_sessions);

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
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;

    test_ctx_tx[i]->pkt_data_len = 1280;
    test_ctx_tx[i]->total_pkts_in_frame = 520; /* compress ratio 1/8, 4320/8 */
    test_ctx_tx[i]->frame_size =
        (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_tx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;

    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;
    ops_tx.notify_frame_done = st22_frame_done;
    ops_tx.get_next_frame = st22_next_video_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
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
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_rx.type = ST22_TYPE_FRAME_LEVEL;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    test_ctx_rx[i]->pkt_data_len = 1280;
    test_ctx_rx[i]->total_pkts_in_frame = 520; /* compress ratio 1/8, 4320/8 */
    test_ctx_rx[i]->frame_size =
        (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_rx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_rx.notify_frame_ready = st22_rx_frame_ready;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 10000 + 1;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
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

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 10000 + 2;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
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

      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 10000 + 0;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
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

    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }

  ret = mtl_stop(m_handle);
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

TEST(St22_rx, update_source) {
  st22_rx_update_src_test(2, ST_TEST_LEVEL_ALL);
}

static void st22_rx_after_start_test(enum st_fps fps[], int width[], int height[],
                                     int pkt_data_len[], int total_pkts[], int sessions,
                                     int repeat) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(1);

  for (int r = 0; r < repeat; r++) {
    /* create tx */
    for (int i = 0; i < sessions; i++) {
      expect_framerate[i] = st_frame_rate(fps[i]);
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
      if (ctx->mcast_only)
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
               MTL_IP_ADDR_LEN);
      snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_P]);
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
      ops_tx.pacing = ST21_PACING_NARROW;
      ops_tx.width = width[i];
      ops_tx.height = height[i];
      ops_tx.fps = fps[i];
      ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
      ops_tx.type = ST22_TYPE_FRAME_LEVEL;
      ops_tx.pack_type = ST22_PACK_CODESTREAM;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;

      test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
      test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i];
      test_ctx_tx[i]->frame_size =
          (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

      /* set max to 100 extra */
      ops_tx.framebuff_max_size =
          test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;

      ops_tx.notify_rtp_done = st22_tx_rtp_done;
      ops_tx.rtp_ring_size = 1024;
      ops_tx.rtp_pkt_size =
          test_ctx_tx[i]->pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
      ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;
      ops_tx.notify_frame_done = st22_frame_done;
      ops_tx.get_next_frame = st22_next_video_frame;

      tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
      test_ctx_tx[i]->handle = tx_handle[i];
      ASSERT_TRUE(tx_handle[i] != NULL);
    }
    /* create rx */
    for (int i = 0; i < sessions; i++) {
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
      if (ctx->mcast_only)
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      else
        memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_R]);
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
      ops_rx.pacing = ST21_PACING_NARROW;
      ops_rx.width = width[i];
      ops_rx.height = height[i];
      ops_rx.fps = fps[i];
      ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
      ops_rx.type = ST22_TYPE_FRAME_LEVEL;
      ops_rx.pack_type = ST22_PACK_CODESTREAM;
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;

      ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;

      test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
      test_ctx_rx[i]->total_pkts_in_frame =
          total_pkts[i]; /* compress ratio 1/8, 4320/8 */
      test_ctx_rx[i]->frame_size =
          (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

      /* set max to 100 extra */
      ops_rx.framebuff_max_size =
          test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
      ops_rx.notify_frame_ready = st22_rx_frame_ready;

      rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
      test_ctx_rx[i]->handle = rx_handle[i];
      ASSERT_TRUE(rx_handle[i] != NULL);
    }

    sleep(10);

    /* check fps, stop rx */
    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    }
    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      EXPECT_LT(test_ctx_rx[i]->sha_fail_cnt,
                2);  // the first frame may be incomplete
      ret = st22_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
    }
    for (int i = 0; i < sessions; i++) {
      ret = st22_tx_free(tx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_tx[i];
      delete test_ctx_rx[i];
    }
    sleep(1);
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
}

TEST(St22_rx, after_start_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  int pkt_data_len[2] = {1280, 1300};
  int total_pkts[2] = {540, 150};
  st22_rx_after_start_test(fps, width, height, pkt_data_len, total_pkts, 2, 2);
}

static void st22_rx_dump_test(enum st_fps fps[], int width[], int height[],
                              int pkt_data_len[], int total_pkts[], int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  if (!mtl_pmd_is_dpdk_based(m_handle, MTL_PORT_R)) {
    info("%s, MTL_PORT_R is not a DPDK based PMD, skip this case\n", __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
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
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_tx[i]->frame_size =
        (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_tx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;

    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;
    ops_tx.notify_frame_done = st22_frame_done;
    ops_tx.get_next_frame = st22_next_video_frame;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
  }

  for (int i = 0; i < sessions; i++) {
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
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_rx.type = ST22_TYPE_FRAME_LEVEL;
    ops_rx.pack_type = ST22_PACK_CODESTREAM;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;

    test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_rx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_rx[i]->frame_size =
        (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_rx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_rx.notify_frame_ready = st22_rx_frame_ready;

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  sleep(5);
  uint32_t max_dump_packets = 100;
  for (int i = 0; i < sessions; i++) {
    struct st_pcap_dump_meta meta;
    ret = st22_rx_pcapng_dump(rx_handle[i], max_dump_packets, true, &meta);
    EXPECT_GE(ret, 0);
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], max_dump_packets);
    dbg("%s, file_name %s\n", __func__, meta.file_name[MTL_SESSION_PORT_P]);
    if (ret >= 0) remove(meta.file_name[MTL_SESSION_PORT_P]);
  }

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_rx, pcap_dump) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  int pkt_data_len[1] = {1280};
  int total_pkts[1] = {540};
  st22_rx_dump_test(fps, width, height, pkt_data_len, total_pkts, 1);
}

static int st22_digest_rx_frame_ready(void* priv, void* frame,
                                      struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  if (meta->frame_total_size != ctx->frame_size) {
    ctx->incomplete_frame_cnt++;
    st22_rx_put_framebuff((st22_rx_handle)ctx->handle, frame);
    return 0;
  }

  std::unique_lock<std::mutex> lck(ctx->mtx);
  if (ctx->buf_q.empty()) {
    ctx->buf_q.push(frame);
    ctx->cv.notify_all();
  } else {
    st22_rx_put_framebuff((st22_rx_handle)ctx->handle, frame);
  }
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  dbg("%s, frame %p\n", __func__, frame);
  return 0;
}

static void st22_digest_rx_frame_check(void* args) {
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
      SHA256((unsigned char*)frame, ctx->frame_size, result);
      for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
        unsigned char* target_sha = ctx->shas[i];
        if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= ST22_TEST_SHA_HIST_NUM) {
        test_sha_dump("st22_rx_error_sha", result);
        ctx->sha_fail_cnt++;
      }
      ctx->check_sha_frame_cnt++;
      st22_rx_put_framebuff((st22_rx_handle)ctx->handle, frame);
    }
  }
}

/* only frame level */
static void st22_rx_digest_test(enum st_fps fps[], int width[], int height[],
                                int pkt_data_len[], int total_pkts[],
                                enum st_test_level level, int sessions = 1,
                                bool enable_rtcp = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
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
    test_ctx_tx[i]->fb_cnt = ST22_TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_tx[i]->frame_size =
        (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_tx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_tx.notify_rtp_done = st22_tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    ops_tx.rtp_pkt_size =
        test_ctx_tx[i]->pkt_data_len + sizeof(struct st22_rfc9134_rtp_hdr);
    ops_tx.rtp_frame_total_pkts = test_ctx_tx[i]->total_pkts_in_frame;
    ops_tx.notify_frame_done = st22_frame_done;
    ops_tx.get_next_frame = st22_next_video_frame;
    if (enable_rtcp) {
      ops_tx.flags |= ST22_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 512;
    }

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    size_t frame_size = test_ctx_tx[i]->frame_size;
    uint8_t* fb;
    for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)st22_tx_get_fb_addr(tx_handle[i], frame);
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
    test_ctx_rx[i]->fb_cnt = ST22_TEST_SHA_HIST_NUM;
    test_ctx_rx[i]->fb_idx = 0;

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_rx.type = ST22_TYPE_FRAME_LEVEL;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    if (enable_rtcp) {
      ops_rx.flags |= ST22_RX_FLAG_ENABLE_RTCP | ST22_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 100;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 4;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    ops_rx.notify_rtp_ready = st22_rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_rx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_rx[i]->frame_size =
        (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_rx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_rx.notify_frame_ready = st22_digest_rx_frame_ready;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    /* copy sha from tx */
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    test_ctx_rx[i]->stop = false;
    sha_check[i] = std::thread(st22_digest_rx_frame_check, test_ctx_rx[i]);

    test_ctx_rx[i]->handle = rx_handle[i];

    struct st_queue_meta meta;
    ret = st22_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    test_ctx_rx[i]->stop = true;
    sha_check[i].join();
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_rx, digest_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  int pkt_data_len[2] = {1280, 1280};
  int total_pkts[2] = {551, 1520};
  st22_rx_digest_test(fps, width, height, pkt_data_len, total_pkts,
                      ST_TEST_LEVEL_MANDATORY, 2);
}

TEST(St22_rx, digest_rtcp_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  int pkt_data_len[2] = {1280, 1280};
  int total_pkts[2] = {551, 1520};
  st22_rx_digest_test(fps, width, height, pkt_data_len, total_pkts, ST_TEST_LEVEL_ALL, 2,
                      true);
}

static void st22_tx_user_pacing_test(int width[], int height[], int pkt_data_len[],
                                     int total_pkts[], enum st_test_level level,
                                     int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st22_tx_ops ops_tx;
  struct st22_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22_tx_handle> tx_handle;
  std::vector<st22_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> tx_framerate;
  std::vector<double> rx_framerate;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  tx_framerate.resize(sessions);
  rx_framerate.resize(sessions);

  enum st_fps fps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps) / 2;
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
    if (ctx->mcast_only)
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps;
    ops_tx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.flags = ST22_TX_FLAG_USER_PACING;

    test_ctx_tx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_tx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_tx[i]->frame_size =
        (size_t)test_ctx_tx[i]->pkt_data_len * test_ctx_tx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_tx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_tx.notify_frame_done = st22_frame_done;
    ops_tx.get_next_frame = st22_next_video_frame_timestamp;

    tx_handle[i] = st22_tx_create(m_handle, &ops_tx);

    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
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
    if (ctx->mcast_only)
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 15000 + i * 2;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps;
    ops_rx.payload_type = ST22_TEST_PAYLOAD_TYPE;
    ops_rx.type = ST22_TYPE_FRAME_LEVEL;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;

    test_ctx_rx[i]->pkt_data_len = pkt_data_len[i];
    test_ctx_rx[i]->total_pkts_in_frame = total_pkts[i]; /* compress ratio 1/8, 4320/8 */
    test_ctx_rx[i]->frame_size =
        (size_t)test_ctx_rx[i]->pkt_data_len * test_ctx_rx[i]->total_pkts_in_frame;

    /* set max to 100 extra */
    ops_rx.framebuff_max_size =
        test_ctx_tx[i]->frame_size + test_ctx_tx[i]->pkt_data_len * 100;
    ops_rx.notify_frame_ready = st22_rx_frame_ready;

    rx_handle[i] = st22_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    rx_framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    tx_framerate[i] = test_ctx_tx[i]->fb_send / time_sec;
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         rx_framerate[i]);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx_tx[i]->fb_send,
         tx_framerate[i]);
    EXPECT_NEAR(rx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(tx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st22_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st22_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St22_tx, tx_user_pacing) {
  int width[1] = {1920};
  int height[1] = {1080};
  int pkt_data_len[1] = {1260};
  int total_pkts[1] = {602};
  st22_tx_user_pacing_test(width, height, pkt_data_len, total_pkts, ST_TEST_LEVEL_ALL, 1);
}
