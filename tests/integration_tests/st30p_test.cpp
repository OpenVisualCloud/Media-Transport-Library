/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

#define ST30P_TEST_PAYLOAD_TYPE (111)
#define ST30P_TEST_UDP_PORT (50000)
#define EPOCH_DROP_TEST_UDP_PORT_ST30P (50100)

/*
 * st30p frames carry 10ms of audio (~100 fps).
 * A 600ms window collects ~60 frames (<= 1.7% edge quantization error),
 * well inside the +/-10% FPS tolerance.
 */
static constexpr int kSt30pRateWarmupFrames = 5;
static constexpr int kSt30pRateMinDeltaFrames = 40;
static constexpr int kSt30pRateMinWindowMs = 600;
static constexpr int kSt30pPhaseTargetFrames = 30;
static constexpr int kSt30pPhaseTimeoutMs = 5000;

static int test_st30p_frame_available(void* priv) {
  auto s = static_cast<tests_context*>(priv);
  std::lock_guard<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

static int test_st30p_tx_frame_done(void* priv, struct st30_frame* /*frame*/) {
  auto s = static_cast<tests_context*>(priv);
  if (!__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE)) return -EIO;
  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);
  return 0;
}

static bool st30p_frame_meta_matches(const struct st30_frame* f, const tests_context* s) {
  return f->data_size == s->frame_size && f->buffer_size == s->frame_size &&
         f->fmt == s->audio_fmt && f->channel == s->audio_channel &&
         f->ptime == s->audio_ptime && f->sampling == s->audio_sampling;
}

