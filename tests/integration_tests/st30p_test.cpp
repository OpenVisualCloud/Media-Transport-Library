/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 3024 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST30P_TEST_PAYLOAD_TYPE (111)
#define ST30P_TEST_UDP_PORT (50000)

static int test_st30p_tx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();
  return 0;
}

static int test_st30p_tx_frame_done(void* priv, struct st30_frame* frame) {
  tests_context* s = (tests_context*)priv;

  if (!s->handle) return -EIO; /* not ready */

  s->fb_send_done++;
  return 0;
}

static int test_st30p_rx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();
  return 0;
}

static void st30p_tx_ops_init(tests_context* st30, struct st30p_tx_ops* ops_tx) {
  auto ctx = st30->ctx;

  memset(ops_tx, 0, sizeof(*ops_tx));
  ops_tx->name = "st30p_test";
  ops_tx->priv = st30;
  ops_tx->port.num_port = 1;
  memcpy(ops_tx->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx->port.udp_port[MTL_SESSION_PORT_P] = ST30P_TEST_UDP_PORT + st30->idx;
  ops_tx->port.payload_type = ST30P_TEST_PAYLOAD_TYPE;
  ops_tx->fmt = ST30_FMT_PCM24;
  ops_tx->channel = 2;
  ops_tx->sampling = ST30_SAMPLING_48K;
  ops_tx->ptime = ST30_PTIME_1MS;
  /* count frame size for 10ms  */
  ops_tx->framebuff_size =
      st30_calculate_framebuff_size(ops_tx->fmt, ops_tx->ptime, ops_tx->sampling,
                                    ops_tx->channel, 10 * NS_PER_MS, NULL);

  ops_tx->framebuff_cnt = st30->fb_cnt;
  ops_tx->notify_frame_available = test_st30p_tx_frame_available;

  st30->frame_size = ops_tx->framebuff_size;
}

static void st30p_rx_ops_init(tests_context* st30, struct st30p_rx_ops* ops_rx) {
  auto ctx = st30->ctx;

  memset(ops_rx, 0, sizeof(*ops_rx));
  ops_rx->name = "st30p_test";
  ops_rx->priv = st30;
  ops_rx->port.num_port = 1;
  memcpy(ops_rx->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops_rx->port.udp_port[MTL_SESSION_PORT_P] = ST30P_TEST_UDP_PORT + st30->idx;
  ops_rx->port.payload_type = ST30P_TEST_PAYLOAD_TYPE;
  ops_rx->fmt = ST30_FMT_PCM24;
  ops_rx->channel = 2;
  ops_rx->sampling = ST30_SAMPLING_48K;
  ops_rx->ptime = ST30_PTIME_1MS;
  /* count frame size */
  ops_rx->framebuff_size =
      st30_calculate_framebuff_size(ops_rx->fmt, ops_rx->ptime, ops_rx->sampling,
                                    ops_rx->channel, 10 * NS_PER_MS, NULL);
  ops_rx->framebuff_cnt = st30->fb_cnt;
  ops_rx->notify_frame_available = test_st30p_rx_frame_available;

  st30->frame_size = ops_rx->framebuff_size;
}

static void st30p_tx_assert_cnt(int expect_st30_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_st30_tx_cnt);
}

static void st30p_rx_assert_cnt(int expect_st30_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_st30_rx_cnt);
}

TEST(St30p, tx_create_free_single) {
  pipeline_create_free_test(st30p_tx, 0, 1, 1);
}
TEST(St30p, tx_create_free_multi) {
  pipeline_create_free_test(st30p_tx, 0, 1, 6);
}
TEST(St30p, tx_create_free_mix) {
  pipeline_create_free_test(st30p_tx, 2, 3, 4);
}
TEST(St30p, rx_create_free_single) {
  pipeline_create_free_test(st30p_rx, 0, 1, 1);
}
TEST(St30p, rx_create_free_multi) {
  pipeline_create_free_test(st30p_rx, 0, 1, 6);
}
TEST(St30p, rx_create_free_mix) {
  pipeline_create_free_test(st30p_rx, 2, 3, 4);
}
TEST(St30p, tx_create_free_max) {
  pipeline_create_free_max(st30p_tx, TEST_CREATE_FREE_MAX);
}
TEST(St30p, rx_create_free_max) {
  pipeline_create_free_max(st30p_rx, TEST_CREATE_FREE_MAX);
}
TEST(St30p, tx_create_expect_fail) {
  pipeline_expect_fail_test(st30p_tx);
}
TEST(St30p, rx_create_expect_fail) {
  pipeline_expect_fail_test(st30p_rx);
}

