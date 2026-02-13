/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST30_TEST_PAYLOAD_TYPE (111)

static int tx_audio_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st30_tx_frame_meta* meta) {
  return tx_next_frame(priv, next_frame_idx);
}

static int tx_audio_next_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                         struct st30_tx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  if (!ctx->ptp_time_first_frame) {
    ctx->ptp_time_first_frame = mtl_ptp_read_time(ctx->ctx->handle);
  }

  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = ctx->ptp_time_first_frame + ctx->fb_send * ctx->frame_time * 2;
  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int tx_audio_build_rtp_packet(tests_context* s, struct st_rfc3550_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = ST30_TEST_PAYLOAD_TYPE;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->rtp_tmstamp;
  s->rtp_tmstamp++;
  rtp->seq_number = htons(s->seq_id);
  s->seq_id++;
  if (s->seq_id == 0x10000) s->seq_id = 0;
  if (s->check_sha) {
    uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);
    mtl_memcpy(payload, s->frame_buf[s->fb_idx], s->pkt_data_len);
    s->fb_idx++;
    if (s->fb_idx >= TEST_SHA_HIST_NUM) s->fb_idx = 0;
  }
  *pkt_len = sizeof(struct st_rfc3550_rtp_hdr) + s->pkt_data_len;
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
    mbuf = st30_tx_get_mbuf((st30_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st30_tx_get_mbuf((st30_tx_handle)ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_audio_build_rtp_packet(ctx, (struct st_rfc3550_rtp_hdr*)usrptr, &mbuf_len);
    st30_tx_put_mbuf((st30_tx_handle)ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;

  if (!ctx->handle) return -EIO; /* not ready */

  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  ctx->fb_send++;
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

static void rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st30_rx_get_mbuf((st30_rx_handle)ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st30_rx_get_mbuf((st30_rx_handle)ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    if (ctx->check_sha) {
      struct st_rfc3550_rtp_hdr* hdr = (struct st_rfc3550_rtp_hdr*)usrptr;
      uint8_t* payload = (uint8_t*)hdr + sizeof(*hdr);
      unsigned char result[SHA256_DIGEST_LENGTH];
      SHA256((unsigned char*)payload, ctx->frame_size, result);
      int i;
      for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
        unsigned char* target_sha = ctx->shas[i];
        if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_SHA_HIST_NUM) {
        test_sha_dump("st30_rx_error_sha", result);
        ctx->sha_fail_cnt++;
      }
      ctx->check_sha_frame_cnt++;
    }
    ctx->fb_rec++;
    st30_rx_put_mbuf((st30_rx_handle)ctx->handle, mbuf);
  }
}

static int st30_rx_frame_ready(void* priv, void* frame, struct st30_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  /* direct do the sha check since frame_size is small */
  if (ctx->check_sha) {
    unsigned char result[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)frame, ctx->frame_size, result);
    int i;
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = ctx->shas[i];
      if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st30_rx_error_sha", result);
      ctx->sha_fail_cnt++;
    }
    ctx->check_sha_frame_cnt++;
  }

  st30_rx_put_framebuff((st30_rx_handle)ctx->handle, frame);
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void st30_rx_ops_init(tests_context* st30, struct st30_rx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_PORT_P] = 20000 + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_PORT_R] = 20000 + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = 2;
  ops->fmt = ST30_FMT_PCM16;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->sampling = ST30_SAMPLING_48K;
  ops->ptime = ST30_PTIME_1MS;
  ops->framebuff_cnt = st30->fb_cnt;
  ops->framebuff_size =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  ops->notify_frame_ready = st30_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st30_tx_ops_init(tests_context* st30, struct st30_tx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->para.num_ports;
  if (ctx->same_dual_port) ops->num_port = 1;
  memcpy(ops->dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->udp_port[MTL_SESSION_PORT_P] = 20000 + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops->udp_port[MTL_SESSION_PORT_R] = 20000 + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = 2;
  ops->fmt = ST30_FMT_PCM16;
  ops->payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops->sampling = ST30_SAMPLING_48K;
  ops->ptime = ST30_PTIME_1MS;
  ops->framebuff_cnt = st30->fb_cnt;
  st30->pkt_data_len =
      st30_get_packet_size(ops->fmt, ops->ptime, ops->sampling, ops->channel);
  ops->framebuff_size = st30->pkt_data_len;
  ops->get_next_frame = tx_audio_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void st30_tx_assert_cnt(int expect_s30_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_s30_tx_cnt);
}

static void st30_rx_assert_cnt(int expect_s30_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_s30_rx_cnt);
}

TEST(St30_tx, create_free_single) {
  create_free_test(st30_tx, 0, 1, 1);
}
TEST(St30_tx, create_free_multi) {
  create_free_test(st30_tx, 0, 1, 6);
}
TEST(St30_tx, create_free_mix) {
  create_free_test(st30_tx, 2, 3, 4);
}
TEST(St30_tx, create_free_max) {
  create_free_max(st30_tx, TEST_CREATE_FREE_MAX);
}
TEST(St30_tx, create_expect_fail) {
  expect_fail_test(st30_tx);
}
TEST(St30_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st30_tx, ST30_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st30_tx, ST30_TYPE_RTP_LEVEL, ring_size);
}
TEST(St30_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st30_tx, fbcnt);
  fbcnt = 1000;
  test_get_framebuffer(st30_tx, fbcnt);
}
TEST(St30_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st30_tx, fbcnt);
  fbcnt = 1000;
  expect_fail_test_get_framebuffer(st30_tx, fbcnt);
}