static void test_st30p_tx_frame_thread(void* args) {
  auto s = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30p_tx_handle>(__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  while (!s->stop) {
    struct st30_frame* frame = st30p_tx_get_frame(handle);
    if (!frame) {
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
        lck.unlock();
      }
      continue;
    }
    if (!st30p_frame_meta_matches(frame, s)) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    st30p_tx_put_frame(handle, frame);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
}

static void test_st30p_rx_frame_thread(void* args) {
  auto s = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30p_rx_handle>(__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  bool has_ts = false;
  uint64_t timestamp = 0;

  while (!s->stop) {
    struct st30_frame* frame = st30p_rx_get_frame(handle);
    if (!frame) {
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
        lck.unlock();
      }
      continue;
    }

    if (!st30p_frame_meta_matches(frame, s) || !st_is_frame_complete(frame->status) ||
        (has_ts && frame->timestamp == timestamp)) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    has_ts = true;
    timestamp = frame->timestamp;

    if (s->check_sha) {
      /* ST 2110-30 RTP packets carry no frame-boundary marker bit, so each packet slice
       * starts with a uint16_t slice index (0..pkts_per_frame-1) followed by the hashed
       * body. Verify both per-packet integrity and monotonic slice ordering within the
       * 10ms frame regardless of packet-phase start offset. */
      const size_t pkt_len = static_cast<size_t>(s->pkt_data_len);
      bool frame_ok = (pkt_len > sizeof(uint16_t)) && (s->frame_size % pkt_len == 0);
      if (frame_ok) {
        const auto* base = static_cast<const unsigned char*>(frame->addr);
        const int hist_cnt = std::min(s->fb_cnt > 0 ? s->fb_cnt : TEST_MAX_SHA_HIST_NUM,
                                      TEST_MAX_SHA_HIST_NUM);
        const uint16_t pkts_per_frame = static_cast<uint16_t>(s->frame_size / pkt_len);
        bool has_prev_slice = false;
        uint16_t prev_slice = 0;
        for (size_t off = 0; off < s->frame_size; off += pkt_len) {
          uint16_t slice_idx = 0;
          memcpy(&slice_idx, base + off, sizeof(slice_idx));
          if (slice_idx >= pkts_per_frame ||
              (has_prev_slice && slice_idx != (prev_slice + 1) % pkts_per_frame)) {
            frame_ok = false;
            break;
          }
          prev_slice = slice_idx;
          has_prev_slice = true;
          unsigned char result[SHA256_DIGEST_LENGTH];
          SHA256(base + off + sizeof(uint16_t), pkt_len - sizeof(uint16_t), result);
          int i = 0;
          for (i = 0; i < hist_cnt; i++) {
            if (!memcmp(result, s->shas[i], SHA256_DIGEST_LENGTH)) break;
          }
          if (i >= hist_cnt) {
            test_sha_dump("st30p_rx_error_sha", result);
            frame_ok = false;
            break;
          }
        }
      }
      if (!frame_ok) __atomic_fetch_add(&s->sha_fail_cnt, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&s->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
    }

    st30p_rx_put_frame(handle, frame);
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
  }
}

namespace {

struct St30pStreamSpec {
  enum st30_fmt fmt = ST30_FMT_PCM24;
  uint16_t channel = 2;
  enum st30_sampling sampling = ST30_SAMPLING_48K;
  enum st30_ptime ptime = ST30_PTIME_1MS;
};

struct St30pDigestOptions {
  bool check_fps = true;
  int fb_cnt = TEST_MAX_SHA_HIST_NUM;
  uint32_t ssrc = 0;
  bool block_get = false;
  bool dedicated_tx_queue = false;
  bool zero_payload_type = false;
};

using ScopedSt30pTx = mtl_test::ScopedHandle<st30p_tx_handle, st30p_tx_free>;
using ScopedSt30pRx = mtl_test::ScopedHandle<st30p_rx_handle, st30p_rx_free>;

void apply_stream_spec(tests_context* c, const St30pStreamSpec& spec, bool block_get) {
  c->audio_fmt = spec.fmt;
  c->audio_channel = spec.channel;
  c->audio_sampling = spec.sampling;
  c->audio_ptime = spec.ptime;
  c->block_get = block_get;
}

void init_st30p_tx_ops(struct st30p_tx_ops* ops, tests_context* st30,
                       const St30pStreamSpec& spec = {}, const char* name = "st30p_test",
                       uint16_t udp_port = 0, uint8_t pt = ST30P_TEST_PAYLOAD_TYPE,
                       uint32_t ssrc = 0, bool block_get = false, bool redundant = false,
                       bool tx_only_mcast = false, double* out_fps = nullptr) {
  auto ctx = st30->ctx;
  const uint16_t port =
      udp_port ? udp_port : static_cast<uint16_t>(ST30P_TEST_UDP_PORT + st30->idx);
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  if (tx_only_mcast) {
    ops->port.num_port = 1;
    memcpy(ops->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops->port.udp_port[MTL_SESSION_PORT_P] = port;
    ops->port.payload_type = pt;
    ops->port.ssrc = ssrc;
  } else {
    mtl_test::fill_tx_port(ops->port, ctx, port, pt, ssrc, redundant);
  }
  ops->fmt = spec.fmt;
  ops->channel = spec.channel;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_size = st30_calculate_framebuff_size(
      ops->fmt, ops->ptime, ops->sampling, ops->channel, 10 * NS_PER_MS, out_fps);
  ops->framebuff_cnt = st30->fb_cnt;
  if (block_get)
    ops->flags |= ST30P_TX_FLAG_BLOCK_GET;
  else
    ops->notify_frame_available = test_st30p_frame_available;
  st30->frame_size = ops->framebuff_size;
  apply_stream_spec(st30, spec, block_get);
}

void init_st30p_rx_ops(struct st30p_rx_ops* ops, tests_context* st30,
                       const St30pStreamSpec& spec = {}, const char* name = "st30p_test",
                       uint16_t udp_port = 0, uint8_t pt = ST30P_TEST_PAYLOAD_TYPE,
                       uint32_t ssrc = 0, bool block_get = false, bool redundant = false,
                       bool rx_only_mcast = false) {
  auto ctx = st30->ctx;
  const uint16_t port =
      udp_port ? udp_port : static_cast<uint16_t>(ST30P_TEST_UDP_PORT + st30->idx);
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  if (rx_only_mcast) {
    ops->port.num_port = 1;
    memcpy(ops->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    const enum mtl_port rx_port = ctx->para.num_ports >= 2 ? MTL_PORT_R : MTL_PORT_P;
    snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[rx_port]);
    ops->port.udp_port[MTL_SESSION_PORT_P] = port;
    ops->port.payload_type = pt;
    ops->port.ssrc = ssrc;
  } else {
    mtl_test::fill_rx_port(ops->port, ctx, port, pt, ssrc, redundant);
  }
  ops->fmt = spec.fmt;
  ops->channel = spec.channel;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_size = st30_calculate_framebuff_size(ops->fmt, ops->ptime, ops->sampling,
                                                      ops->channel, 10 * NS_PER_MS, NULL);
  ops->framebuff_cnt = st30->fb_cnt;
  if (block_get)
    ops->flags |= ST30P_RX_FLAG_BLOCK_GET;
  else
    ops->notify_frame_available = test_st30p_frame_available;
  st30->frame_size = ops->framebuff_size;
  apply_stream_spec(st30, spec, block_get);
}

/* Compact RAII loopback pair for blocking st30p tests; declaration order guarantees
 * workers stop -> mtl_stop -> handles free -> contexts free even on ASSERT failure. */
struct St30pLoopback {
  mtl_test::ScopedContext tx_ctx;
  mtl_test::ScopedContext rx_ctx;
  ScopedSt30pTx tx;
  ScopedSt30pRx rx;
  mtl_test::ScopedMtlStart mtl;
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;

  explicit St30pLoopback(struct st_tests_context* ctx)
      : tx_ctx(mtl_test::make_context(ctx, 0, 3)),
        rx_ctx(mtl_test::make_context(ctx, 0, 3)),
        mtl(ctx->handle) {
  }

  void create(struct st_tests_context* ctx, uint16_t udp_port, const char* tag,
              bool redundant = false, uint32_t rx_extra_flags = 0,
              bool start_tx_worker = true, bool start_rx_worker = true) {
    const St30pStreamSpec spec;
    struct st30p_tx_ops ops_tx;
    init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, tag, udp_port, ST30P_TEST_PAYLOAD_TYPE,
                      0, /*block_get=*/true, redundant);
    tx.reset(st30p_tx_create(ctx->handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx));
    EXPECT_EQ(st30p_tx_set_block_timeout(tx.get(), NS_PER_S), 0);
    __atomic_store_n(&tx_ctx->handle, tx.get(), __ATOMIC_RELEASE);

    struct st30p_rx_ops ops_rx;
    init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, tag, udp_port, ST30P_TEST_PAYLOAD_TYPE,
                      0, /*block_get=*/true, redundant);
    ops_rx.flags |= rx_extra_flags;
    rx.reset(st30p_rx_create(ctx->handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(rx));
    EXPECT_EQ(st30p_rx_set_block_timeout(rx.get(), NS_PER_S), 0);
    __atomic_store_n(&rx_ctx->handle, rx.get(), __ATOMIC_RELEASE);

    if (start_tx_worker) start_tx();
    if (start_rx_worker) start_rx();
  }

  void start_tx() {
    tx_worker = mtl_test::ScopedWorker(
        tx_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx_ctx.get()),
        [h = tx.get()]() { st30p_tx_wake_block(h); });
  }

  void start_rx() {
    rx_worker = mtl_test::ScopedWorker(
        rx_ctx.get(), std::thread(test_st30p_rx_frame_thread, rx_ctx.get()),
        [h = rx.get()]() { st30p_rx_wake_block(h); });
  }

  void stop_all() {
    tx_worker.stop();
    rx_worker.stop();
    EXPECT_GE(mtl.stop(), 0);
  }
};

void st30p_tx_assert_cnt(int expect_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_cnt);
}

void st30p_rx_assert_cnt(int expect_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_cnt);
}

