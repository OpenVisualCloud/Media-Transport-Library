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

struct St40pWorkerCfg {
  bool block_get = true;
  bool user_pacing = false;
  uint16_t udw_len = 0; /* 0 = minimal empty frame */
  bool fill_pattern = false;
  uint16_t did = 0x43;
  uint16_t sdid = 0x02;
  uint16_t line_number = 10;
  uint16_t hori_offset = 0;
  bool verify_meta_and_sha = false;
  uint32_t expect_rx_udw_buf_size = 0;
  std::atomic<bool>* rx_slot_addr_matched = nullptr;
};

int test_st40p_frame_available(void* priv) {
  auto* s = static_cast<tests_context*>(priv);
  std::unique_lock<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

int test_st40p_tx_frame_done(void* priv, struct st40_frame_info* fi) {
  auto* s = static_cast<tests_context*>(priv);
  if (!__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE)) return -EIO;
  if (!fi || fi->status != ST_FRAME_STATUS_COMPLETE) {
    __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);
  return 0;
}

struct st40p_tx_ops make_st40p_tx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool block_get = true,
                                      bool redundant = false,
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
  ops.flags = (block_get ? ST40P_TX_FLAG_BLOCK_GET : 0) | extra_flags;
  if (!block_get) ops.notify_frame_available = test_st40p_frame_available;
  return ops;
}

struct st40p_rx_ops make_st40p_rx_ops(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port, bool block_get = true,
                                      bool redundant = false,
                                      bool mcast_single_port = false,
                                      uint32_t max_udw_buff_size = 1024) {
  struct st40p_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = test_ctx;
  if (mcast_single_port) {
    ops.port.num_port = 1;
    memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    const enum mtl_port rx_port = (ctx->para.num_ports >= 2) ? MTL_PORT_R : MTL_PORT_P;
    snprintf(ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[rx_port]);
    ops.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
    ops.port.payload_type = ST40P_TEST_PAYLOAD_TYPE;
  } else {
    mtl_test::fill_rx_port(ops.port, ctx, udp_port, ST40P_TEST_PAYLOAD_TYPE, 0,
                           redundant);
  }
  ops.interlaced = false;
  ops.framebuff_cnt = test_ctx->fb_cnt;
  ops.max_udw_buff_size = max_udw_buff_size;
  ops.rtp_ring_size = 1024;
  ops.flags = block_get ? ST40P_RX_FLAG_BLOCK_GET : 0;
  if (!block_get) ops.notify_frame_available = test_st40p_frame_available;
  return ops;
}

void st40p_tx_ops_init(tests_context* st40, struct st40p_tx_ops* ops_tx) {
  *ops_tx = make_st40p_tx_ops(st40->ctx, st40, "st40p_test_tx",
                              ABORT_TEST_UDP_PORT + st40->idx, /*block_get=*/true,
                              /*redundant=*/false, /*mcast_single_port=*/true);
}

