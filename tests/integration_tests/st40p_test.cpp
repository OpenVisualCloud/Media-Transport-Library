/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <atomic>
#include <chrono>
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

struct st40p_tx_ops make_st40p_tx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool redundant = false,
                                      bool mcast_single_port = false,
                                      uint32_t extra_flags = 0,
                                      uint32_t max_udw_buff_size = 1024) {
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
  ops.max_udw_buff_size = max_udw_buff_size;
  ops.flags = ST40P_TX_FLAG_BLOCK_GET | extra_flags;
  return ops;
}

struct st40p_rx_ops make_st40p_rx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool redundant = false,
                                      uint32_t max_udw_buff_size = 1024) {
  struct st40p_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = test_ctx;
  mtl_test::fill_rx_port(ops.port, ctx, udp_port, ST40P_TEST_PAYLOAD_TYPE, 0, redundant);
  ops.interlaced = false;
  ops.framebuff_cnt = test_ctx->fb_cnt;
  ops.max_udw_buff_size = max_udw_buff_size;
  ops.rtp_ring_size = 1024;
  ops.flags = ST40P_RX_FLAG_BLOCK_GET;
  return ops;
}

void st40p_tx_ops_init(tests_context* st40, struct st40p_tx_ops* ops_tx) {
  *ops_tx = make_st40p_tx_ops(st40->ctx, st40, "st40p_test_tx",
                              ABORT_TEST_UDP_PORT + st40->idx, /*redundant=*/false,
                              /*mcast_single_port=*/true);
}

void st40p_rx_ops_init(tests_context* st40, struct st40p_rx_ops* ops_rx) {
  auto* ctx = st40->ctx;
  memset(ops_rx, 0, sizeof(*ops_rx));
  ops_rx->name = "st40p_test_rx";
  ops_rx->priv = st40;
  ops_rx->port.num_port = 1;
  memcpy(ops_rx->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  const enum mtl_port rx_port = (ctx->para.num_ports >= 2) ? MTL_PORT_R : MTL_PORT_P;
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[rx_port]);
  ops_rx->port.udp_port[MTL_SESSION_PORT_P] = ABORT_TEST_UDP_PORT + st40->idx;
  ops_rx->port.payload_type = ST40P_TEST_PAYLOAD_TYPE;
  ops_rx->interlaced = false;
  ops_rx->framebuff_cnt = st40->fb_cnt;
  ops_rx->max_udw_buff_size = 1024;
  ops_rx->flags = ST40P_RX_FLAG_BLOCK_GET;
}

void st40p_tx_assert_cnt(int expect_cnt) {
  auto* ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st40_tx_sessions_cnt, expect_cnt);
}

void st40p_rx_assert_cnt(int expect_cnt) {
  auto* ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st40_rx_sessions_cnt, expect_cnt);
}