const auto kTxOpsInit = [](tests_context* c, struct st30p_tx_ops* o) {
  init_st30p_tx_ops(o, c, {}, "st30p_test", 0, ST30P_TEST_PAYLOAD_TYPE, 0, false, false,
                    /*tx_only_mcast=*/true);
};
const auto kRxOpsInit = [](tests_context* c, struct st30p_rx_ops* o) {
  init_st30p_rx_ops(o, c, {}, "st30p_test", 0, ST30P_TEST_PAYLOAD_TYPE, 0, false, false,
                    /*rx_only_mcast=*/true);
};

}  // namespace

TEST(St30p, tx_create_free_mix) {
  mtl_test::verify_create_free<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                               st30p_tx_free>(kTxOpsInit, st30p_tx_assert_cnt, 2, 3, 4);
}
TEST(St30p, rx_create_free_mix) {
  mtl_test::verify_create_free<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                               st30p_rx_free>(kRxOpsInit, st30p_rx_assert_cnt, 2, 3, 4);
}
TEST(St30p, tx_create_free_max) {
  mtl_test::verify_create_free_max<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                                   st30p_tx_free>(kTxOpsInit, st30p_tx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St30p, rx_create_free_max) {
  mtl_test::verify_create_free_max<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                                   st30p_rx_free>(kRxOpsInit, st30p_rx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St30p, tx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                                 st30p_tx_free>(kTxOpsInit);
}
TEST(St30p, rx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                                 st30p_rx_free>(kRxOpsInit);
}

static void st30p_rx_digest_test(const std::vector<St30pStreamSpec>& specs,
                                 const St30pDigestOptions& opts, st_tests_context* ctx) {
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const size_t sessions = specs.size();
  const uint8_t pt = opts.zero_payload_type ? 0 : ST30P_TEST_PAYLOAD_TYPE;

  std::vector<mtl_test::ScopedContext> tx_ctxs, rx_ctxs;
  std::vector<tests_context*> raw_tx, raw_rx;
  std::vector<ScopedSt30pTx> tx_handles;
  std::vector<ScopedSt30pRx> rx_handles;
  mtl_test::ScopedMtlStart mtl_guard(st);
  std::vector<mtl_test::ScopedWorker> tx_workers, rx_workers;
  std::vector<double> expect_fps(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const uint16_t udp_port = ST30P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    tx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), opts.fb_cnt));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30p_tx_ops ops_tx;
    init_st30p_tx_ops(&ops_tx, raw_tx[i], specs[i], "st30p_test", udp_port, pt, opts.ssrc,
                      opts.block_get, /*redundant=*/false, /*tx_only_mcast=*/false,
                      &expect_fps[i]);
    if (opts.dedicated_tx_queue) ops_tx.flags |= ST30P_TX_FLAG_DEDICATE_QUEUE;
    ops_tx.notify_frame_done = test_st30p_tx_frame_done;

    ScopedSt30pTx handle(st30p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));
    if (opts.block_get) {
      EXPECT_EQ(st30p_tx_set_block_timeout(handle.get(), NS_PER_S), 0);
    }

    const size_t frame_size = raw_tx[i]->frame_size;
    EXPECT_EQ(st30p_tx_frame_size(handle.get()), frame_size);
    const int pkt_len_i =
        st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
    ASSERT_GT(pkt_len_i, 0);
    const size_t pkt_len = static_cast<size_t>(pkt_len_i);
    ASSERT_EQ(frame_size % pkt_len, 0u);
    raw_tx[i]->pkt_data_len = pkt_len_i;

    std::vector<void*> seen_fb;
    for (int frame = 0; frame < raw_tx[i]->fb_cnt; frame++) {
      auto fb = static_cast<uint8_t*>(st30p_tx_get_fb_addr(handle.get(), frame));
      ASSERT_NE(fb, nullptr);
      EXPECT_EQ(std::find(seen_fb.begin(), seen_fb.end(), fb), seen_fb.end());
      seen_fb.push_back(fb);
      ASSERT_GT(pkt_len, sizeof(uint16_t));
      const size_t body_len = pkt_len - sizeof(uint16_t);
      st_test_rand_data(fb + sizeof(uint16_t), body_len, frame);
      SHA256(fb + sizeof(uint16_t), body_len, raw_tx[i]->shas[frame]);
      const uint16_t pkts_per_frame = static_cast<uint16_t>(frame_size / pkt_len);
      for (uint16_t j = 0; j < pkts_per_frame; j++) {
        uint8_t* slice = fb + static_cast<size_t>(j) * pkt_len;
        if (j > 0) memcpy(slice + sizeof(uint16_t), fb + sizeof(uint16_t), body_len);
        memcpy(slice, &j, sizeof(j));
      }
      test_sha_dump("st30p_tx", raw_tx[i]->shas[frame]);
    }
    EXPECT_EQ(st30p_tx_get_fb_addr(handle.get(), raw_tx[i]->fb_cnt), nullptr);
    EXPECT_EQ(
        st30p_tx_get_fb_addr(handle.get(), static_cast<uint16_t>(raw_tx[i]->fb_cnt * 2)),
        nullptr);

    st30p_tx_handle raw_h = handle.get();
    __atomic_store_n(&raw_tx[i]->handle, raw_h, __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    std::function<void()> wake =
        opts.block_get ? std::function<void()>([raw_h]() { st30p_tx_wake_block(raw_h); })
                       : nullptr;
    tx_workers.emplace_back(raw_tx[i], std::thread(test_st30p_tx_frame_thread, raw_tx[i]),
                            std::move(wake));
  }

  for (size_t i = 0; i < sessions; i++) {
    const uint16_t udp_port = ST30P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    rx_ctxs.push_back(mtl_test::make_context(ctx, static_cast<int>(i), opts.fb_cnt,
                                             /*check_sha=*/true));
    raw_rx.push_back(rx_ctxs.back().get());

    struct st30p_rx_ops ops_rx;
    init_st30p_rx_ops(&ops_rx, raw_rx[i], specs[i], "st30p_test", udp_port, pt, opts.ssrc,
                      opts.block_get);
    ops_rx.framebuff_size = static_cast<uint32_t>(raw_tx[i]->frame_size);
    raw_rx[i]->frame_size = raw_tx[i]->frame_size;
    raw_rx[i]->pkt_data_len = raw_tx[i]->pkt_data_len;
    memcpy(raw_rx[i]->shas, raw_tx[i]->shas,
           TEST_MAX_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    ScopedSt30pRx handle(st30p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(handle));
    EXPECT_EQ(st30p_rx_frame_size(handle.get()), raw_rx[i]->frame_size);
    if (opts.block_get) {
      EXPECT_EQ(st30p_rx_set_block_timeout(handle.get(), NS_PER_S), 0);
    }

    st30p_rx_handle raw_h = handle.get();
    __atomic_store_n(&raw_rx[i]->handle, raw_h, __ATOMIC_RELEASE);
    rx_handles.push_back(std::move(handle));
    std::function<void()> wake =
        opts.block_get ? std::function<void()>([raw_h]() { st30p_rx_wake_block(raw_h); })
                       : nullptr;
    rx_workers.emplace_back(raw_rx[i], std::thread(test_st30p_rx_frame_thread, raw_rx[i]),
                            std::move(wake));
  }

  EXPECT_GE(mtl_guard.start(), 0);
  auto win =
      mtl_test::measure_pipeline_window(raw_tx, raw_rx, kSt30pRateWarmupFrames,
                                        kSt30pRateMinDeltaFrames, kSt30pRateMinWindowMs);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  for (auto& w : tx_workers) w.stop();
  for (auto& w : rx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    EXPECT_GT(mtl_test::tx_frames(raw_tx[i]), 0);
    EXPECT_GT(mtl_test::tx_done_frames(raw_tx[i]), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&raw_tx[i]->incomplete_frame_cnt), 0);
    EXPECT_GT(mtl_test::rx_frames(raw_rx[i]), 0);
    EXPECT_LE(mtl_test::relaxed_load(&raw_rx[i]->incomplete_frame_cnt), 4);
    EXPECT_GT(mtl_test::sha_checked_frames(raw_rx[i]), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&raw_rx[i]->sha_fail_cnt), 0);
    if (opts.check_fps) {
      EXPECT_NEAR(win.rx_fps[i], expect_fps[i], expect_fps[i] * 0.1);
    }
  }
}

