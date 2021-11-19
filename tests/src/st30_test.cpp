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

static int tx_audio_build_rtp_packet(struct tests_context* s,
                                     struct st30_rfc3550_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->csrc_count = 0;
  rtp->extension = 0;
  rtp->padding = 0;
  rtp->version = 2;
  rtp->marker = 0;
  rtp->payload_type = 111;
  rtp->ssrc = htonl(0x66666666 + s->idx);
  rtp->tmstamp = s->rtp_tmstamp;
  s->rtp_tmstamp++;
  rtp->seq_number = htons(s->seq_id);
  s->seq_id++;
  *pkt_len = sizeof(struct st30_rfc3550_rtp_hdr) + s->pkt_data_len;
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
    mbuf = st30_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st30_tx_get_mbuf(ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_audio_build_rtp_packet(ctx, (struct st30_rfc3550_rtp_hdr*)usrptr, &mbuf_len);
    st30_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
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
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st30_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st30_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    ctx->fb_rec++;
    st30_rx_put_mbuf(ctx->handle, mbuf);
  }
}

static int rx_frame_ready(void* priv, void* frame) {
  auto ctx = (struct tests_context*)priv;
  st30_rx_put_framebuff((st30_rx_handle)ctx->handle, frame);
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void st30_rx_ops_init(struct tests_context* st30, struct st30_rx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 20000 + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 20000 + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = ST30_CHAN_STEREO;
  ops->fmt = ST30_FMT_PCM16;
  ops->sampling = ST30_SAMPLING_48K;
  ops->sample_size = st30_get_sample_size(ops->fmt, ops->channel, ops->sampling);
  ops->framebuff_cnt = st30->fb_cnt;
  ops->framebuff_size = ops->sample_size;
  ops->notify_frame_ready = rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st30_tx_ops_init(struct tests_context* st30, struct st30_tx_ops* ops) {
  auto ctx = st30->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st30_test";
  ops->priv = st30;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 20000 + st30->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 20000 + st30->idx;
  }
  ops->type = ST30_TYPE_FRAME_LEVEL;
  ops->channel = ST30_CHAN_STEREO;
  ops->fmt = ST30_FMT_PCM16;
  ops->sampling = ST30_SAMPLING_48K;
  ops->sample_size = st30_get_sample_size(ops->fmt, ops->channel, ops->sampling);
  ops->framebuff_cnt = st30->fb_cnt;
  ops->framebuff_size = ops->sample_size;
  ops->get_next_frame = tx_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
  st30->pkt_data_len = ops->sample_size;
}

static void st30_tx_assert_cnt(int expect_s30_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st30_tx_sessions_cnt, expect_s30_tx_cnt);
}

static void st30_rx_assert_cnt(int expect_s30_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st30_rx_sessions_cnt, expect_s30_rx_cnt);
}

TEST(St30_tx, create_free_single) { create_free_test(st30_tx, 0, 1, 1); }
TEST(St30_tx, create_free_multi) { create_free_test(st30_tx, 0, 1, 6); }
TEST(St30_tx, create_free_mix) { create_free_test(st30_tx, 2, 3, 4); }
TEST(St30_tx, create_free_max) { create_free_max(st30_tx, 100); }
TEST(St30_tx, create_expect_fail) { expect_fail_test(st30_tx); }
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

TEST(St30_rx, create_free_single) { create_free_test(st30_rx, 0, 1, 1); }
TEST(St30_rx, create_free_multi) { create_free_test(st30_rx, 0, 1, 6); }
TEST(St30_rx, create_free_mix) { create_free_test(st30_rx, 2, 3, 4); }
TEST(St30_rx, create_free_max) { create_free_max(st30_rx, 100); }
TEST(St30_rx, create_expect_fail) { expect_fail_test(st30_rx); }
TEST(St30_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st30_rx, ST30_TYPE_RTP_LEVEL, ring_size);
}

static void st30_tx_fps_test(enum st30_type type[], enum st30_sampling sample[],
                             enum st30_channel channel[], enum st30_fmt fmt[],
                             int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops;
  struct tests_context* test_ctx[sessions];
  st30_tx_handle handle[sessions];
  double expect_framerate = 1000.0;
  double framerate[sessions];
  std::thread rtp_thread[sessions];
  for (int i = 0; i < sessions; i++) {
    test_ctx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
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
    ops.sample_size = st30_get_sample_size(ops.fmt, ops.channel, ops.sampling);
    ops.framebuff_size = ops.sample_size;

    handle[i] = st30_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);
    test_ctx[i]->handle = handle[i];
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    test_ctx[i]->stop = true;
    if (type[i] == ST30_TYPE_RTP_LEVEL) rtp_thread[i].join();
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx[i]->fb_send > 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
    ret = st30_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx[i]);
  }
}