static void test_st30p_tx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st30_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st30p_tx_get_frame((st30p_tx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
        lck.unlock();
      }
      continue;
    }
    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->fmt != s->audio_fmt) s->incomplete_frame_cnt++;
    if (frame->channel != s->audio_channel) s->incomplete_frame_cnt++;
    if (frame->ptime != s->audio_ptime) s->incomplete_frame_cnt++;
    if (frame->sampling != s->audio_sampling) s->incomplete_frame_cnt++;
    if (s->user_timestamp) {
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = s->fb_send;
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, s->fb_send);
    }

    st30p_tx_put_frame((st30p_tx_handle)handle, frame);

    s->fb_send++;
    if (!s->start_time) {
      s->start_time = st_test_get_monotonic_time();
      dbg("%s(%d), start_time %" PRIu64 "\n", __func__, s->idx, s->start_time);
    }
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st30p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st30_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  uint64_t timestamp = 0;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st30p_rx_get_frame((st30p_rx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
        lck.unlock();
      }
      continue;
    }

    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->fmt != s->audio_fmt) s->incomplete_frame_cnt++;
    if (frame->channel != s->audio_channel) s->incomplete_frame_cnt++;
    if (frame->ptime != s->audio_ptime) s->incomplete_frame_cnt++;
    if (frame->sampling != s->audio_sampling) s->incomplete_frame_cnt++;

    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == timestamp) s->incomplete_frame_cnt++;
    timestamp = frame->timestamp;

    /* check user timestamp if it has */
    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp) {
        /*
         * some frame may drop as SHA256 is slow,
         * just check timestamp is adding with small step
         */
        if (((uint32_t)frame->timestamp - s->pre_timestamp) > 4) {
          s->incomplete_frame_cnt++;
          err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
              s->idx, frame->timestamp, s->pre_timestamp);
        }
      }
      s->pre_timestamp = (uint32_t)frame->timestamp;
    }

    unsigned char result[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)frame->addr, s->frame_size, result);
    int i = 0;
    for (i = 0; i < TEST_MAX_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_MAX_SHA_HIST_NUM) {
      test_sha_dump("st30p_rx_error_sha", result);
      s->sha_fail_cnt++;
    }
    /* directly put */
    st30p_rx_put_frame((st30p_rx_handle)handle, frame);
    s->fb_rec++;
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

struct st30p_rx_digest_test_para {
  int sessions;
  bool check_fps;
  enum st_test_level level;
  int fb_cnt;
  uint32_t ssrc;
  bool block_get;
  bool dedicated_tx_queue;
  bool zero_payload_type;
};

static void test_st30p_init_rx_digest_para(struct st30p_rx_digest_test_para* para) {
  memset(para, 0, sizeof(*para));

  para->sessions = 1;
  para->check_fps = true;
  para->fb_cnt = TEST_MAX_SHA_HIST_NUM;
  para->level = ST_TEST_LEVEL_MANDATORY;
  para->ssrc = 0;
  para->block_get = false;
  para->dedicated_tx_queue = false;
  para->zero_payload_type = false;
}

