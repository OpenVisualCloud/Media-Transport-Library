/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

#include "st20_common.h"

static void st20_tx_user_pacing_test(int width[], int height[], enum st20_fmt fmt[],
                                     bool user_pacing[], bool user_timestamp[],
                                     enum st_test_level level, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* skip if level is below global */
  if (level < ctx->level) GTEST_SKIP() << "Test level is below the requested level";

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
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

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &sha_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  enum st_fps fps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    if (user_pacing[i])
      expect_framerate[i] = st_frame_rate(fps) / 2;
    else
      expect_framerate[i] = st_frame_rate(fps);
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->user_pacing = user_pacing[i];
    test_ctx_tx[i]->user_timestamp = user_timestamp[i];
    test_ctx_tx[i]->frame_time = (double)NS_PER_S / st_frame_rate(fps);

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
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
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->user_pacing = user_pacing[i];
    test_ctx_rx[i]->user_timestamp = user_timestamp[i];

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps;
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    sha_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    rx_framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    tx_framerate[i] = test_ctx_tx[i]->fb_send / time_sec;
  }

  /* freeze counters before assertions */
  guard.stop();
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

namespace {
/* first-packet arrival minus target TAI of each complete frame, one session */
struct RtpExactPacingRx {
  std::mutex mtx;
  std::vector<int64_t> deltas;
};
RtpExactPacingRx rtp_exact_pacing_rx;

/* frame k carries RTP timestamp k + 1, the feed starting at 1 */
uint64_t rtp_exact_target_tai(tests_context* s, uint32_t rtp_ts) {
  return s->ptp_time_first_frame + (uint64_t)((rtp_ts - 1) * s->frame_time);
}

void tx_feed_packet_tai(void* args) {
  auto ctx = (tests_context*)args;
  void* usrptr = nullptr;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st20_tx_get_mbuf((st20_tx_handle)ctx->handle, &usrptr);
      if (!mbuf) {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
      lck.unlock();
    }
    uint64_t tai = ctx->pkt_idx ? 0 : rtp_exact_target_tai(ctx, ctx->rtp_tmstamp);
    tx_video_build_rtp_packet(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);
    st20_tx_put_mbuf_tai((st20_tx_handle)ctx->handle, mbuf, mbuf_len, tai);
  }
}

int rx_exact_pacing_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  if (!ctx->handle) return -EIO;
  if (st_is_frame_complete(meta->status)) {
    int64_t delta = (int64_t)(meta->timestamp_first_pkt -
                              rtp_exact_target_tai(ctx, meta->rtp_timestamp));
    std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
    rtp_exact_pacing_rx.deltas.push_back(delta);
  }
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  return 0;
}
}  // namespace

/* An RTP-level session with EXACT_USER_PACING sends each frame at the TAI of
 * st20_tx_put_mbuf_tai(): across session restarts, the RX-measured first-packet arrival
 * keeps the same offset from that target, whatever the epoch grid. */