void st40p_rx_ops_init(tests_context* st40, struct st40p_rx_ops* ops_rx) {
  *ops_rx = make_st40p_rx_ops(st40->ctx, st40, "st40p_test_rx",
                              ABORT_TEST_UDP_PORT + st40->idx, /*block_get=*/true,
                              /*redundant=*/false, /*mcast_single_port=*/true);
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

void st40p_tx_worker(tests_context* s, St40pWorkerCfg cfg) {
  auto handle = (st40p_tx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  auto st = s->ctx->handle;
  while (!s->stop) {
    auto* fi = st40p_tx_get_frame(handle);
    if (!fi) {
      if (!cfg.block_get) {
        std::unique_lock<std::mutex> lck(s->mtx);
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
      }
      continue;
    }
    if (cfg.udw_len > 0 && fi->meta && fi->udw_buffer_size >= cfg.udw_len) {
      fi->meta_num = 1;
      memset(&fi->meta[0], 0, sizeof(fi->meta[0]));
      fi->meta[0].did = cfg.did;
      fi->meta[0].sdid = cfg.sdid;
      fi->meta[0].line_number = cfg.line_number;
      fi->meta[0].hori_offset = cfg.hori_offset;
      fi->meta[0].udw_offset = 0;
      fi->meta[0].udw_size = cfg.udw_len;
      if (cfg.fill_pattern && fi->udw_buff_addr) {
        for (uint16_t i = 0; i < cfg.udw_len; i++) {
          fi->udw_buff_addr[i] = static_cast<uint8_t>(0xA0 + i);
        }
      }
      fi->udw_buffer_fill = cfg.udw_len;
    } else {
      fi->meta_num = 0;
      fi->udw_buffer_fill = 0;
    }
    if (cfg.user_pacing) {
      fi->tfmt = ST10_TIMESTAMP_FMT_TAI;
      fi->timestamp = mtl_ptp_read_time(st) + 20 * 1000 * 1000; /* +20ms */
    }
    st40p_tx_put_frame(handle, fi);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
}

void st40p_rx_worker(tests_context* s, St40pWorkerCfg cfg) {
  auto handle = (st40p_rx_handle)__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
  if (!handle) return;
  while (!s->stop) {
    auto* fi = st40p_rx_get_frame(handle);
    if (!fi) {
      if (!cfg.block_get) {
        std::unique_lock<std::mutex> lck(s->mtx);
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
      }
      continue;
    }
    if (cfg.verify_meta_and_sha) {
      if (fi->status != ST_FRAME_STATUS_COMPLETE) {
        __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
      }
      bool meta_ok =
          fi->meta != nullptr && fi->meta_num == 1 &&
          fi->udw_buffer_fill == cfg.udw_len && fi->meta[0].did == cfg.did &&
          fi->meta[0].sdid == cfg.sdid && fi->meta[0].line_number == cfg.line_number &&
          fi->meta[0].hori_offset == cfg.hori_offset &&
          fi->meta[0].udw_size == cfg.udw_len && fi->udw_buff_addr != nullptr &&
          (!cfg.expect_rx_udw_buf_size ||
           fi->udw_buffer_size == cfg.expect_rx_udw_buf_size);
      if (!meta_ok) __atomic_fetch_add(&s->rx_meta_fail_cnt, 1, __ATOMIC_RELAXED);

      if (cfg.rx_slot_addr_matched && !cfg.rx_slot_addr_matched->load()) {
        for (int idx = 0; idx < s->fb_cnt; idx++) {
          if (st40p_rx_get_udw_buff_addr(handle, static_cast<uint16_t>(idx)) ==
              fi->udw_buff_addr) {
            cfg.rx_slot_addr_matched->store(true, std::memory_order_relaxed);
            break;
          }
        }
      }

      if (fi->udw_buff_addr && fi->udw_buffer_fill == cfg.udw_len) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(fi->udw_buff_addr, cfg.udw_len, digest);
        bool matched = false;
        for (int i = 0; i < TEST_MAX_SHA_HIST_NUM; i++) {
          if (memcmp(digest, s->shas[i], SHA256_DIGEST_LENGTH) == 0) {
            matched = true;
            break;
          }
        }
        if (!matched) __atomic_fetch_add(&s->sha_fail_cnt, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
      }
    }
    st40p_rx_put_frame(handle, fi);
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
  }
}

/* Compact RAII loopback pair; member declaration order guarantees strict reverse
 * teardown: workers -> mtl_stop -> handles -> contexts. */
struct St40pLoopback {
  mtl_test::ScopedContext tx_ctx;
  mtl_test::ScopedContext rx_ctx;
  ScopedSt40pTx tx;
  ScopedSt40pRx rx;
  mtl_test::ScopedMtlStart mtl;
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;
  struct st40p_tx_ops ops_tx = {};
  struct st40p_rx_ops ops_rx = {};

  St40pLoopback(struct st_tests_context* ctx, uint16_t fb_cnt = 3, bool check_sha = false)
      : tx_ctx(mtl_test::make_context(ctx, 0, fb_cnt, check_sha)),
        rx_ctx(mtl_test::make_context(ctx, 0, fb_cnt, check_sha)),
        mtl(ctx->handle) {
  }

  void create(struct st_tests_context* ctx, uint16_t udp_port, const char* tag,
              bool block_get = true, bool redundant = false, uint32_t tx_max_udw = 1024,
              uint32_t rx_max_udw = 1024, bool notify_tx_done = false) {
    ops_tx = make_st40p_tx_ops(ctx, tx_ctx.get(), tag, udp_port, block_get, redundant,
                               /*mcast_single_port=*/false, 0, tx_max_udw);
    if (notify_tx_done) ops_tx.notify_frame_done = test_st40p_tx_frame_done;
    tx.reset(st40p_tx_create(ctx->handle, &ops_tx));
    ASSERT_TRUE(tx);
    if (block_get) {
      EXPECT_EQ(st40p_tx_set_block_timeout(tx.get(), NS_PER_S), 0);
    }
    __atomic_store_n(&tx_ctx->handle, tx.get(), __ATOMIC_RELEASE);

    ops_rx = make_st40p_rx_ops(ctx, rx_ctx.get(), tag, udp_port, block_get, redundant,
                               /*mcast_single_port=*/false, rx_max_udw);
    rx.reset(st40p_rx_create(ctx->handle, &ops_rx));
    ASSERT_TRUE(rx);
    if (block_get) {
      EXPECT_EQ(st40p_rx_set_block_timeout(rx.get(), NS_PER_S), 0);
    }
    __atomic_store_n(&rx_ctx->handle, rx.get(), __ATOMIC_RELEASE);
  }

  void start_workers(const St40pWorkerCfg& cfg, bool with_rx = true) {
    std::function<void()> tx_wake =
        cfg.block_get
            ? std::function<void()>([h = tx.get()]() { st40p_tx_wake_block(h); })
            : nullptr;
    tx_worker = mtl_test::ScopedWorker(tx_ctx.get(),
                                       std::thread(st40p_tx_worker, tx_ctx.get(), cfg),
                                       std::move(tx_wake));
    if (with_rx) {
      std::function<void()> rx_wake =
          cfg.block_get
              ? std::function<void()>([h = rx.get()]() { st40p_rx_wake_block(h); })
              : nullptr;
      rx_worker = mtl_test::ScopedWorker(rx_ctx.get(),
                                         std::thread(st40p_rx_worker, rx_ctx.get(), cfg),
                                         std::move(rx_wake));
    }
  }

  void stop_all() {
    tx_worker.stop();
    rx_worker.stop();
    EXPECT_GE(mtl.stop(), 0);
  }
};

void st40p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 3);

  uint32_t extra_flags =
      user_pacing ? (ST40P_TX_FLAG_USER_PACING | ST40P_TX_FLAG_USER_TIMESTAMP) : 0;
  auto ops_tx =
      make_st40p_tx_ops(ctx, test_ctx.get(), "st40p_epoch_drop_test",
                        EPOCH_DROP_TEST_UDP_PORT, /*block_get=*/true,
                        /*redundant=*/false, /*mcast_single_port=*/true, extra_flags);

  ScopedSt40pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_run(st);
  EXPECT_GE(mtl_run.start(), 0);

  tx_handle.reset(st40p_tx_create(st, &ops_tx));
  ASSERT_TRUE(tx_handle);
  EXPECT_EQ(st40p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&test_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  St40pWorkerCfg cfg;
  cfg.user_pacing = user_pacing;
  mtl_test::ScopedWorker tx_worker(test_ctx.get(),
                                   std::thread(st40p_tx_worker, test_ctx.get(), cfg),
                                   [h = tx_handle.get()]() { st40p_tx_wake_block(h); });

  /* Wait for >= 25 paced frames (~0.42 s at 59.94 fps) to verify zero epoch drops. */
  std::vector<tests_context*> tx_ctxs = {test_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(tx_ctxs, mtl_test::tx_frames, 25, 6000));

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  struct st40_tx_user_stats stats;
  EXPECT_GE(st40p_tx_get_session_stats(tx_handle.get(), &stats), 0);
  EXPECT_GT(mtl_test::tx_frames(test_ctx.get()), 0);
  EXPECT_EQ(stats.common.stat_epoch_drop, 0u);
}

template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename InitFn>
void expect_st40p_create_fails(
    InitFn ops_init,
    std::initializer_list<std::pair<const char*, std::function<void(Ops&)>>> cases) {
  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  Ops base;
  ops_init(test_ctx.get(), &base);

  EXPECT_EQ(CreateFn(st, nullptr), nullptr);
  EXPECT_EQ(CreateFn(nullptr, &base), nullptr);
  for (const auto& [label, mutate] : cases) {
    Ops ops = base;
    mutate(ops);
    EXPECT_FALSE(
        static_cast<bool>(mtl_test::ScopedHandle<Handle, FreeFn>(CreateFn(st, &ops))))
        << label;
  }
}

} /* namespace */

TEST(St40p, put_frame_abort) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St40pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ABORT_TEST_UDP_PORT + 14, "st40p_abort"));

  /* TX put_frame_abort before starting worker. */
  struct st40_frame_info* tx_fi = st40p_tx_get_frame(lb.tx.get());
  ASSERT_NE(tx_fi, nullptr);
  EXPECT_GE(st40p_tx_put_frame_abort(lb.tx.get(), tx_fi), 0);

  /* RX put_frame_abort on live loopback stream. */
  St40pWorkerCfg cfg;
  lb.start_workers(cfg, /*with_rx=*/false);
  EXPECT_GE(lb.mtl.start(), 0);
  std::vector<tests_context*> tx_ctxs = {lb.tx_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(tx_ctxs, mtl_test::tx_frames, 15, 5000));

  struct st40_frame_info* rx_fi = st40p_rx_get_frame(lb.rx.get());
  ASSERT_NE(rx_fi, nullptr);
  EXPECT_GE(st40p_rx_put_frame_abort(lb.rx.get(), rx_fi), 0);

  lb.stop_all();
}