static void st30p_rx_digest_test(enum st30_fmt fmt[], uint16_t channel[],
                                 enum st30_sampling sampling[], enum st30_ptime ptime[],
                                 struct st30p_rx_digest_test_para* para,
                                 st_tests_context* ctx) {
  auto st = ctx->handle;
  int ret;
  struct st30p_tx_ops ops_tx;
  struct st30p_rx_ops ops_rx;
  int sessions = para->sessions;

  if (ctx->para.num_ports < 2) {
    info("%s, dual port should be enabled, one for tx and one for rx\n", __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level lower than global */
  if (para->level < ctx->level) return;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st30p_tx_handle> tx_handle;
  std::vector<st30p_rx_handle> rx_handle;
  std::vector<double> expect_framerate_tx;
  std::vector<double> expect_framerate_rx;
  std::vector<double> framerate_tx;
  std::vector<double> framerate_rx;
  std::vector<std::thread> tx_thread;
  std::vector<std::thread> rx_thread;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate_tx.resize(sessions);
  expect_framerate_rx.resize(sessions);
  framerate_tx.resize(sessions);
  framerate_rx.resize(sessions);
  tx_thread.resize(sessions);
  rx_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = para->fb_cnt;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->audio_fmt = fmt[i];
    test_ctx_tx[i]->audio_channel = channel[i];
    test_ctx_tx[i]->audio_sampling = sampling[i];
    test_ctx_tx[i]->audio_ptime = ptime[i];
    test_ctx_tx[i]->block_get = para->block_get;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30p_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.port.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ST30P_TEST_UDP_PORT + i * 2;
    ops_tx.port.payload_type = para->zero_payload_type ? 0 : ST30P_TEST_PAYLOAD_TYPE;
    ops_tx.port.ssrc = para->ssrc;
    ops_tx.fmt = fmt[i];
    ops_tx.channel = channel[i];
    ops_tx.sampling = sampling[i];
    ops_tx.ptime = ptime[i];
    double fps;
    ops_tx.framebuff_size = st30_calculate_framebuff_size(
        ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel, 10 * NS_PER_MS, &fps);
    expect_framerate_tx[i] = fps;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (para->block_get)
      ops_tx.flags |= ST30P_TX_FLAG_BLOCK_GET;
    else
      ops_tx.notify_frame_available = test_st30p_tx_frame_available;
    if (para->dedicated_tx_queue) ops_tx.flags |= ST30P_TX_FLAG_DEDICATE_QUEUE;
    ops_tx.notify_frame_done = test_st30p_tx_frame_done;

    test_ctx_tx[i]->frame_size = ops_tx.framebuff_size;

    tx_handle[i] = st30p_tx_create(st, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    if (para->block_get) {
      ret = st30p_tx_set_block_timeout(tx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    /* sha calculate */
    size_t frame_size = test_ctx_tx[i]->frame_size;
    uint8_t* fb;
    for (int frame = 0; frame < test_ctx_tx[i]->fb_cnt; frame++) {
      fb = (uint8_t*)st30p_tx_get_fb_addr(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);

      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st30p_tx", result);
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    tx_thread[i] = std::thread(test_st30p_tx_frame_thread, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = para->fb_cnt;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->audio_fmt = fmt[i];
    test_ctx_rx[i]->audio_channel = channel[i];
    test_ctx_rx[i]->audio_sampling = sampling[i];
    test_ctx_rx[i]->audio_ptime = ptime[i];
    test_ctx_rx[i]->block_get = para->block_get;
    /* copy tx frame size */
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    expect_framerate_rx[i] = expect_framerate_tx[i];
    /* copy sha */
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_MAX_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st30p_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.port.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ST30P_TEST_UDP_PORT + i * 2;
    ops_rx.port.payload_type = para->zero_payload_type ? 0 : ST30P_TEST_PAYLOAD_TYPE;
    ops_rx.port.ssrc = para->ssrc;
    ops_rx.fmt = fmt[i];
    ops_rx.channel = channel[i];
    ops_rx.sampling = sampling[i];
    ops_rx.ptime = ptime[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.framebuff_size = test_ctx_rx[i]->frame_size;
    if (para->block_get)
      ops_rx.flags |= ST30P_RX_FLAG_BLOCK_GET;
    else
      ops_rx.notify_frame_available = test_st30p_rx_frame_available;

    rx_handle[i] = st30p_rx_create(st, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    if (para->block_get) {
      ret = st30p_rx_set_block_timeout(rx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    test_ctx_rx[i]->handle = rx_handle[i];

    rx_thread[i] = std::thread(test_st30p_rx_frame_thread, test_ctx_rx[i]);
  }

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    framerate_tx[i] = test_ctx_tx[i]->fb_send / time_sec;

    test_ctx_tx[i]->stop = true;
    if (para->block_get) st30p_tx_wake_block(tx_handle[i]);
    test_ctx_tx[i]->cv.notify_all();
    tx_thread[i].join();
  }

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate_rx[i] = test_ctx_rx[i]->fb_rec / time_sec;

    test_ctx_rx[i]->stop = true;
    if (para->block_get) st30p_rx_wake_block(rx_handle[i]);
    test_ctx_rx[i]->cv.notify_all();
    rx_thread[i].join();
  }

  for (int i = 0; i < sessions; i++) {
    ret = st30p_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_send %d framerate %f:%f\n", __func__, i,
         test_ctx_tx[i]->fb_send, framerate_tx[i], expect_framerate_tx[i]);
    EXPECT_GT(test_ctx_tx[i]->fb_send, 0);
    test_ctx_rx[i]->fb_send = test_ctx_tx[i]->fb_send;
    delete test_ctx_tx[i];
  }
  for (int i = 0; i < sessions; i++) {
    ret = st30p_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_rec %d framerate %f:%f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate_rx[i], expect_framerate_rx[i]);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    size_t pkt_len = st30_get_packet_size(
        test_ctx_rx[i]->audio_fmt, test_ctx_rx[i]->audio_ptime,
        test_ctx_rx[i]->audio_sampling, test_ctx_rx[i]->audio_channel);
    if (pkt_len == test_ctx_rx[i]->frame_size) {
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    }
    EXPECT_LE(test_ctx_rx[i]->user_meta_fail_cnt, 2);
    if (para->check_fps) {
      EXPECT_NEAR(framerate_rx[i], expect_framerate_rx[i], expect_framerate_rx[i] * 0.1);
    }
    delete test_ctx_rx[i];
  }
}

TEST(St30p, digest_s3) {
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[3] = {ST30_PTIME_1MS, ST30_PTIME_1MS, ST30_PTIME_125US};
  uint16_t c[3] = {8, 2, 4};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};

  struct st30p_rx_digest_test_para para;
  test_st30p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.check_fps = true;
  para.sessions = 3;
  para.dedicated_tx_queue = true;
  para.zero_payload_type = true;

  st30p_rx_digest_test(f, c, s, pt, &para, st_test_ctx());
}

TEST(St30p, digest_s3_block) {
  enum st30_sampling s[3] = {ST31_SAMPLING_44K, ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[3] = {ST31_PTIME_1_09MS, ST30_PTIME_1MS, ST30_PTIME_125US};
  uint16_t c[3] = {3, 5, 7};
  enum st30_fmt f[3] = {ST31_FMT_AM824, ST30_FMT_PCM16, ST30_FMT_PCM24};

  struct st30p_rx_digest_test_para para;
  test_st30p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.check_fps = true;
  para.block_get = true;
  para.sessions = 3;

  st30p_rx_digest_test(f, c, s, pt, &para, st_test_ctx());
}