static void st20_tx_rtp_exact_user_pacing_test(int restarts, enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  enum st_fps fps = ST_FPS_P59_94;
  /* off the epoch grid, so a session paced on epochs would not match */
  constexpr uint64_t kTargetLeadNs = 300 * NS_PER_MS + 333 * 1000;
  constexpr int64_t kMaxSpreadNs = 10'000;
  constexpr int64_t kMaxOffsetNs = 100'000;
  constexpr int kRunS = 3; /* ~180 frames a run at 59.94 fps */
  std::vector<int64_t> run_offsets;

  if (level < ctx->level) GTEST_SKIP() << "Test level is below the requested level";
  if (ctx->para.num_ports < 2) {
    info("%s, dual port should be enabled, one for tx and one for rx\n", __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  MtlStopGuard mtl_guard(m_handle);
  ASSERT_GE(mtl_start(m_handle), 0);
  mtl_guard.started = true;

  for (int r = 0; r < restarts; r++) {
    std::vector<tests_context*> test_ctx_tx(1);
    std::vector<tests_context*> test_ctx_rx(1);
    std::vector<st20_tx_handle> tx_handle(1);
    std::vector<st20_rx_handle> rx_handle(1);
    std::vector<std::thread> rtp_thread_tx(1);
    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx);
    struct st20_tx_ops ops_tx;
    struct st20_rx_ops ops_rx;

    {
      std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
      rtp_exact_pacing_rx.deltas.clear();
    }

    test_ctx_rx[0] = init_test_ctx(ctx, 0, 3);
    ASSERT_TRUE(test_ctx_rx[0] != nullptr);
    init_single_port_rx(ops_rx, test_ctx_rx[0], "st20_rtp_exact_pacing",
                        udp_port_for_idx(0));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = 1280;
    ops_rx.height = 720;
    ops_rx.fps = fps;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[0]->fb_cnt;
    ops_rx.notify_frame_ready = rx_exact_pacing_frame_ready;
    rx_handle[0] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[0] != nullptr);

    test_ctx_tx[0] = init_test_ctx(ctx, 0, 3);
    ASSERT_TRUE(test_ctx_tx[0] != nullptr);
    init_single_port_tx(ops_tx, test_ctx_tx[0], "st20_rtp_exact_pacing",
                        udp_port_for_idx(0));
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.width = 1280;
    ops_tx.height = 720;
    ops_tx.fps = fps;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.flags = ST20_TX_FLAG_USER_PACING | ST20_TX_FLAG_EXACT_USER_PACING |
                   ST20_TX_FLAG_USER_TIMESTAMP;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[0]);
    tx_handle[0] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[0] != nullptr);

    uint64_t base = mtl_ptp_read_time(m_handle) + kTargetLeadNs;
    for (tests_context* c : {test_ctx_tx[0], test_ctx_rx[0]}) {
      c->ptp_time_first_frame = base;
      c->frame_time = (double)NS_PER_S / st_frame_rate(fps);
    }
    test_ctx_tx[0]->rtp_tmstamp = 1;
    test_ctx_rx[0]->handle = rx_handle[0];
    test_ctx_tx[0]->handle = tx_handle[0];
    test_ctx_tx[0]->stop = false;
    rtp_thread_tx[0] = std::thread(tx_feed_packet_tai, test_ctx_tx[0]);

    sleep(kRunS);
    struct st20_tx_user_stats stats;
    ASSERT_EQ(st20_tx_get_session_stats(tx_handle[0], &stats), 0);
    guard.stop();
    /* a frame that fell back to the default pacing would skew the offsets */
    EXPECT_EQ(stats.common.stat_error_user_timestamp, 0u);

    std::vector<int64_t> deltas;
    {
      std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
      deltas = rtp_exact_pacing_rx.deltas;
    }
    ASSERT_GT(deltas.size(), (size_t)st_frame_rate(fps));
    std::nth_element(deltas.begin(),
                     deltas.begin() + static_cast<std::ptrdiff_t>(deltas.size() / 2),
                     deltas.end());
    int64_t median = deltas[deltas.size() / 2];
    info("%s, run %d frames %zu median arrival - target %" PRId64 "ns\n", __func__, r,
         deltas.size(), median);
    /* a constant bias, such as tr_offset (~1ms at 720p) or the warm-up lead added to
     * the TAI, keeps the spread small; measured ~7-11us on E835 VFs */
    EXPECT_LT(std::abs(median), kMaxOffsetNs);
    run_offsets.push_back(median);
  }

  auto minmax = std::minmax_element(run_offsets.begin(), run_offsets.end());
  EXPECT_LE(*minmax.second - *minmax.first, kMaxSpreadNs);
}

TEST(St20_tx, rtp_exact_user_pacing_repeatable) {
  st20_tx_rtp_exact_user_pacing_test(3, ST_TEST_LEVEL_ALL);
}