TEST(St30p, digest_s3) {
  St30pDigestOptions opts;
  opts.dedicated_tx_queue = true;
  opts.zero_payload_type = true;
  st30p_rx_digest_test(
      {
          {ST30_FMT_PCM8, 8, ST30_SAMPLING_96K, ST30_PTIME_1MS},
          {ST30_FMT_PCM16, 2, ST30_SAMPLING_48K, ST30_PTIME_1MS},
          {ST30_FMT_PCM24, 4, ST30_SAMPLING_48K, ST30_PTIME_125US},
      },
      opts, st_test_ctx());
}

TEST(St30p, digest_s3_block) {
  St30pDigestOptions opts;
  opts.block_get = true;
  st30p_rx_digest_test(
      {
          {ST31_FMT_AM824, 3, ST31_SAMPLING_44K, ST31_PTIME_1_09MS},
          {ST30_FMT_PCM16, 5, ST30_SAMPLING_96K, ST30_PTIME_1MS},
          {ST30_FMT_PCM24, 7, ST30_SAMPLING_48K, ST30_PTIME_125US},
      },
      opts, st_test_ctx());
}

TEST(St30p, put_frame_abort) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint64_t kFbCnt = 3;
  St30pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ST30P_TEST_UDP_PORT, "st30p_abort",
                                    /*redundant=*/false, /*rx_extra_flags=*/0,
                                    /*start_tx_worker=*/false,
                                    /*start_rx_worker=*/false));

  /* Acquire and abort kFbCnt TX frames before starting MTL; double-abort must fail. */
  for (uint64_t i = 0; i < kFbCnt; i++) {
    struct st30_frame* tx_frame = st30p_tx_get_frame(lb.tx.get());
    ASSERT_NE(tx_frame, nullptr) << "TX get_frame failed at iteration " << i;
    EXPECT_EQ(st30p_tx_put_frame_abort(lb.tx.get(), tx_frame), 0);
    EXPECT_LT(st30p_tx_put_frame_abort(lb.tx.get(), tx_frame), 0);
  }

  struct st30_tx_user_stats tx_stats;
  struct st30_rx_user_stats rx_stats;
  memset(&tx_stats, 0, sizeof(tx_stats));
  memset(&rx_stats, 0, sizeof(rx_stats));
  EXPECT_EQ(st30p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.stat_frames_dropped, kFbCnt);

  /* Start MTL briefly without calling st30p_tx_put_frame; aborted frames must not send.
   */
  EXPECT_GE(lb.mtl.start(), 0);
  st_usleep(150 * 1000);
  EXPECT_GE(lb.mtl.stop(), 0);

  EXPECT_EQ(st30p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_EQ(st30p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_EQ(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_EQ(rx_stats.common.stat_frames_received, 0u);

  /* Stream normal frames from TX to RX, then acquire and abort kFbCnt * 2 RX frames. */
  lb.start_tx();
  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(lb.tx_ctx.get()) >= 5; }, 2000));

  for (uint64_t i = 0; i < kFbCnt * 2; i++) {
    struct st30_frame* rx_frame = st30p_rx_get_frame(lb.rx.get());
    ASSERT_NE(rx_frame, nullptr) << "RX get_frame failed at iteration " << i;
    EXPECT_EQ(st30p_rx_put_frame_abort(lb.rx.get(), rx_frame), 0);
    EXPECT_LT(st30p_rx_put_frame_abort(lb.rx.get(), rx_frame), 0);
  }

  lb.stop_all();
}