int test_st40p_frame_available(void* priv) {
  auto* s = static_cast<tests_context*>(priv);
  std::unique_lock<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

int test_st40p_tx_frame_done(void* priv, struct st40_frame_info* fi) {
  auto* s = static_cast<tests_context*>(priv);
  auto handle = (st40p_tx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return -EIO;
  if (!fi || fi->status != ST_FRAME_STATUS_COMPLETE) {
    __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);
  return 0;
}

void st40p_tx_worker(tests_context* s, bool user_pacing, bool with_anc_payload = false) {
  auto handle = (st40p_tx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  auto st = s->ctx->handle;
  while (!s->stop) {
    auto frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) continue;
    if (with_anc_payload && frame_info->udw_buff_addr &&
        frame_info->udw_buffer_size >= 16) {
      constexpr uint16_t kUdwLen = 16;
      frame_info->meta_num = 1;
      memset(&frame_info->meta[0], 0, sizeof(frame_info->meta[0]));
      frame_info->meta[0].line_number = 10;
      frame_info->meta[0].did = 0x43;
      frame_info->meta[0].sdid = 0x02;
      frame_info->meta[0].udw_offset = 0;
      frame_info->meta[0].udw_size = kUdwLen;
      for (uint16_t i = 0; i < kUdwLen; i++) {
        frame_info->udw_buff_addr[i] = static_cast<uint8_t>(0xA0 + i);
      }
      frame_info->udw_buffer_fill = kUdwLen;
    } else {
      /* fill minimal ANC metadata */
      frame_info->meta_num = 0;
      frame_info->udw_buffer_fill = 0;
    }
    if (user_pacing) {
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
      frame_info->timestamp = mtl_ptp_read_time(st) + 20 * 1000 * 1000; /* +20ms */
    }
    st40p_tx_put_frame(handle, frame_info);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
}

void st40p_rx_worker(tests_context* s) {
  auto handle = (st40p_rx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  while (!s->stop) {
    auto frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) continue;
    st40p_rx_put_frame(handle, frame_info);
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
  }
}

void st40p_tx_non_block_worker(tests_context* s, uint16_t udw_len, uint16_t did,
                               uint16_t sdid, uint16_t line_number,
                               uint16_t hori_offset) {
  auto handle = (st40p_tx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  while (!s->stop) {
    auto* frame_info = st40p_tx_get_frame(handle);
    if (!frame_info) {
      std::unique_lock<std::mutex> lck(s->mtx);
      if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
      continue;
    }
    if (frame_info->meta && frame_info->udw_buffer_size >= udw_len) {
      frame_info->meta_num = 1;
      memset(&frame_info->meta[0], 0, sizeof(frame_info->meta[0]));
      frame_info->meta[0].did = did;
      frame_info->meta[0].sdid = sdid;
      frame_info->meta[0].line_number = line_number;
      frame_info->meta[0].hori_offset = hori_offset;
      frame_info->meta[0].udw_offset = 0;
      frame_info->meta[0].udw_size = udw_len;
      frame_info->udw_buffer_fill = udw_len;
    } else {
      frame_info->meta_num = 0;
      frame_info->udw_buffer_fill = 0;
    }
    st40p_tx_put_frame(handle, frame_info);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
}

void st40p_rx_non_block_worker(tests_context* s, uint16_t udw_len, uint16_t did,
                               uint16_t sdid, uint16_t line_number,
                               uint16_t hori_offset) {
  auto handle = (st40p_rx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  while (!s->stop) {
    auto* frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) {
      std::unique_lock<std::mutex> lck(s->mtx);
      if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
      continue;
    }
    if (frame_info->status != ST_FRAME_STATUS_COMPLETE) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    bool meta_ok = frame_info->meta != nullptr && frame_info->meta_num == 1 &&
                   frame_info->udw_buffer_fill == udw_len &&
                   frame_info->meta[0].did == did && frame_info->meta[0].sdid == sdid &&
                   frame_info->meta[0].line_number == line_number &&
                   frame_info->meta[0].hori_offset == hori_offset &&
                   frame_info->meta[0].udw_size == udw_len &&
                   frame_info->udw_buff_addr != nullptr;
    if (!meta_ok) {
      __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame_info->udw_buff_addr && frame_info->udw_buffer_fill == udw_len) {
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256(frame_info->udw_buff_addr, udw_len, digest);
      bool matched = false;
      for (int i = 0; i < TEST_MAX_SHA_HIST_NUM; i++) {
        if (memcmp(digest, s->shas[i], SHA256_DIGEST_LENGTH) == 0) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        __atomic_fetch_add(&s->sha_fail_cnt, 1, __ATOMIC_RELAXED);
      }
      __atomic_fetch_add(&s->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
    }
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

  __atomic_store_n(&test_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);
  test_ctx->stop = false;
  mtl_test::ScopedWorker tx_worker(
      test_ctx.get(), std::thread(st40p_tx_worker, test_ctx.get(), user_pacing, false),
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
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx =
      make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_abort_rx", ABORT_TEST_UDP_PORT + 14);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);

  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false, false),
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
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_redundant_rx", udp_port,
                                  /*redundant=*/true);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false, false),
      [&]() { st40p_tx_wake_block(tx_handle.get()); });
  mtl_test::ScopedWorker rx_worker(rx_ctx.get(),
                                   std::thread(st40p_rx_worker, rx_ctx.get()),
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

  mtl_test::expect_redundant_port_stats(tx_stats, rx_stats);
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 ", RX bytes port0=%" PRIu64
       " port1=%" PRIu64 "\n",
       __func__, tx_stats.common.port[MTL_SESSION_PORT_P].frames,
       tx_stats.common.port[MTL_SESSION_PORT_R].frames,
       rx_stats.common.port[MTL_SESSION_PORT_P].bytes,
       rx_stats.common.port[MTL_SESSION_PORT_R].bytes);

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

/* Verify st40p TX buffer size and per-slot address introspection APIs. */
TEST(St40p, tx_introspection) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  constexpr uint16_t kFbCnt = 4;
  constexpr uint32_t kMaxUdwSize = 2048;
  auto tx_ctx = mtl_test::make_context(ctx, 0, kFbCnt);
  ASSERT_TRUE(tx_ctx != nullptr);

  auto ops_tx =
      make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_tx_introspect",
                        ABORT_TEST_UDP_PORT + 20, /*redundant=*/false,
                        /*mcast_single_port=*/true, /*extra_flags=*/0, kMaxUdwSize);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);

  EXPECT_EQ(st40p_tx_max_udw_buff_size(tx_handle.get()), kMaxUdwSize);

  std::vector<void*> seen_udw;
  std::vector<void*> seen_fb;
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    void* udw = st40p_tx_get_udw_buff_addr(tx_handle.get(), idx);
    auto* fb =
        static_cast<struct st40_frame*>(st40p_tx_get_fb_addr(tx_handle.get(), idx));
    ASSERT_NE(udw, nullptr);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->data, udw);
    EXPECT_EQ(std::find(seen_udw.begin(), seen_udw.end(), udw), seen_udw.end());
    EXPECT_EQ(std::find(seen_fb.begin(), seen_fb.end(), fb), seen_fb.end());
    seen_udw.push_back(udw);
    seen_fb.push_back(fb);
  }

  /* Out-of-range indices must return nullptr. */
  EXPECT_EQ(st40p_tx_get_udw_buff_addr(tx_handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st40p_tx_get_udw_buff_addr(tx_handle.get(), kFbCnt * 2), nullptr);
  EXPECT_EQ(st40p_tx_get_fb_addr(tx_handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st40p_tx_get_fb_addr(tx_handle.get(), kFbCnt * 2), nullptr);
}

/* Verify st40p RX buffer size, queue meta, and live slot UDW buffer address APIs. */
TEST(St40p, rx_introspection_and_queue_meta) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kUdpPort = ABORT_TEST_UDP_PORT + 22;
  constexpr uint16_t kRxFbCnt = 4;
  constexpr uint32_t kRxMaxUdwSize = 1536;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, kRxFbCnt);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_rx_introspect_tx", kUdpPort);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_rx_introspect_rx", kUdpPort,
                                  /*redundant=*/false, kRxMaxUdwSize);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), 2 * NS_PER_S), 0);

  EXPECT_EQ(st40p_rx_max_udw_buff_size(rx_handle.get()), kRxMaxUdwSize);

  struct st_queue_meta qmeta;
  memset(&qmeta, 0, sizeof(qmeta));
  EXPECT_GE(st40p_rx_get_queue_meta(rx_handle.get(), &qmeta), 0);
  EXPECT_EQ(qmeta.num_port, 1);

  /* Out-of-range indices must return nullptr. */
  EXPECT_EQ(st40p_rx_get_udw_buff_addr(rx_handle.get(), kRxFbCnt), nullptr);
  EXPECT_EQ(st40p_rx_get_udw_buff_addr(rx_handle.get(), kRxFbCnt * 2), nullptr);

  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false, true),
      [&]() { st40p_tx_wake_block(tx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for TX to pace >= 10 frames (~0.17 s at 59.94 fps) then receive one. */
  std::vector<tests_context*> tx_ctxs = {tx_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(tx_ctxs, mtl_test::tx_frames, 10, 5000));

  struct st40_frame_info* frame_info = st40p_rx_get_frame(rx_handle.get());
  ASSERT_NE(frame_info, nullptr);
  EXPECT_NE(frame_info->udw_buff_addr, nullptr);
  EXPECT_EQ(frame_info->udw_buffer_size, kRxMaxUdwSize);
  EXPECT_EQ(frame_info->meta_num, 1u);
  EXPECT_EQ(frame_info->udw_buffer_fill, 16u);

  /* Zero-copy RX binds the transport UDW buffer onto the active pipeline slot. */
  bool found_slot_addr = false;
  for (uint16_t idx = 0; idx < kRxFbCnt; idx++) {
    if (st40p_rx_get_udw_buff_addr(rx_handle.get(), idx) == frame_info->udw_buff_addr) {
      found_slot_addr = true;
      break;
    }
  }
  EXPECT_TRUE(found_slot_addr);
  EXPECT_GE(st40p_rx_put_frame(rx_handle.get(), frame_info), 0);

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);
}