TEST(St30_rx, create_free_single) {
  create_free_test(st30_rx, 0, 1, 1);
}
TEST(St30_rx, create_free_multi) {
  create_free_test(st30_rx, 0, 1, 6);
}
TEST(St30_rx, create_free_mix) {
  create_free_test(st30_rx, 2, 3, 4);
}
TEST(St30_rx, create_free_max) {
  create_free_max(st30_rx, TEST_CREATE_FREE_MAX);
}
TEST(St30_rx, create_expect_fail) {
  expect_fail_test(st30_rx);
}
TEST(St30_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
}

static void st30_tx_fps_test(enum st30_type type[], enum st30_sampling sample[],
                             enum st30_ptime ptime[], uint16_t channel[],
                             enum st30_fmt fmt[], enum st_test_level level,
                             int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<st30_tx_handle> handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  /* return if level small than global */
  if (level < ctx->level) return;

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = (double)NS_PER_S / st30_get_packet_time(ptime[i]);
    test_ctx[i] = new tests_context();
    ASSERT_TRUE(test_ctx[i] != NULL);

    test_ctx[i]->idx = i;
    test_ctx[i]->ctx = ctx;
    test_ctx[i]->fb_cnt = 3;
    test_ctx[i]->fb_idx = 0;
    st30_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.sampling = sample[i];
    ops.channel = channel[i];
    ops.fmt = fmt[i];
    ops.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops.ptime = ptime[i];
    ops.framebuff_size = st30_get_sample_size(ops.fmt) *
                         st30_get_sample_num(ops.ptime, ops.sampling) * ops.channel;
    test_ctx[i]->pkt_data_len =
        st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);

    handle[i] = st30_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    test_ctx[i]->handle = handle[i];

    if (type[i] == ST30_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);
  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    test_ctx[i]->stop = true;
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st30_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx[i];
  }
}