TEST(St30p, frame_is_late) {
  auto st = st_test_ctx()->handle;
  struct st30_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.tfmt = ST10_TIMESTAMP_FMT_TAI;
  const uint64_t now = mtl_ptp_read_time(st);
  frame.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st30_frame_is_late(st, &frame));

  frame.timestamp = now + static_cast<uint64_t>(10) * NS_PER_S;
  EXPECT_FALSE(st30_frame_is_late(st, &frame));

  frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame.timestamp = 0;
  EXPECT_FALSE(st30_frame_is_late(st, &frame));
}

/* Regression test for issue #1305: redundant TX/RX port stats (requires 4 ports). */
TEST(St30p, redundant_stats) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  St30pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ST30P_TEST_UDP_PORT + 100, "st30p_redundant",
                                    /*redundant=*/true));

  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        struct st30_rx_user_stats stats;
        return st30p_rx_get_session_stats(lb.rx.get(), &stats) >= 0 &&
               stats.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               stats.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      kSt30pPhaseTimeoutMs))
      << "Both redundant RX legs must receive traffic";

  const int base_rx = mtl_test::rx_frames(lb.rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return mtl_test::rx_frames(lb.rx_ctx.get()) - base_rx >= kSt30pPhaseTargetFrames;
      },
      kSt30pPhaseTimeoutMs));
  lb.stop_all();

  struct st30_tx_user_stats tx_stats;
  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_GE(st30p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);

  mtl_test::expect_redundant_port_stats(tx_stats, rx_stats, /*check_tx_packets=*/true);
  EXPECT_GT(mtl_test::rx_frames(lb.rx_ctx.get()), 0);
  EXPECT_EQ(rx_stats.common.stat_frames_corrupted, 0u);
}