/* Verify st40p TX and RX session stats accumulate and reset cleanly. */
TEST(St40p, session_stats_reset) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kUdpPort = ABORT_TEST_UDP_PORT + 24;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_stats_tx", kUdpPort);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_stats_rx", kUdpPort);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false, true),
      [&]() { st40p_tx_wake_block(tx_handle.get()); });
  mtl_test::ScopedWorker rx_worker(rx_ctx.get(),
                                   std::thread(st40p_rx_worker, rx_ctx.get()),
                                   [&]() { st40p_rx_wake_block(rx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* 15 frames at 59.94 fps (~0.25 s) populates both TX and RX stats. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= 15; }, 6000));

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;
  ASSERT_GE(st40p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  ASSERT_GE(st40p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(rx_stats.common.stat_frames_received, 0u);

  EXPECT_GE(st40p_tx_reset_session_stats(tx_handle.get()), 0);
  EXPECT_GE(st40p_rx_reset_session_stats(rx_handle.get()), 0);

  ASSERT_GE(st40p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  ASSERT_GE(st40p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_EQ(tx_stats.common.stat_frames_dropped, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.stat_frames_received, 0u);
  EXPECT_EQ(rx_stats.common.stat_frames_dropped, 0u);

  /* Invalid argument handling. */
  EXPECT_LT(st40p_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st40p_tx_get_session_stats(tx_handle.get(), nullptr), 0);
  EXPECT_LT(st40p_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st40p_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st40p_rx_get_session_stats(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st40p_rx_reset_session_stats(nullptr), 0);
}

/* Verify online st40p_tx_update_destination and st40p_rx_update_source switching. */
TEST(St40p, update_dest_and_source) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kBasePort = ABORT_TEST_UDP_PORT + 30;
  const double expect_fps = st_frame_rate(ST_FPS_P59_94);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_switch_tx", kBasePort);
  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_switch_rx", kBasePort);
  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  EXPECT_EQ(st40p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_worker, tx_ctx.get(), false, true),
      [&]() { st40p_tx_wake_block(tx_handle.get()); });
  mtl_test::ScopedWorker rx_worker(rx_ctx.get(),
                                   std::thread(st40p_rx_worker, rx_ctx.get()),
                                   [&]() { st40p_rx_wake_block(rx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for initial stream (>= 15 frames ~0.25 s at 59.94 fps) before switching. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= 15; }, 6000));

  auto verify_switch = [&](const uint8_t* tx_dip, const uint8_t* rx_ip, uint16_t udp_port,
                           const char* label) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_dip, MTL_IP_ADDR_LEN);
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
    EXPECT_GE(st40p_tx_update_destination(tx_handle.get(), &dst), 0) << label;

    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_ip, MTL_IP_ADDR_LEN);
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    EXPECT_GE(st40p_rx_update_source(rx_handle.get(), &src), 0) << label;

    const int baseline = mtl_test::rx_frames(rx_ctx.get());
    std::vector<tests_context*> rx_ctxs = {rx_ctx.get()};
    auto delta_rx = [&](tests_context* c) { return mtl_test::rx_frames(c) - baseline; };
    /* 1.0 s window with >= 25 frames after 3-frame warmup: <= 1/59 (1.7%) edge
     * quantization error at 59.94 fps vs +/-10% tolerance. */
    auto win = mtl_test::measure_rate_window(rx_ctxs, delta_rx, /*warmup_frames=*/3,
                                             /*min_delta_frames=*/25,
                                             /*min_window_ms=*/1000);
    EXPECT_TRUE(win.warmed_up) << label;
    EXPECT_TRUE(win.complete) << label;
    EXPECT_GT(delta_rx(rx_ctx.get()), 0) << label;
    EXPECT_NEAR(win.rate[0], expect_fps, expect_fps * 0.1) << label;
  };

  /* Phase 1: switch TX dest and RX source to multicast on kBasePort + 2. */
  verify_switch(ctx->mcast_ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
                kBasePort + 2, "mcast R");

  /* Phase 2: switch back to initial destination and source on kBasePort. */
  verify_switch(ops_tx.port.dip_addr[MTL_SESSION_PORT_P],
                ops_rx.port.ip_addr[MTL_SESSION_PORT_P], kBasePort, "initial");

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);
}