static void st30_rx_fps_test(enum st30_type type[], enum st30_sampling sample[],
                             enum st30_ptime ptime[], uint16_t channel[],
                             enum st30_fmt fmt[], enum st_test_level level,
                             int sessions = 1, bool check_sha = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops_tx;
  struct st30_rx_ops ops_rx;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level small than global */
  if (level < ctx->level) return;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st30_tx_handle> tx_handle;
  std::vector<st30_rx_handle> rx_handle;
  std::vector<double> framerate;
  std::vector<double> expect_framerate;
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
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    if (check_sha)
      test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    else
      test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30_test";
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
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_tx.type = type[i];
    ops_tx.sampling = sample[i];
    ops_tx.channel = channel[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_tx.ssrc = i ? i + 0x66666666 : 0;
    ops_tx.ptime = ptime[i];
    ops_tx.pacing_way = ctx->tx_audio_pacing_way;
    ops_tx.framebuff_size =
        st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
    EXPECT_GE(ops_tx.framebuff_size, 0);
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_audio_next_frame;
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    test_ctx_tx[i]->pkt_data_len = ops_tx.framebuff_size;
    tx_handle[i] = st30_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->check_sha = check_sha;
    if (check_sha) {
      uint8_t* fb;
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        if (type[i] == ST30_TYPE_FRAME_LEVEL) {
          fb = (uint8_t*)st30_tx_get_framebuffer(tx_handle[i], frame);
        } else {
          test_ctx_tx[i]->frame_buf[frame] =
              (uint8_t*)st_test_zmalloc(ops_tx.framebuff_size);
          fb = test_ctx_tx[i]->frame_buf[frame];
        }
        ASSERT_TRUE(fb != NULL);
        st_test_rand_data(fb, ops_tx.framebuff_size, frame);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, ops_tx.framebuff_size, result);
        test_sha_dump("st30_rx", result);
      }
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    ops_rx.name = "st30_test";
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
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_rx.type = type[i];
    ops_rx.sampling = sample[i];
    ops_rx.channel = channel[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_rx.ssrc = i ? i + 0x66666666 : 0;
    ops_rx.ptime = ptime[i];
    ops_rx.framebuff_size =
        st30_get_packet_size(ops_rx.fmt, ops_rx.ptime, ops_rx.sampling, ops_rx.channel);
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st30_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    double pkt_time_ns = st30_get_packet_time(ops_rx.ptime);
    if (pkt_time_ns > 0)
      expect_framerate[i] = 1000000000.0 / pkt_time_ns;
    else
      expect_framerate[i] = 1000;

    rx_handle[i] = st30_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->check_sha = check_sha;
    test_ctx_rx[i]->frame_size = ops_rx.framebuff_size;
    if (check_sha) {
      memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
             TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    }
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }

    test_ctx_rx[i]->handle = rx_handle[i];

    struct st_queue_meta q_meta;
    ret = st30_rx_get_queue_meta(rx_handle[i], &q_meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    if (check_sha) {
      EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    }
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st30_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st30_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    if (check_sha && (type[i] == ST30_TYPE_RTP_LEVEL)) {
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        if (test_ctx_tx[i]->frame_buf[frame])
          st_test_free(test_ctx_tx[i]->frame_buf[frame]);
      }
    }
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St30_tx, frame_48k_mono_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {1};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, rtp_96k_mono_s1) {
  enum st30_type type[1] = {ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {1};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, frame_48k_stereo_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {2};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, frame_48k_stereo_125us_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_ptime pt[1] = {ST30_PTIME_125US};
  uint16_t c[1] = {2};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, rtp_48k_stereo_125us_s1) {
  enum st30_type type[1] = {ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_ptime pt[1] = {ST30_PTIME_125US};
  uint16_t c[1] = {2};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, rtp_96k_stereo_s1) {
  enum st30_type type[1] = {ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {2};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, frame_48k_sgrp_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {4};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, frame_96k_sgrp_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  enum st30_ptime pt[1] = {ST30_PTIME_1MS};
  uint16_t c[1] = {4};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, pt, c, &f[i], ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, mix_96k_stereo_s3) {
  enum st30_type type[3] = {ST30_TYPE_RTP_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_96K, ST30_SAMPLING_96K};
  enum st30_ptime pt[3] = {ST30_PTIME_1MS, ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[3] = {2, 2, 2};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_tx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 3);
}

TEST(St30_tx, mix_48k_96_mix) {
  enum st30_type type[3] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_RTP_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[3] = {ST30_PTIME_1MS, ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[3] = {2, 1, 4};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_tx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 3);
}
TEST(St30_rx, mix_48k_96_mix) {
  enum st30_type type[3] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_RTP_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[3] = {ST30_PTIME_1MS, ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[3] = {2, 1, 4};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 3);
}
TEST(St30_rx, frame_digest_48k_96_mix) {
  enum st30_type type[2] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[2] = {ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[2] = {2, 1};
  enum st30_fmt f[2] = {ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St30_rx, rtp_digest_48k_96_mix) {
  enum st30_type type[2] = {ST30_TYPE_RTP_LEVEL, ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[2] = {ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[2] = {1, 4};
  enum st30_fmt f[2] = {ST30_FMT_PCM16, ST30_FMT_PCM8};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St30_rx, digest_mix) {
  enum st30_type type[2] = {ST30_TYPE_RTP_LEVEL, ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[2] = {ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[2] = {1, 4};
  enum st30_fmt f[2] = {ST30_FMT_PCM16, ST30_FMT_PCM8};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_MANDATORY, 2, true);
}
TEST(St30_rx, frame_digest_st31_mix) {
  enum st30_type type[2] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[2] = {ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[2] = {2, 2};
  enum st30_fmt f[2] = {ST31_FMT_AM824, ST31_FMT_AM824};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_MANDATORY, 2, true);
}
TEST(St30_rx, rtp_digest_st31_mix) {
  enum st30_type type[2] = {ST30_TYPE_RTP_LEVEL, ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  enum st30_ptime pt[2] = {ST30_PTIME_1MS, ST30_PTIME_1MS};
  uint16_t c[2] = {2, 2};
  enum st30_fmt f[2] = {ST31_FMT_AM824, ST31_FMT_AM824};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 2, true);
}
TEST(St30_rx, frame_digest_stereo_ptime_mix_s5) {
  enum st30_type type[5] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[5] = {ST30_SAMPLING_48K, ST30_SAMPLING_48K, ST30_SAMPLING_48K,
                             ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[5] = {ST30_PTIME_125US, ST30_PTIME_250US, ST30_PTIME_333US,
                           ST30_PTIME_4MS, ST31_PTIME_80US};
  uint16_t c[5] = {2, 2, 2, 2, 2};
  enum st30_fmt f[5] = {ST30_FMT_PCM16, ST30_FMT_PCM16, ST30_FMT_PCM16, ST30_FMT_PCM16,
                        ST31_FMT_AM824};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 5, true);
}
TEST(St30_rx, frame_digest_max_channel_48k_16bit_ptime_mix_s5) {
  enum st30_type type[5] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[5] = {ST30_SAMPLING_48K, ST30_SAMPLING_48K, ST30_SAMPLING_48K,
                             ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[5] = {ST30_PTIME_125US, ST30_PTIME_250US, ST30_PTIME_333US,
                           ST30_PTIME_1MS, ST30_PTIME_4MS};
  uint16_t c[5] = {120, 60, 45, 15, 3};
  enum st30_fmt f[5] = {ST30_FMT_PCM16, ST30_FMT_PCM16, ST30_FMT_PCM16, ST30_FMT_PCM16,
                        ST30_FMT_PCM16};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_ALL, 5, true);
}
TEST(St30_rx, frame_digest_max_channel_48k_24bit_ptime_mix_s5) {
  enum st30_type type[5] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[5] = {ST30_SAMPLING_48K, ST30_SAMPLING_48K, ST30_SAMPLING_48K,
                             ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_ptime pt[5] = {ST30_PTIME_125US, ST30_PTIME_250US, ST30_PTIME_333US,
                           ST30_PTIME_1MS, ST30_PTIME_4MS};
  uint16_t c[5] = {80, 40, 30, 10, 2};
  enum st30_fmt f[5] = {ST30_FMT_PCM24, ST30_FMT_PCM24, ST30_FMT_PCM24, ST30_FMT_PCM24,
                        ST30_FMT_PCM24};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_MANDATORY, 5, true);
}
TEST(St30_rx, frame_digest_max_channel_96k_24bit_ptime_mix_s5) {
  enum st30_type type[5] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[5] = {ST30_SAMPLING_96K, ST30_SAMPLING_96K, ST30_SAMPLING_96K,
                             ST30_SAMPLING_96K, ST30_SAMPLING_96K};
  enum st30_ptime pt[5] = {ST30_PTIME_125US, ST30_PTIME_250US, ST30_PTIME_333US,
                           ST30_PTIME_1MS, ST30_PTIME_4MS};
  uint16_t c[5] = {40, 20, 15, 5, 1};
  enum st30_fmt f[5] = {ST30_FMT_PCM24, ST30_FMT_PCM24, ST30_FMT_PCM24, ST30_FMT_PCM24,
                        ST30_FMT_PCM24};
  st30_rx_fps_test(type, s, pt, c, f, ST_TEST_LEVEL_MANDATORY, 5, true);
}

static void st30_rx_update_src_test(enum st30_type type, int tx_sessions,
                                    enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;

  struct st30_tx_ops ops_tx;
  struct st30_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }
  /* return if level lower than global */
  if (level < ctx->level) return;

  ASSERT_TRUE(tx_sessions >= 1);
  bool tx_update_dst = (tx_sessions == 1);

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st30_tx_handle> tx_handle;
  std::vector<st30_rx_handle> rx_handle;
  double expect_framerate = 1000.0;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  framerate.resize(rx_sessions);
  rtp_thread_tx.resize(tx_sessions);
  rtp_thread_rx.resize(rx_sessions);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30_test";
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
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_tx.type = type;
    ops_tx.sampling = ST30_SAMPLING_48K;
    ops_tx.channel = 2;
    ops_tx.fmt = ST30_FMT_PCM24;
    ops_tx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_tx.ptime = ST30_PTIME_1MS;
    ops_tx.pacing_way = ctx->tx_audio_pacing_way;
    ops_tx.framebuff_size =
        st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_audio_next_frame;
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;

    tx_handle[i] = st30_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->handle = tx_handle[i];

    if (type == ST30_TYPE_RTP_LEVEL) {
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
    ops_rx.name = "st30_test";
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
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_rx.type = type;
    ops_rx.sampling = ST30_SAMPLING_48K;
    ops_rx.channel = 2;
    ops_rx.fmt = ST30_FMT_PCM24;
    ops_rx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_rx.ptime = ST30_PTIME_1MS;
    ops_rx.framebuff_size =
        st30_get_packet_size(ops_rx.fmt, ops_rx.ptime, ops_rx.sampling, ops_rx.channel);
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st30_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st30_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);
    if (type == ST30_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 20000 + 2;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  if (tx_update_dst) {
    test_ctx_tx[0]->seq_id = 0; /* reset seq id */
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 20000 + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    ret = st30_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  } else {
    test_ctx_tx[1]->seq_id = 0; /* reset seq id */
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st30_rx_update_source(rx_handle[i], &src);
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
    EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = 20000 + 2;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    for (int i = 0; i < rx_sessions; i++) {
      ret = st30_rx_update_source(rx_handle[i], &src);
      EXPECT_GE(ret, 0);
      test_ctx_tx[2]->seq_id = rand(); /* random seq id */
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
      EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[MTL_SESSION_PORT_P] = 20000 + 0;
  memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  test_ctx_tx[0]->seq_id = rand(); /* random seq id */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = 20000 + 0;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    ret = st30_tx_update_destination(tx_handle[0], &dst);
    EXPECT_GE(ret, 0);
  }
  for (int i = 0; i < rx_sessions; i++) {
    ret = st30_rx_update_source(rx_handle[i], &src);
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
    EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
  }

  /* stop rtp thread */
  for (int i = 0; i < rx_sessions; i++) {
    if (type == ST30_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_rx[i].join();
    }
  }
  for (int i = 0; i < tx_sessions; i++) {
    if (type == ST30_TYPE_RTP_LEVEL) {
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
    ret = st30_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st30_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
  }
}

TEST(St30_rx, update_source_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 3, ST_TEST_LEVEL_ALL);
}
TEST(St30_rx, update_source_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 2, ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, update_dest_frame) {
  st30_rx_update_src_test(ST30_TYPE_FRAME_LEVEL, 1, ST_TEST_LEVEL_ALL);
}
TEST(St30_tx, update_dest_rtp) {
  st30_rx_update_src_test(ST30_TYPE_RTP_LEVEL, 1, ST_TEST_LEVEL_ALL);
}

static int st30_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st30_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st30_rx_frame_meta*)ctx->priv;

  if (!ctx->handle) return -EIO;

  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  if (expect_meta->sampling != meta->sampling) ctx->rx_meta_fail_cnt++;
  if (expect_meta->channel != meta->channel) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->rx_meta_fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->rx_meta_fail_cnt++;
  expect_meta->timestamp = meta->timestamp;

  st30_rx_put_framebuff((st30_rx_handle)ctx->handle, frame);

  return 0;
}

static void st30_rx_meta_test(enum st30_fmt fmt[], enum st30_sampling sampling[],
                              uint16_t channel[], enum st_test_level level,
                              int sessions = 1, bool user_timestamp = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops_tx;
  struct st30_rx_ops ops_rx;
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
  std::vector<st30_tx_handle> tx_handle;
  std::vector<st30_rx_handle> rx_handle;
  double expect_framerate = 1000.0;
  std::vector<double> framerate;

  if (user_timestamp) expect_framerate /= 2;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  framerate.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30_meta_test";
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
    ops_tx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_tx.type = ST30_TYPE_FRAME_LEVEL;
    ops_tx.sampling = sampling[i];
    ops_tx.channel = channel[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_tx.ptime = ST30_PTIME_1MS;
    ops_tx.pacing_way = ctx->tx_audio_pacing_way;
    ops_tx.framebuff_size =
        st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
    ;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (user_timestamp) {
      ops_tx.get_next_frame = tx_audio_next_frame_timestamp;
      ops_tx.flags |= ST30_TX_FLAG_USER_PACING;
    } else {
      ops_tx.get_next_frame = tx_audio_next_frame;
    }
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    test_ctx_tx[i]->pkt_data_len = ops_tx.framebuff_size;
    test_ctx_tx[i]->frame_time = st30_get_packet_time(ops_tx.ptime);
    tx_handle[i] = st30_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    test_ctx_tx[i]->handle = tx_handle[i];

    test_ctx_tx[i]->stop = false;
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st30_meta_test";
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
    ops_rx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
    ops_rx.type = ST30_TYPE_FRAME_LEVEL;
    ops_rx.sampling = sampling[i];
    ops_rx.channel = channel[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST30_TEST_PAYLOAD_TYPE;
    ops_rx.ptime = ST30_PTIME_1MS;
    ops_rx.framebuff_size =
        st30_get_packet_size(ops_rx.fmt, ops_rx.ptime, ops_rx.sampling, ops_rx.channel);
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st30_rx_meta_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st30_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->stop = false;

    /* set expect meta data to private */
    auto meta =
        (struct st30_rx_frame_meta*)st_test_zmalloc(sizeof(struct st30_rx_frame_meta));
    ASSERT_TRUE(meta != NULL);
    meta->channel = ops_rx.channel;
    meta->sampling = ops_rx.sampling;
    meta->fmt = ops_rx.fmt;
    test_ctx_rx[i]->priv = meta;

    test_ctx_rx[i]->handle = rx_handle[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
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

    test_ctx_rx[i]->stop = true;
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d fail %d framerate %f, fb send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->rx_meta_fail_cnt, framerate[i],
         test_ctx_tx[i]->fb_send);
    EXPECT_LE(test_ctx_rx[i]->rx_meta_fail_cnt, 2);
    EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
    ret = st30_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st30_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    st_test_free(test_ctx_rx[i]->priv);
    delete test_ctx_rx[i];
  }
}

TEST(St30_rx, frame_meta_pcm16_48k_2ch_s1) {
  enum st30_fmt fmt[1] = {ST30_FMT_PCM16};
  enum st30_sampling sampling[1] = {ST30_SAMPLING_48K};
  uint16_t channel[1] = {2};
  st30_rx_meta_test(fmt, sampling, channel, ST_TEST_LEVEL_ALL);
}

TEST(St30_rx, frame_user_timestamp) {
  enum st30_fmt fmt[1] = {ST30_FMT_PCM16};
  enum st30_sampling sampling[1] = {ST30_SAMPLING_48K};
  uint16_t channel[1] = {2};
  st30_rx_meta_test(fmt, sampling, channel, ST_TEST_LEVEL_MANDATORY, 1, true);
}

static void st30_create_after_start_test(enum st30_type type[],
                                         enum st30_sampling sample[], uint16_t channel[],
                                         enum st30_fmt fmt[], int sessions, int repeat,
                                         enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops_tx;
  struct st30_rx_ops ops_rx;

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
  std::vector<st30_tx_handle> tx_handle;
  std::vector<st30_rx_handle> rx_handle;
  double expect_framerate = 1000.0;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  for (int r = 0; r < repeat; r++) {
    for (int i = 0; i < sessions; i++) {
      test_ctx_tx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_tx[i] != NULL);

      test_ctx_tx[i]->idx = i;
      test_ctx_tx[i]->ctx = ctx;
      test_ctx_tx[i]->fb_cnt = 3;
      test_ctx_tx[i]->fb_idx = 0;
      memset(&ops_tx, 0, sizeof(ops_tx));
      ops_tx.name = "st30_test";
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
      ops_tx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
      ops_tx.type = type[i];
      ops_tx.sampling = sample[i];
      ops_tx.channel = channel[i];
      ops_tx.fmt = fmt[i];
      ops_tx.payload_type = ST30_TEST_PAYLOAD_TYPE;
      ops_tx.ptime = ST30_PTIME_1MS;
      ops_tx.pacing_way = ctx->tx_audio_pacing_way;
      ops_tx.framebuff_size =
          st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_audio_next_frame;
      ops_tx.notify_rtp_done = tx_rtp_done;
      ops_tx.rtp_ring_size = 1024;
      test_ctx_tx[i]->pkt_data_len = ops_tx.framebuff_size;
      tx_handle[i] = st30_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);

      test_ctx_tx[i]->handle = tx_handle[i];

      if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
      ops_rx.name = "st30_test";
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
      ops_rx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
      ops_rx.type = type[i];
      ops_rx.sampling = sample[i];
      ops_rx.channel = channel[i];
      ops_rx.fmt = fmt[i];
      ops_rx.payload_type = ST30_TEST_PAYLOAD_TYPE;
      ops_rx.ptime = ST30_PTIME_1MS;
      ops_rx.framebuff_size =
          st30_get_packet_size(ops_rx.fmt, ops_rx.ptime, ops_rx.sampling, ops_rx.channel);
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
      ops_rx.notify_frame_ready = st30_rx_frame_ready;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;

      rx_handle[i] = st30_rx_create(m_handle, &ops_rx);
      ASSERT_TRUE(rx_handle[i] != NULL);

      if (type[i] == ST30_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = false;
        rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      }

      test_ctx_rx[i]->handle = rx_handle[i];
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
      if (type[i] == ST30_TYPE_RTP_LEVEL) {
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

    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
      ret = st30_tx_free(tx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_tx[i];
      ret = st30_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_rx[i];
    }
  }

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);
}

TEST(St30_rx, after_start_mix_s2_r1) {
  enum st30_type type[2] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[2] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  uint16_t c[2] = {1, 2};
  enum st30_fmt f[2] = {ST30_FMT_PCM24, ST30_FMT_PCM16};
  st30_create_after_start_test(type, s, c, f, 2, 1, ST_TEST_LEVEL_MANDATORY);
}

TEST(St30_rx, after_start_frame_s1_r2) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  uint16_t c[1] = {2};
  enum st30_fmt f[1] = {ST30_FMT_PCM16};
  st30_create_after_start_test(type, s, c, f, 1, 2, ST_TEST_LEVEL_ALL);
}