TEST(St30p, rx_simulate_pkt_loss) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St30pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(
      lb.create(ctx, ST30P_TEST_UDP_PORT + 200, "st30p_sim_loss", /*redundant=*/false,
                ST30P_RX_FLAG_SIMULATE_PKT_LOSS | ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME,
                /*start_tx_worker=*/false, /*start_rx_worker=*/false));

  /* Fill all TX framebuffers with non-zero byte 0x5A so zeroed missing-packet gaps stand
   * out cleanly on corrupted RX frames. */
  const size_t frame_size = st30p_tx_frame_size(lb.tx.get());
  for (uint16_t i = 0; i < static_cast<uint16_t>(lb.tx_ctx->fb_cnt); i++) {
    void* fb = st30p_tx_get_fb_addr(lb.tx.get(), i);
    ASSERT_NE(fb, nullptr);
    memset(fb, 0x5A, frame_size);
  }
  const int pkt_len_i =
      st30_get_packet_size(lb.rx_ctx->audio_fmt, lb.rx_ctx->audio_ptime,
                           lb.rx_ctx->audio_sampling, lb.rx_ctx->audio_channel);
  ASSERT_GT(pkt_len_i, 0);
  const size_t pkt_len = static_cast<size_t>(pkt_len_i);

  std::atomic<int> zero_gap_frames{0};
  lb.start_tx();
  lb.rx_worker = mtl_test::ScopedWorker(
      lb.rx_ctx.get(),
      std::thread(
          [pkt_len, &zero_gap_frames](tests_context* s) {
            auto handle = static_cast<st30p_rx_handle>(
                __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
            while (!s->stop) {
              struct st30_frame* frame = st30p_rx_get_frame(handle);
              if (!frame) continue;
              if (frame->status == ST_FRAME_STATUS_CORRUPTED && frame->addr) {
                const auto* bytes = static_cast<const uint8_t*>(frame->addr);
                for (size_t off = 0; off + pkt_len <= frame->data_size; off += pkt_len) {
                  bool all_zero = true;
                  for (size_t b = 0; b < pkt_len; b++) {
                    if (bytes[off + b] != 0) {
                      all_zero = false;
                      break;
                    }
                  }
                  if (all_zero) {
                    zero_gap_frames.fetch_add(1, std::memory_order_relaxed);
                    break;
                  }
                }
              }
              st30p_rx_put_frame(handle, frame);
              __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
            }
          },
          lb.rx_ctx.get()),
      [h = lb.rx.get()]() { st30p_rx_wake_block(h); });

  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        if (mtl_test::rx_frames(lb.rx_ctx.get()) < 20) return false;
        if (zero_gap_frames.load(std::memory_order_relaxed) == 0) return false;
        struct st30_rx_user_stats stats;
        return st30p_rx_get_session_stats(lb.rx.get(), &stats) >= 0 &&
               stats.common.stat_frames_corrupted > 0 &&
               stats.stat_frames_incomplete > 0 && stats.common.stat_lost_packets > 0;
      },
      kSt30pPhaseTimeoutMs));
  lb.stop_all();

  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_GT(mtl_test::rx_frames(lb.rx_ctx.get()), 0);
  EXPECT_GT(rx_stats.common.stat_frames_corrupted, 0u);
  EXPECT_GT(rx_stats.stat_frames_incomplete, 0u);
  EXPECT_GT(rx_stats.common.stat_lost_packets, 0u);
  EXPECT_GT(zero_gap_frames.load(std::memory_order_relaxed), 0);
}

