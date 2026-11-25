/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

/* st40_pipeline_api.h is not included by tests.hpp, pull it in for st40p + is_late */
#include <mtl/st40_pipeline_api.h>

#define ABORT_TEST_UDP_PORT (40000)
#define EPOCH_DROP_TEST_UDP_PORT (40100)
#define ST40P_TEST_PAYLOAD_TYPE (113)

namespace {

using ScopedSt40pTx = mtl_test::ScopedHandle<st40p_tx_handle, st40p_tx_free>;
using ScopedSt40pRx = mtl_test::ScopedHandle<st40p_rx_handle, st40p_rx_free>;

struct ScopedWorker {
  tests_context* ctx = nullptr;
  std::thread th;
  std::function<void()> wake_fn;

  ScopedWorker() = default;
  ScopedWorker(tests_context* c, std::thread t, std::function<void()> wake = nullptr)
      : ctx(c), th(std::move(t)), wake_fn(std::move(wake)) {
  }
  ~ScopedWorker() {
    stop();
  }
  ScopedWorker(const ScopedWorker&) = delete;
  ScopedWorker& operator=(const ScopedWorker&) = delete;

  void stop() {
    mtl_test::stop_worker(ctx, th, wake_fn);
    ctx = nullptr;
  }
};

struct st40p_tx_ops make_st40p_tx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool redundant = false,
                                      bool mcast_single_port = false,
                                      uint32_t extra_flags = 0) {
  struct st40p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = test_ctx;
  if (mcast_single_port) {
    ops.port.num_port = 1;
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
    ops.port.payload_type = ST40P_TEST_PAYLOAD_TYPE;
  } else {
    mtl_test::fill_tx_port(ops.port, ctx, udp_port, ST40P_TEST_PAYLOAD_TYPE, 0,
                           redundant);
  }
  ops.fps = ST_FPS_P59_94;
  ops.interlaced = false;
  ops.framebuff_cnt = test_ctx->fb_cnt;
  ops.max_udw_buff_size = 1024;
  ops.flags = ST40P_TX_FLAG_BLOCK_GET | extra_flags;
  return ops;
}

struct st40p_rx_ops make_st40p_rx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool redundant = false) {
  struct st40p_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = test_ctx;
  mtl_test::fill_rx_port(ops.port, ctx, udp_port, ST40P_TEST_PAYLOAD_TYPE, 0, redundant);
  ops.interlaced = false;
  ops.framebuff_cnt = test_ctx->fb_cnt;
  ops.max_udw_buff_size = 1024;
  ops.rtp_ring_size = 1024;
  ops.flags = ST40P_RX_FLAG_BLOCK_GET;
  return ops;
}

void st40p_tx_worker(tests_context* s, bool user_pacing) {
  auto handle = (st40p_tx_handle)s->handle;
  auto st = s->ctx->handle;
  while (!s->stop) {
    auto frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) continue;
    /* fill minimal ANC metadata */
    frame_info->meta_num = 0;
    frame_info->udw_buffer_fill = 0;
    if (user_pacing) {
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
      frame_info->timestamp = mtl_ptp_read_time(st) + 20 * 1000 * 1000; /* +20ms */
    }
    st40p_tx_put_frame(handle, frame_info);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
}

void st40p_rx_worker(tests_context* s) {
  auto handle = (st40p_rx_handle)s->handle;
  while (!s->stop) {
    auto frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) continue;
    st40p_rx_put_frame(handle, frame_info);
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
  }
}

void st40p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  auto test_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(test_ctx != nullptr);

  uint32_t extra_flags =
      user_pacing ? (ST40P_TX_FLAG_USER_PACING | ST40P_TX_FLAG_USER_TIMESTAMP) : 0;
  auto ops_tx = make_st40p_tx_ops(ctx, test_ctx.get(), "st40p_epoch_drop_test",
                                  EPOCH_DROP_TEST_UDP_PORT, false, true, extra_flags);

  /* Declare handle before ScopedMtlStart so RAII stops worker -> mtl_stop -> free,
   * while still creating the session after mtl_start (the path under test). */
  ScopedSt40pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_run(st);
  EXPECT_GE(mtl_run.start(), 0);

  tx_handle.reset(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  test_ctx->handle = tx_handle.get();
  test_ctx->stop = false;
  ScopedWorker tx_worker(test_ctx.get(),
                         std::thread(st40p_tx_worker, test_ctx.get(), user_pacing),
                         [&]() { st40p_tx_wake_block(tx_handle.get()); });

  /* Wait for >= 25 paced frames (~0.42-0.50 s at 50-59.94 fps vs legacy sleep(5))
   * to confirm post-start creation incurs zero epoch drops. */
  std::vector<tests_context*> tx_ctxs = {test_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(tx_ctxs, mtl_test::tx_frames, 25, 6000));

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  struct st40_tx_user_stats stats;
  EXPECT_GE(st40p_tx_get_session_stats(tx_handle.get(), &stats), 0);
  EXPECT_GT(mtl_test::tx_frames(test_ctx.get()), 0);
  info("%s, user_pacing %d, fb_send %d, epoch_drop %" PRIu64 ", epoch_mismatch %" PRIu64
       "\n",
       __func__, user_pacing, mtl_test::tx_frames(test_ctx.get()),
       stats.common.stat_epoch_drop, stats.common.stat_epoch_mismatch);
  EXPECT_EQ(stats.common.stat_epoch_drop, 0u);
}

} /* namespace */

TEST(St40p, tx_put_frame_abort) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  auto test_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(test_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, test_ctx.get(), "st40p_abort_test",
                                  ABORT_TEST_UDP_PORT + 12, false, true);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  struct st40_frame_info* frame_info = st40p_tx_get_frame(tx_handle.get());
  ASSERT_TRUE(frame_info != NULL);
  EXPECT_GE(st40p_tx_put_frame_abort(tx_handle.get(), frame_info), 0);
}