TEST(St40p, frame_is_late) {
  auto st = st_test_ctx()->handle;
  struct st40_frame_info frame_info;
  memset(&frame_info, 0, sizeof(frame_info));
  frame_info.tfmt = ST10_TIMESTAMP_FMT_TAI;
  const uint64_t now = mtl_ptp_read_time(st);
  frame_info.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st40_frame_is_late(st, &frame_info));

  frame_info.timestamp = now + (uint64_t)10 * NS_PER_S;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));

  frame_info.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame_info.timestamp = 0;
  EXPECT_FALSE(st40_frame_is_late(st, &frame_info));
}

/* Verify redundant TX and RX port stats for st40p ancillary sessions (requires 4 ports).
 */
TEST(St40p, redundant_stats) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  St40pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ABORT_TEST_UDP_PORT + 100, "st40p_redundant",
                                    /*block_get=*/true, /*redundant=*/true));
  lb.start_workers(St40pWorkerCfg{});

  EXPECT_GE(lb.mtl.start(), 0);
  /* Wait until both redundant RX legs carry traffic, then receive >= 50 frames
   * (~0.83 s at 59.94 fps) to stabilize port byte balance. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        struct st40_rx_user_stats st_rx;
        return st40p_rx_get_session_stats(lb.rx.get(), &st_rx) >= 0 &&
               st_rx.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               st_rx.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      6000));
  int rx_base = mtl_test::rx_frames(lb.rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(lb.rx_ctx.get()) - rx_base >= 50; }, 6000));
  lb.stop_all();

  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;
  EXPECT_GE(st40p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_GE(st40p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  mtl_test::expect_redundant_port_stats(tx_stats, rx_stats);
  EXPECT_GT(mtl_test::rx_frames(lb.rx_ctx.get()), 0);
}

TEST(St40p, tx_no_epoch_drop) {
  st40p_tx_epoch_drop_test(false);
}
TEST(St40p, tx_user_pacing_no_epoch_drop) {
  st40p_tx_epoch_drop_test(true);
}

TEST(St40p, session_stats_reset) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St40pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ABORT_TEST_UDP_PORT + 24, "st40p_stats"));
  St40pWorkerCfg cfg;
  cfg.udw_len = 16;
  cfg.fill_pattern = true;
  lb.start_workers(cfg);

  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(lb.rx_ctx.get()) >= 15; }, 6000));
  lb.stop_all();

  struct st40_tx_user_stats tx_stats;
  struct st40_rx_user_stats rx_stats;
  ASSERT_GE(st40p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  ASSERT_GE(st40p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(rx_stats.common.stat_frames_received, 0u);

  EXPECT_GE(st40p_tx_reset_session_stats(lb.tx.get()), 0);
  EXPECT_GE(st40p_rx_reset_session_stats(lb.rx.get()), 0);

  ASSERT_GE(st40p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  ASSERT_GE(st40p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_EQ(tx_stats.common.stat_frames_dropped, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.stat_frames_received, 0u);
  EXPECT_EQ(rx_stats.common.stat_frames_dropped, 0u);

  EXPECT_LT(st40p_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st40p_tx_get_session_stats(lb.tx.get(), nullptr), 0);
  EXPECT_LT(st40p_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st40p_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st40p_rx_get_session_stats(lb.rx.get(), nullptr), 0);
  EXPECT_LT(st40p_rx_reset_session_stats(nullptr), 0);
}

TEST(St40p, update_dest_and_source) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kBasePort = ABORT_TEST_UDP_PORT + 30;
  const double expect_fps = st_frame_rate(ST_FPS_P59_94);

  St40pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, kBasePort, "st40p_switch"));
  St40pWorkerCfg cfg;
  cfg.udw_len = 16;
  cfg.fill_pattern = true;
  lb.start_workers(cfg);

  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(lb.rx_ctx.get()) >= 15; }, 6000));

  auto verify_switch = [&](const uint8_t* tx_dip, const uint8_t* rx_ip, uint16_t udp_port,
                           const char* label) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_dip, MTL_IP_ADDR_LEN);
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
    EXPECT_GE(st40p_tx_update_destination(lb.tx.get(), &dst), 0) << label;

    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_ip, MTL_IP_ADDR_LEN);
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    EXPECT_GE(st40p_rx_update_source(lb.rx.get(), &src), 0) << label;

    const int baseline = mtl_test::rx_frames(lb.rx_ctx.get());
    std::vector<tests_context*> rx_ctxs = {lb.rx_ctx.get()};
    auto delta_rx = [&](tests_context* c) { return mtl_test::rx_frames(c) - baseline; };
    /* 1.0 s window, >= 25 frames after 3-frame warmup: <= 1.7% edge error vs +/-10%. */
    auto win = mtl_test::measure_rate_window(rx_ctxs, delta_rx, 3, 25, 1000);
    EXPECT_TRUE(win.warmed_up) << label;
    EXPECT_TRUE(win.complete) << label;
    EXPECT_GT(delta_rx(lb.rx_ctx.get()), 0) << label;
    EXPECT_NEAR(win.rate[0], expect_fps, expect_fps * 0.1) << label;
  };

  verify_switch(ctx->mcast_ip_addr[MTL_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
                kBasePort + 2, "mcast R");
  verify_switch(lb.ops_tx.port.dip_addr[MTL_SESSION_PORT_P],
                lb.ops_rx.port.ip_addr[MTL_SESSION_PORT_P], kBasePort, "initial");

  lb.stop_all();
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
  expect_st40p_create_fails<struct st40p_tx_ops, st40p_tx_handle, st40p_tx_create,
                            st40p_tx_free>(
      st40p_tx_ops_init,
      {
          {"invalid fps", [](auto& o) { o.fps = ST_FPS_MAX; }},
          {"zero max_udw_buff_size", [](auto& o) { o.max_udw_buff_size = 0; }},
          {"zero framebuff_cnt", [](auto& o) { o.framebuff_cnt = 0; }},
          {"force numa", [](auto& o) { o.flags |= ST40P_TX_FLAG_FORCE_NUMA; }},
          {"invalid primary port",
           [](auto& o) {
             snprintf(o.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "invalid_port");
           }},
      });
}