static void st30p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), {}, "st30p_epoch_drop_test",
                    EPOCH_DROP_TEST_UDP_PORT_ST30P, ST30P_TEST_PAYLOAD_TYPE, 0,
                    /*block_get=*/true, /*redundant=*/false, /*tx_only_mcast=*/true);
  if (user_pacing) ops_tx.flags |= ST30P_TX_FLAG_USER_PACING;

  /* Create after start, the path that reported bogus epoch drops. */
  EXPECT_GE(mtl_guard.start(), 0);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  const uint64_t base_tai = mtl_ptp_read_time(st) + 50ULL * NS_PER_MS;
  const uint64_t t_start = st_test_get_monotonic_time();

  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(),
      std::thread(
          [user_pacing, base_tai](tests_context* s) {
            auto handle = static_cast<st30p_tx_handle>(
                __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
            uint64_t seq = 0;
            while (!s->stop) {
              auto frame = st30p_tx_get_frame(handle);
              if (!frame) continue;
              if (user_pacing) {
                frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
                frame->timestamp = base_tai + seq * 25ULL * NS_PER_MS;
                seq++;
              }
              st30p_tx_put_frame(handle, frame);
              __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
            }
          },
          tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  if (user_pacing) {
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          struct st30_tx_user_stats st_now;
          return st30p_tx_get_session_stats(tx_handle.get(), &st_now) >= 0 &&
                 st_now.common.stat_frames_sent >= 15u;
        },
        kSt30pPhaseTimeoutMs));
  } else {
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() { return mtl_test::tx_frames(tx_ctx.get()) >= kSt30pPhaseTargetFrames; },
        kSt30pPhaseTimeoutMs));
  }
  const uint64_t elapsed_ms = (st_test_get_monotonic_time() - t_start) / NS_PER_MS;
  tx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  struct st30_tx_user_stats stats;
  EXPECT_GE(st30p_tx_get_session_stats(tx_handle.get(), &stats), 0);
  EXPECT_GT(mtl_test::tx_frames(tx_ctx.get()), 0);
  EXPECT_EQ(stats.common.stat_epoch_drop, 0u);
  if (user_pacing) {
    EXPECT_GE(stats.common.stat_frames_sent, 15u);
    EXPECT_EQ(stats.common.stat_error_user_timestamp, 0u);
    EXPECT_GE(elapsed_ms, 300u)
        << "15 user-paced frames at 25ms spacing should take >= 300ms";
  }
}

TEST(St30p, tx_no_epoch_drop) {
  st30p_tx_epoch_drop_test(false);
}
TEST(St30p, tx_user_pacing_no_epoch_drop) {
  st30p_tx_epoch_drop_test(true);
}