TEST(St40p, rx_put_frame_abort) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx =
      make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_abort_tx", ABORT_TEST_UDP_PORT + 14);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  tx_ctx->handle = tx_handle.get();

  auto ops_rx =
      make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_abort_rx", ABORT_TEST_UDP_PORT + 14);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);

  mtl_test::ScopedMtlStart mtl_run(st);
  ScopedWorker tx_worker(tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false),
                         [&]() { st40p_tx_wake_block(tx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for TX to pace >= 15 frames (~0.25 s at 59.94 fps vs legacy sleep(2))
   * before blocking on st40p_rx_get_frame. */
  std::vector<tests_context*> tx_ctxs = {tx_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(tx_ctxs, mtl_test::tx_frames, 15, 5000));

  struct st40_frame_info* frame_info = st40p_rx_get_frame(rx_handle.get());
  EXPECT_TRUE(frame_info != NULL);
  if (frame_info) {
    EXPECT_GE(st40p_rx_put_frame_abort(rx_handle.get(), frame_info), 0);
  }

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);
}

TEST(St40p, frame_is_late) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  struct st40_frame_info frame_info;
  memset(&frame_info, 0, sizeof(frame_info));
  frame_info.tfmt = ST10_TIMESTAMP_FMT_TAI;
  uint64_t now = mtl_ptp_read_time(st);
  frame_info.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st40_frame_is_late(st, &frame_info));

  frame_info.timestamp = now + (uint64_t)10 * NS_PER_S;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));

  frame_info.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame_info.timestamp = 0;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));

  info("%s, st40_frame_is_late tests passed\n", __func__);
}

/*
 * Verify redundant TX and RX port stats for st40p ancillary sessions.
 * TX frames must be counted on both ports.
 * RX bytes must be approximately equal on both ports.
 * Requires 4 ports: TX on ports 0+1, RX on ports 2+3.
 */
TEST(St40p, redundant_stats) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  uint16_t udp_port = ABORT_TEST_UDP_PORT + 100;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_redundant_tx", udp_port,
                                  /*redundant=*/true);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  tx_ctx->handle = tx_handle.get();

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_redundant_rx", udp_port,
                                  /*redundant=*/true);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  rx_ctx->handle = rx_handle.get();

  mtl_test::ScopedMtlStart mtl_run(st);
  ScopedWorker tx_worker(tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false),
                         [&]() { st40p_tx_wake_block(tx_handle.get()); });
  ScopedWorker rx_worker(rx_ctx.get(), std::thread(st40p_rx_worker, rx_ctx.get()),
                         [&]() { st40p_rx_wake_block(rx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait until both redundant RX legs carry traffic, then receive >= 50 frames
   * (~0.83 s at 59.94 fps vs legacy sleep(10)) so a ~100 ms startup skew on
   * leg R cannot drop rx_min_bytes / rx_max_bytes below 80%. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        struct st40_rx_user_stats st_rx;
        if (st40p_rx_get_session_stats(rx_handle.get(), &st_rx) < 0) return false;
        return st_rx.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               st_rx.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      6000));
  int rx_base = mtl_test::rx_frames(rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) - rx_base >= 50; }, 6000));

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  struct st40_tx_user_stats tx_stats;
  EXPECT_GE(st40p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  struct st40_rx_user_stats rx_stats;
  EXPECT_GE(st40p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);

  /* verify TX frames on both ports */
  uint64_t tx_frames_p = tx_stats.common.port[MTL_SESSION_PORT_P].frames;
  uint64_t tx_frames_r = tx_stats.common.port[MTL_SESSION_PORT_R].frames;
  EXPECT_GT(tx_frames_p, (uint64_t)0) << "TX port 0 frames must be > 0";
  EXPECT_GT(tx_frames_r, (uint64_t)0) << "TX port 1 frames must be > 0";
  EXPECT_EQ(tx_frames_p, tx_frames_r)
      << "TX port frames should be equal (port0=" << tx_frames_p
      << " port1=" << tx_frames_r << ")";
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, tx_frames_p,
       tx_frames_r);

  /* verify RX bytes approximately equal on both ports */
  uint64_t rx_bytes_p = rx_stats.common.port[MTL_SESSION_PORT_P].bytes;
  uint64_t rx_bytes_r = rx_stats.common.port[MTL_SESSION_PORT_R].bytes;
  EXPECT_GT(rx_bytes_p, (uint64_t)0) << "RX port 0 bytes must be > 0";
  EXPECT_GT(rx_bytes_r, (uint64_t)0) << "RX port 1 bytes must be > 0";
  uint64_t rx_max_bytes = std::max(rx_bytes_p, rx_bytes_r);
  uint64_t rx_min_bytes = std::min(rx_bytes_p, rx_bytes_r);
  EXPECT_GT(rx_min_bytes, rx_max_bytes * 80 / 100)
      << "RX port bytes should be approximately equal (port0=" << rx_bytes_p
      << " port1=" << rx_bytes_r << ")";
  info("%s, RX bytes port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, rx_bytes_p,
       rx_bytes_r);

  /* verify RX received frames */
  int rx_rec = mtl_test::rx_frames(rx_ctx.get());
  EXPECT_GT(rx_rec, 0) << "Must have received frames";
  info("%s, TX sent %d, RX received %d frames\n", __func__,
       mtl_test::tx_frames(tx_ctx.get()), rx_rec);
}

TEST(St40p, tx_no_epoch_drop) {
  st40p_tx_epoch_drop_test(false);
}

TEST(St40p, tx_user_pacing_no_epoch_drop) {
  st40p_tx_epoch_drop_test(true);
}