TEST(St40p, rx_create_expect_fail) {
  /* Do not call verify_invalid_ports here:
   * lib/src/st2110/pipeline/st40_pipeline_rx.c:159-174 clamps ops->port.num_port to
   * MTL_SESSION_PORT_MAX and calls mt_port_id(impl, MTL_PORT_MAX) when port[1] is empty.
   */
  expect_st40p_create_fails<struct st40p_rx_ops, st40p_rx_handle, st40p_rx_create,
                            st40p_rx_free>(
      st40p_rx_ops_init,
      {
          {"zero num_port", [](auto& o) { o.port.num_port = 0; }},
          {"zero max_udw_buff_size", [](auto& o) { o.max_udw_buff_size = 0; }},
          {"zero framebuff_cnt", [](auto& o) { o.framebuff_cnt = 0; }},
          {"force numa", [](auto& o) { o.flags |= ST40P_RX_FLAG_FORCE_NUMA; }},
          {"invalid primary port",
           [](auto& o) {
             snprintf(o.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "invalid_port");
           }},
      });
}

/* End-to-end non-blocking st40p TX/RX test with TX/RX introspection, queue meta,
 * notify_frame_available, notify_frame_done, ANC metadata, SHA256 digest, and FPS. */
TEST(St40p, digest_non_block) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kFbCnt = 3;
  constexpr uint32_t kTxMaxUdwSize = 2048;
  constexpr uint32_t kRxMaxUdwSize = 1536;
  constexpr uint16_t kUdwLen = 240;
  constexpr uint16_t kUdpPort = ABORT_TEST_UDP_PORT + 40;
  const double expect_fps = st_frame_rate(ST_FPS_P59_94);

  St40pLoopback lb(ctx, kFbCnt, /*check_sha=*/true);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, kUdpPort, "st40p_digest_nb",
                                    /*block_get=*/false, /*redundant=*/false,
                                    kTxMaxUdwSize, kRxMaxUdwSize,
                                    /*notify_tx_done=*/true));

  /* TX/RX introspection, queue meta, and OOB slot checks. */
  EXPECT_EQ(st40p_tx_max_udw_buff_size(lb.tx.get()), kTxMaxUdwSize);
  EXPECT_EQ(st40p_rx_max_udw_buff_size(lb.rx.get()), kRxMaxUdwSize);

  struct st_queue_meta qmeta;
  memset(&qmeta, 0, sizeof(qmeta));
  EXPECT_GE(st40p_rx_get_queue_meta(lb.rx.get(), &qmeta), 0);
  EXPECT_EQ(qmeta.num_port, 1);

  std::vector<void*> seen_udw, seen_fb;
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    auto* udw = static_cast<uint8_t*>(st40p_tx_get_udw_buff_addr(lb.tx.get(), idx));
    auto* fb = static_cast<struct st40_frame*>(st40p_tx_get_fb_addr(lb.tx.get(), idx));
    ASSERT_NE(udw, nullptr);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->data, udw);
    EXPECT_EQ(std::find(seen_udw.begin(), seen_udw.end(), udw), seen_udw.end());
    EXPECT_EQ(std::find(seen_fb.begin(), seen_fb.end(), fb), seen_fb.end());
    seen_udw.push_back(udw);
    seen_fb.push_back(fb);
    st_test_rand_data(udw, kUdwLen, idx);
    SHA256(udw, kUdwLen, lb.tx_ctx->shas[idx]);
  }
  memcpy(lb.rx_ctx->shas, lb.tx_ctx->shas, kFbCnt * SHA256_DIGEST_LENGTH);

  EXPECT_EQ(st40p_tx_get_udw_buff_addr(lb.tx.get(), kFbCnt), nullptr);
  EXPECT_EQ(st40p_tx_get_udw_buff_addr(lb.tx.get(), kFbCnt * 2), nullptr);
  EXPECT_EQ(st40p_tx_get_fb_addr(lb.tx.get(), kFbCnt), nullptr);
  EXPECT_EQ(st40p_tx_get_fb_addr(lb.tx.get(), kFbCnt * 2), nullptr);
  EXPECT_EQ(st40p_rx_get_udw_buff_addr(lb.rx.get(), kFbCnt), nullptr);
  EXPECT_EQ(st40p_rx_get_udw_buff_addr(lb.rx.get(), kFbCnt * 2), nullptr);

  std::atomic<bool> rx_slot_matched{false};
  St40pWorkerCfg cfg;
  cfg.block_get = false;
  cfg.udw_len = kUdwLen;
  cfg.hori_offset = 0x10;
  cfg.verify_meta_and_sha = true;
  cfg.expect_rx_udw_buf_size = kRxMaxUdwSize;
  cfg.rx_slot_addr_matched = &rx_slot_matched;
  lb.start_workers(cfg);

  EXPECT_GE(lb.mtl.start(), 0);
  /* 1.0 s window, >= 25 frames after 3-frame warmup: <= 1.7% edge error vs +/-10%. */
  std::vector<tests_context*> rx_ctxs = {lb.rx_ctx.get()};
  auto win = mtl_test::measure_rate_window(rx_ctxs, mtl_test::rx_frames, 3, 25, 1000);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);
  lb.stop_all();

  EXPECT_TRUE(rx_slot_matched.load());
  EXPECT_GT(mtl_test::tx_frames(lb.tx_ctx.get()), 0);
  EXPECT_GT(mtl_test::tx_done_frames(lb.tx_ctx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&lb.tx_ctx->incomplete_frame_cnt), 0);
  EXPECT_GT(mtl_test::rx_frames(lb.rx_ctx.get()), 0);
  EXPECT_GT(mtl_test::sha_checked_frames(lb.rx_ctx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&lb.rx_ctx->rx_meta_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&lb.rx_ctx->sha_fail_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&lb.rx_ctx->incomplete_frame_cnt), 0);
  EXPECT_NEAR(win.rate[0], expect_fps, expect_fps * 0.1);
}