TEST(St30p, session_stats_reset) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St30pLoopback lb(ctx);
  ASSERT_NO_FATAL_FAILURE(lb.create(ctx, ST30P_TEST_UDP_PORT + 280, "st30p_stats"));

  struct st30_tx_user_stats tx_stats;
  struct st30_rx_user_stats rx_stats;
  EXPECT_LT(st30p_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st30p_tx_get_session_stats(lb.tx.get(), nullptr), 0);
  EXPECT_LT(st30p_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st30p_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st30p_rx_get_session_stats(lb.rx.get(), nullptr), 0);
  EXPECT_LT(st30p_rx_reset_session_stats(nullptr), 0);

  EXPECT_GE(lb.mtl.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(lb.rx_ctx.get()) >= kSt30pPhaseTargetFrames; },
      kSt30pPhaseTimeoutMs));
  lb.stop_all();
  /* Wait briefly for queued TX audio frames to drain under DEV_AUTO_START_STOP. */
  st_usleep(50 * 1000);

  EXPECT_EQ(st30p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(st30p_tx_reset_session_stats(lb.tx.get()), 0);
  EXPECT_EQ(st30p_tx_get_session_stats(lb.tx.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.stat_frames_sent, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);

  /* Free TX before resetting RX stats so no in-flight packet arrives after reset. */
  lb.tx.reset();
  st_usleep(20 * 1000);

  EXPECT_EQ(st30p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.stat_frames_received, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(st30p_rx_reset_session_stats(lb.rx.get()), 0);
  EXPECT_EQ(st30p_rx_get_session_stats(lb.rx.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.stat_frames_received, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
}

TEST(St30p, update_source_and_destination) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  const uint16_t base_port = ST30P_TEST_UDP_PORT + 300;
  double expect_fps = 100.0;

  /* TX0 + RX share St30pLoopback; TX1 provides a second multicast source. */
  St30pLoopback lb(ctx);
  auto tx1_ctx = mtl_test::make_context(ctx, 1, 3);
  ScopedSt30pTx tx1;
  mtl_test::ScopedWorker tx1_worker;

  struct st30p_tx_ops ops_tx0;
  init_st30p_tx_ops(&ops_tx0, lb.tx_ctx.get(), spec, "st30p_update_tx0", base_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/false,
                    /*tx_only_mcast=*/false, &expect_fps);
  lb.tx.reset(st30p_tx_create(st, &ops_tx0));
  ASSERT_TRUE(static_cast<bool>(lb.tx));
  EXPECT_EQ(st30p_tx_set_block_timeout(lb.tx.get(), NS_PER_S), 0);
  __atomic_store_n(&lb.tx_ctx->handle, lb.tx.get(), __ATOMIC_RELEASE);

  struct st30p_tx_ops ops_tx1;
  init_st30p_tx_ops(&ops_tx1, tx1_ctx.get(), spec, "st30p_update_tx1", base_port + 2,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  memcpy(ops_tx1.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  tx1.reset(st30p_tx_create(st, &ops_tx1));
  ASSERT_TRUE(static_cast<bool>(tx1));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx1.get(), NS_PER_S), 0);
  __atomic_store_n(&tx1_ctx->handle, tx1.get(), __ATOMIC_RELEASE);

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, lb.rx_ctx.get(), spec, "st30p_update_rx", base_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  lb.rx.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(lb.rx));
  EXPECT_EQ(st30p_rx_set_block_timeout(lb.rx.get(), NS_PER_S), 0);
  __atomic_store_n(&lb.rx_ctx->handle, lb.rx.get(), __ATOMIC_RELEASE);

  lb.start_tx();
  lb.start_rx();
  tx1_worker = mtl_test::ScopedWorker(
      tx1_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx1_ctx.get()),
      [h = tx1.get()]() { st30p_tx_wake_block(h); });

  EXPECT_GE(lb.mtl.start(), 0);
  std::vector<tests_context*> rx_ctxs{lb.rx_ctx.get()};
  EXPECT_TRUE(mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames,
                                     kSt30pPhaseTargetFrames, kSt30pPhaseTimeoutMs));

  auto verify_switch = [&](const char* label, const uint8_t* src_ip, uint16_t udp_port) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st30p_rx_update_source(lb.rx.get(), &src), 0);

    const int base = mtl_test::rx_frames(lb.rx_ctx.get());
    auto delta = [base](const tests_context* c) { return mtl_test::rx_frames(c) - base; };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta, kSt30pRateWarmupFrames,
                                      kSt30pRateMinDeltaFrames, kSt30pRateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up) << label;
    EXPECT_TRUE(rate_res.complete) << label;
    EXPECT_GT(mtl_test::rx_frames(lb.rx_ctx.get()), base);
    EXPECT_NEAR(rate_res.rate[0], expect_fps, expect_fps * 0.1) << label;
  };

  auto update_tx0 = [&](const uint8_t* dip, uint16_t udp_port) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st30p_tx_update_destination(lb.tx.get(), &dst), 0);
  };

  verify_switch("rx_src_mcast1", ctx->mcast_ip_addr[MTL_PORT_P], base_port + 2);

  update_tx0(ctx->mcast_ip_addr[MTL_PORT_R], base_port + 4);
  verify_switch("tx_dst_mcast2", ctx->mcast_ip_addr[MTL_PORT_R], base_port + 4);

  update_tx0(ctx->para.sip_addr[MTL_PORT_R], base_port);
  verify_switch("back_to_init", ctx->para.sip_addr[MTL_PORT_P], base_port);

  tx1_worker.stop();
  lb.stop_all();
}