TEST(St40p, tx_create_free_single) {
  mtl_test::verify_create_free<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                               st40p_tx_free>(st40p_tx_ops_init, st40p_tx_assert_cnt, 0,
                                              1, 1);
}
TEST(St40p, tx_create_free_multi) {
  mtl_test::verify_create_free<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                               st40p_tx_free>(st40p_tx_ops_init, st40p_tx_assert_cnt, 0,
                                              1, 6);
}
TEST(St40p, tx_create_free_mix) {
  mtl_test::verify_create_free<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                               st40p_tx_free>(st40p_tx_ops_init, st40p_tx_assert_cnt, 2,
                                              3, 4);
}
TEST(St40p, tx_create_free_max) {
  mtl_test::verify_create_free_max<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                                   st40p_tx_free>(st40p_tx_ops_init, st40p_tx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}

TEST(St40p, rx_create_free_single) {
  mtl_test::verify_create_free<struct st40p_rx_ops, st40p_rx_handle, st40p_rx_create,
                               st40p_rx_free>(st40p_rx_ops_init, st40p_rx_assert_cnt, 0,
                                              1, 1);
}
TEST(St40p, rx_create_free_multi) {
  mtl_test::verify_create_free<struct st40p_rx_ops, st40p_rx_handle, st40p_rx_create,
                               st40p_rx_free>(st40p_rx_ops_init, st40p_rx_assert_cnt, 0,
                                              1, 6);
}
TEST(St40p, rx_create_free_mix) {
  mtl_test::verify_create_free<struct st40p_rx_ops, st40p_rx_handle, st40p_rx_create,
                               st40p_rx_free>(st40p_rx_ops_init, st40p_rx_assert_cnt, 2,
                                              3, 4);
}
TEST(St40p, rx_create_free_max) {
  mtl_test::verify_create_free_max<struct st40p_rx_ops, st40p_rx_handle, st40p_rx_create,
                                   st40p_rx_free>(st40p_rx_ops_init, st40p_rx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}

TEST(St40p, tx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                                 st40p_tx_free>(st40p_tx_ops_init);

  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

  struct st40p_tx_ops ops_base;
  st40p_tx_ops_init(test_ctx.get(), &ops_base);

  EXPECT_EQ(st40p_tx_create(st, nullptr), nullptr);
  EXPECT_EQ(st40p_tx_create(nullptr, &ops_base), nullptr);

  auto expect_fail = [&](const std::function<void(struct st40p_tx_ops&)>& mutate,
                         const char* label) {
    struct st40p_tx_ops ops = ops_base;
    mutate(ops);
    ScopedSt40pTx h(st40p_tx_create(st, &ops));
    EXPECT_FALSE(static_cast<bool>(h)) << label;
  };

  expect_fail([](auto& o) { o.fps = ST_FPS_MAX; }, "invalid fps");
  expect_fail([](auto& o) { o.max_udw_buff_size = 0; }, "zero max_udw_buff_size");
  expect_fail([](auto& o) { o.framebuff_cnt = 0; }, "zero framebuff_cnt");
  expect_fail([](auto& o) { o.flags |= ST40P_TX_FLAG_FORCE_NUMA; }, "force numa");
  expect_fail(
      [](auto& o) {
        snprintf(o.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", "invalid_port");
      },
      "invalid primary port");
}

TEST(St40p, rx_create_expect_fail) {
  /* Do not call verify_invalid_ports here:
   * lib/src/st2110/pipeline/st40_pipeline_rx.c:159-174 clamps ops->port.num_port to
   * MTL_SESSION_PORT_MAX and calls mt_port_id(impl, MTL_PORT_MAX) when port[1] is empty.
   * Test safe single/zero-port rejections only. */
  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

  struct st40p_rx_ops ops_base;
  st40p_rx_ops_init(test_ctx.get(), &ops_base);

  EXPECT_EQ(st40p_rx_create(st, nullptr), nullptr);
  EXPECT_EQ(st40p_rx_create(nullptr, &ops_base), nullptr);

  auto expect_fail = [&](const std::function<void(struct st40p_rx_ops&)>& mutate,
                         const char* label) {
    struct st40p_rx_ops ops = ops_base;
    mutate(ops);
    ScopedSt40pRx h(st40p_rx_create(st, &ops));
    EXPECT_FALSE(static_cast<bool>(h)) << label;
  };

  expect_fail([](auto& o) { o.port.num_port = 0; }, "zero num_port");
  expect_fail([](auto& o) { o.max_udw_buff_size = 0; }, "zero max_udw_buff_size");
  expect_fail([](auto& o) { o.framebuff_cnt = 0; }, "zero framebuff_cnt");
  expect_fail([](auto& o) { o.flags |= ST40P_RX_FLAG_FORCE_NUMA; }, "force numa");
  expect_fail(
      [](auto& o) {
        snprintf(o.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", "invalid_port");
      },
      "invalid primary port");
}

/* End-to-end non-blocking st40p TX/RX test with notify_frame_available,
 * notify_frame_done, ANC metadata validation, SHA256 digest check, and FPS rate check. */
TEST(St40p, digest_non_block) {
  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kFbCnt = 3;
  constexpr uint16_t kUdwLen = 240;
  constexpr uint16_t kDid = 0x43;
  constexpr uint16_t kSdid = 0x02;
  constexpr uint16_t kLine = 10;
  constexpr uint16_t kHoriOffset = 0x10;
  constexpr uint16_t kUdpPort = ABORT_TEST_UDP_PORT + 40;
  const double expect_fps = st_frame_rate(ST_FPS_P59_94);

  auto tx_ctx = mtl_test::make_context(ctx, 0, kFbCnt, /*check_sha=*/true);
  auto rx_ctx = mtl_test::make_context(ctx, 0, kFbCnt, /*check_sha=*/true);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  auto ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), "st40p_digest_nb_tx", kUdpPort);
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.flags &= ~ST40P_TX_FLAG_BLOCK_GET;
  ops_tx.notify_frame_available = test_st40p_frame_available;
  ops_tx.notify_frame_done = test_st40p_tx_frame_done;

  ScopedSt40pTx tx_handle(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);

  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    auto* udw = static_cast<uint8_t*>(st40p_tx_get_udw_buff_addr(tx_handle.get(), idx));
    ASSERT_NE(udw, nullptr);
    st_test_rand_data(udw, kUdwLen, idx);
    SHA256(udw, kUdwLen, tx_ctx->shas[idx]);
  }
  memcpy(rx_ctx->shas, tx_ctx->shas, kFbCnt * SHA256_DIGEST_LENGTH);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  auto ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), "st40p_digest_nb_rx", kUdpPort);
  ops_rx.flags &= ~ST40P_RX_FLAG_BLOCK_GET;
  ops_rx.notify_frame_available = test_st40p_frame_available;

  ScopedSt40pRx rx_handle(st40p_rx_create(st, &ops_rx));
  ASSERT_TRUE(rx_handle);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  /* Strict RAII teardown order: workers -> mtl_run -> handles -> contexts. */
  mtl_test::ScopedMtlStart mtl_run(st);
  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(), std::thread(st40p_tx_non_block_worker, tx_ctx.get(), kUdwLen, kDid,
                                kSdid, kLine, kHoriOffset));
  mtl_test::ScopedWorker rx_worker(
      rx_ctx.get(), std::thread(st40p_rx_non_block_worker, rx_ctx.get(), kUdwLen, kDid,
                                kSdid, kLine, kHoriOffset));

  EXPECT_GE(mtl_run.start(), 0);
  /* 1.0 s window with >= 25 frames after 3-frame warmup: <= 1/59 (1.7%) edge
   * quantization error at 59.94 fps vs +/-10% tolerance. */
  std::vector<tests_context*> rx_ctxs = {rx_ctx.get()};
  auto win = mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames,
                                           /*warmup_frames=*/3,
                                           /*min_delta_frames=*/25,
                                           /*min_window_ms=*/1000);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  EXPECT_GT(mtl_test::tx_frames(tx_ctx.get()), 0);
  EXPECT_GT(mtl_test::tx_done_frames(tx_ctx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&tx_ctx->incomplete_frame_cnt), 0);
  EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0);
  EXPECT_GT(mtl_test::sha_checked_frames(rx_ctx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx_ctx->rx_meta_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx_ctx->sha_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx_ctx->incomplete_frame_cnt), 0);
  EXPECT_NEAR(win.rate[0], expect_fps, expect_fps * 0.1);
}