static void st30_rx_fps_test(enum st30_type type[], enum st30_sampling sample[],
                             enum st30_channel channel[], enum st30_fmt fmt[],
                             int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st30_tx_ops ops_tx;
  struct st30_rx_ops ops_rx;
  std::thread rtp_thread_tx[sessions];
  std::thread rtp_thread_rx[sessions];
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  struct tests_context* test_ctx_tx[sessions];
  struct tests_context* test_ctx_rx[sessions];
  st30_tx_handle tx_handle[sessions];
  st30_rx_handle rx_handle[sessions];
  double expect_framerate = 1000.0;
  double framerate[sessions];

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = (struct tests_context*)st_test_zmalloc(sizeof(struct tests_context));
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st30_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 20000 + i;
    ops_tx.type = type[i];
    ops_tx.sampling = sample[i];
    ops_tx.channel = channel[i];
    ops_tx.fmt = fmt[i];
    ops_tx.sample_size =
        st30_get_sample_size(ops_tx.fmt, ops_tx.channel, ops_tx.sampling);
    ops_tx.framebuff_size = ops_tx.sample_size;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    ops_tx.notify_rtp_done = tx_rtp_done;
    ops_tx.rtp_ring_size = 1024;
    tx_handle[i] = st30_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->handle = tx_handle[i];
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    ops_rx.name = "st30_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 20000 + i;
    ops_rx.type = type[i];
    ops_rx.sampling = sample[i];
    ops_rx.channel = channel[i];
    ops_rx.fmt = fmt[i];
    ops_rx.sample_size =
        st30_get_sample_size(ops_tx.fmt, ops_tx.channel, ops_tx.sampling);
    ops_rx.framebuff_size = ops_rx.sample_size;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;

    rx_handle[i] = st30_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    if (type[i] == ST30_TYPE_RTP_LEVEL) {
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
    EXPECT_NEAR(framerate[i], expect_framerate, expect_framerate * 0.1);
    ret = st30_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st30_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx_tx[i]);
    st_test_free(test_ctx_rx[i]);
  }
}

TEST(St30_tx, frame_48k_mono_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_channel c[1] = {ST30_CHAN_MONO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, c, &f[i]);
}
TEST(St30_tx, frame_96k_mono_s1) {
  enum st30_type type[1] = {ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  enum st30_channel c[1] = {ST30_CHAN_MONO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, c, &f[i]);
}
TEST(St30_tx, frame_48k_stereo_s1) {
  enum st30_type type[1] = {ST30_TYPE_FRAME_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_48K};
  enum st30_channel c[1] = {ST30_CHAN_STEREO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, c, &f[i]);
}
TEST(St30_tx, frame_96k_stereo_s1) {
  enum st30_type type[1] = {ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[1] = {ST30_SAMPLING_96K};
  enum st30_channel c[1] = {ST30_CHAN_STEREO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  for (int i = 0; i < 3; i++) st30_tx_fps_test(type, s, c, &f[i]);
}
TEST(St30_tx, frame_96k_stereo_s3) {
  enum st30_type type[3] = {ST30_TYPE_RTP_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_96K, ST30_SAMPLING_96K};
  enum st30_channel c[3] = {ST30_CHAN_STEREO, ST30_CHAN_STEREO, ST30_CHAN_STEREO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_tx_fps_test(type, s, c, f, 3);
}
TEST(St30_tx, frame_48k_96_mix) {
  enum st30_type type[3] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_channel c[3] = {ST30_CHAN_STEREO, ST30_CHAN_MONO, ST30_CHAN_STEREO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_tx_fps_test(type, s, c, f, 3);
}
TEST(St30_rx, frame_48k_96_mix) {
  enum st30_type type[3] = {ST30_TYPE_FRAME_LEVEL, ST30_TYPE_FRAME_LEVEL,
                            ST30_TYPE_RTP_LEVEL};
  enum st30_sampling s[3] = {ST30_SAMPLING_96K, ST30_SAMPLING_48K, ST30_SAMPLING_48K};
  enum st30_channel c[3] = {ST30_CHAN_STEREO, ST30_CHAN_MONO, ST30_CHAN_STEREO};
  enum st30_fmt f[3] = {ST30_FMT_PCM8, ST30_FMT_PCM16, ST30_FMT_PCM24};
  st30_rx_fps_test(type, s, c, f, 3);
}