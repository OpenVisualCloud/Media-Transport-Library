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

static int test_st30p_tx_frame_available(void* priv) {
  auto s = static_cast<tests_context*>(priv);
  std::lock_guard<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

static int test_st30p_tx_frame_done(void* priv, struct st30_frame* /*frame*/) {
  auto s = static_cast<tests_context*>(priv);
  if (!__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */
  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);
  return 0;
}

static int test_st30p_rx_frame_available(void* priv) {
  auto s = static_cast<tests_context*>(priv);
  std::lock_guard<std::mutex> lck(s->mtx);
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
  const enum mtl_port rx_port = ctx->para.num_ports >= 2 ? MTL_PORT_R : MTL_PORT_P;
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[rx_port]);
  ops_rx->port.udp_port[MTL_SESSION_PORT_P] = ST30P_TEST_UDP_PORT + st30->idx;
  ops_rx->port.payload_type = ST30P_TEST_PAYLOAD_TYPE;
  ops_rx->fmt = ST30_FMT_PCM24;
  ops_rx->channel = 2;
  ops_rx->sampling = ST30_SAMPLING_48K;
  ops_rx->ptime = ST30_PTIME_1MS;
  ops_rx->framebuff_size =
      st30_calculate_framebuff_size(ops_rx->fmt, ops_rx->ptime, ops_rx->sampling,
                                    ops_rx->channel, 10 * NS_PER_MS, NULL);
  ops_rx->framebuff_cnt = st30->fb_cnt;
  ops_rx->notify_frame_available = test_st30p_rx_frame_available;
  st30->frame_size = ops_rx->framebuff_size;
}

static void st30p_tx_assert_cnt(int expect_st30_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  int ret = st_get_var_info(ctx->handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_tx_sessions_cnt, expect_st30_tx_cnt);
}

static void st30p_rx_assert_cnt(int expect_st30_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  int ret = st_get_var_info(ctx->handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st30_rx_sessions_cnt, expect_st30_rx_cnt);
}

namespace {

void st30p_tx_verify_create_free(int base, int step, int repeat) {
  mtl_test::verify_create_free<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                               st30p_tx_free>(st30p_tx_ops_init, st30p_tx_assert_cnt,
                                              base, step, repeat);
}

void st30p_rx_verify_create_free(int base, int step, int repeat) {
  mtl_test::verify_create_free<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                               st30p_rx_free>(st30p_rx_ops_init, st30p_rx_assert_cnt,
                                              base, step, repeat);
}

}  // namespace

TEST(St30p, tx_create_free_single) {
  st30p_tx_verify_create_free(0, 1, 1);
}
TEST(St30p, tx_create_free_multi) {
  st30p_tx_verify_create_free(0, 1, 6);
}
TEST(St30p, tx_create_free_mix) {
  st30p_tx_verify_create_free(2, 3, 4);
}
TEST(St30p, rx_create_free_single) {
  st30p_rx_verify_create_free(0, 1, 1);
}
TEST(St30p, rx_create_free_multi) {
  st30p_rx_verify_create_free(0, 1, 6);
}
TEST(St30p, rx_create_free_mix) {
  st30p_rx_verify_create_free(2, 3, 4);
}
TEST(St30p, tx_create_free_max) {
  mtl_test::verify_create_free_max<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                                   st30p_tx_free>(st30p_tx_ops_init, st30p_tx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St30p, rx_create_free_max) {
  mtl_test::verify_create_free_max<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                                   st30p_rx_free>(st30p_rx_ops_init, st30p_rx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St30p, tx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30p_tx_ops, st30p_tx_handle, st30p_tx_create,
                                 st30p_tx_free>(st30p_tx_ops_init);
}
TEST(St30p, rx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st30p_rx_ops, st30p_rx_handle, st30p_rx_create,
                                 st30p_rx_free>(st30p_rx_ops_init);
}

static void test_st30p_tx_frame_thread(void* args) {
  auto s = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30p_tx_handle>(__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
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
    if (frame->data_size != s->frame_size) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->buffer_size != s->frame_size) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->fmt != s->audio_fmt) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->channel != s->audio_channel) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->ptime != s->audio_ptime) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->sampling != s->audio_sampling) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }

    st30p_tx_put_frame(handle, frame);
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st30p_rx_frame_thread(void* args) {
  auto s = static_cast<tests_context*>(args);
  auto handle =
      static_cast<st30p_rx_handle>(__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  bool has_ts = false;
  uint64_t timestamp = 0;

  dbg("%s(%d), start\n", __func__, s->idx);
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

    if (frame->data_size != s->frame_size) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->buffer_size != s->frame_size) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->fmt != s->audio_fmt) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->channel != s->audio_channel) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->ptime != s->audio_ptime) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (frame->sampling != s->audio_sampling) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    if (!st_is_frame_complete(frame->status)) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }

    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (has_ts && frame->timestamp == timestamp) {
      __atomic_fetch_add(&s->incomplete_frame_cnt, 1, __ATOMIC_RELAXED);
    }
    has_ts = true;
    timestamp = frame->timestamp;

    if (s->check_sha) {
      /*
       * ST 2110-30 RTP packets carry no frame-boundary marker bit, so RX aligns
       * its 10ms frame window to the first received packet. Hash every packet
       * slice across the received framebuffer to verify all payload bytes
       * regardless of packet-phase offset.
       */
      const size_t pkt_len = static_cast<size_t>(s->pkt_data_len);
      bool frame_ok = (pkt_len > 0) && (s->frame_size % pkt_len == 0);
      if (frame_ok) {
        const auto* base = static_cast<const unsigned char*>(frame->addr);
        const int hist_cnt = std::min(s->fb_cnt > 0 ? s->fb_cnt : TEST_MAX_SHA_HIST_NUM,
                                      TEST_MAX_SHA_HIST_NUM);
        for (size_t off = 0; off < s->frame_size; off += pkt_len) {
          unsigned char result[SHA256_DIGEST_LENGTH];
          SHA256(base + off, pkt_len, result);
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
  dbg("%s(%d), stop\n", __func__, s->idx);
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
                       const St30pStreamSpec& spec, const char* name, uint16_t udp_port,
                       uint8_t pt, uint32_t ssrc, bool block_get, bool redundant = false,
                       bool tx_only_mcast = false, double* out_fps = nullptr) {
  auto ctx = st30->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  if (tx_only_mcast) {
    ops->port.num_port = 1;
    memcpy(ops->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops->port.udp_port[MTL_SESSION_PORT_P] = udp_port;
    ops->port.payload_type = pt;
    ops->port.ssrc = ssrc;
  } else {
    mtl_test::fill_tx_port(ops->port, ctx, udp_port, pt, ssrc, redundant);
  }
  ops->fmt = spec.fmt;
  ops->channel = spec.channel;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_size = st30_calculate_framebuff_size(
      ops->fmt, ops->ptime, ops->sampling, ops->channel, 10 * NS_PER_MS, out_fps);
  ops->framebuff_cnt = st30->fb_cnt;
  if (block_get) {
    ops->flags |= ST30P_TX_FLAG_BLOCK_GET;
  } else {
    ops->notify_frame_available = test_st30p_tx_frame_available;
  }
  st30->frame_size = ops->framebuff_size;
  apply_stream_spec(st30, spec, block_get);
}

void init_st30p_rx_ops(struct st30p_rx_ops* ops, tests_context* st30,
                       const St30pStreamSpec& spec, const char* name, uint16_t udp_port,
                       uint8_t pt, uint32_t ssrc, bool block_get,
                       bool redundant = false) {
  auto ctx = st30->ctx;
  memset(ops, 0, sizeof(*ops));
  ops->name = name;
  ops->priv = st30;
  mtl_test::fill_rx_port(ops->port, ctx, udp_port, pt, ssrc, redundant);
  ops->fmt = spec.fmt;
  ops->channel = spec.channel;
  ops->sampling = spec.sampling;
  ops->ptime = spec.ptime;
  ops->framebuff_size = st30_calculate_framebuff_size(ops->fmt, ops->ptime, ops->sampling,
                                                      ops->channel, 10 * NS_PER_MS, NULL);
  ops->framebuff_cnt = st30->fb_cnt;
  if (block_get) {
    ops->flags |= ST30P_RX_FLAG_BLOCK_GET;
  } else {
    ops->notify_frame_available = test_st30p_rx_frame_available;
  }
  st30->frame_size = ops->framebuff_size;
  apply_stream_spec(st30, spec, block_get);
}

}  // namespace

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
  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  raw_tx.reserve(sessions);
  raw_rx.reserve(sessions);
  tx_handles.reserve(sessions);
  rx_handles.reserve(sessions);
  tx_workers.reserve(sessions);
  rx_workers.reserve(sessions);
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
    ASSERT_EQ(frame_size % pkt_len, static_cast<size_t>(0));
    raw_tx[i]->pkt_data_len = pkt_len_i;

    for (int frame = 0; frame < raw_tx[i]->fb_cnt; frame++) {
      auto fb = static_cast<uint8_t*>(st30p_tx_get_fb_addr(handle.get(), frame));
      ASSERT_TRUE(fb != nullptr);
      st_test_rand_data(fb, pkt_len, frame);
      for (size_t off = pkt_len; off < frame_size; off += pkt_len) {
        memcpy(fb + off, fb, pkt_len);
      }
      unsigned char* result = raw_tx[i]->shas[frame];
      SHA256(fb, pkt_len, result);
      test_sha_dump("st30p_tx", result);
    }

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
    const int fb_send = mtl_test::tx_frames(raw_tx[i]);
    info("%s, session %zu fb_send %d framerate %f:%f\n", __func__, i, fb_send,
         win.tx_fps[i], expect_fps[i]);
    EXPECT_GT(fb_send, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&raw_tx[i]->incomplete_frame_cnt), 0);
  }
  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(raw_rx[i]);
    info("%s, session %zu fb_rec %d framerate %f:%f\n", __func__, i, fb_rec,
         win.rx_fps[i], expect_fps[i]);
    EXPECT_GT(fb_rec, 0);
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
  opts.check_fps = true;
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
  opts.check_fps = true;
  opts.block_get = true;

  st30p_rx_digest_test(
      {
          {ST31_FMT_AM824, 3, ST31_SAMPLING_44K, ST31_PTIME_1_09MS},
          {ST30_FMT_PCM16, 5, ST30_SAMPLING_96K, ST30_PTIME_1MS},
          {ST30_FMT_PCM24, 7, ST30_SAMPLING_48K, ST30_PTIME_125US},
      },
      opts, st_test_ctx());
}

TEST(St30p, tx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  const St30pStreamSpec spec;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_abort_test", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/false,
                    /*tx_only_mcast=*/true);

  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  struct st30_frame* frame = st30p_tx_get_frame(tx_handle.get());
  ASSERT_NE(frame, nullptr);
  EXPECT_GE(st30p_tx_put_frame_abort(tx_handle.get(), frame), 0);
  info("%s, st30p_tx_put_frame_abort succeeded\n", __func__);
}

TEST(St30p, rx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  ScopedSt30pRx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);
  mtl_test::ScopedWorker tx_worker;

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_abort_tx", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);
  tx_worker = mtl_test::ScopedWorker(
      tx_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_abort_rx", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  rx_handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);

  EXPECT_GE(mtl_guard.start(), 0);
  /* Wait until TX produces >= 5 frames (~50ms at 10ms per frame) before RX get. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(tx_ctx.get()) >= 5; }, 2000));

  struct st30_frame* frame = st30p_rx_get_frame(rx_handle.get());
  ASSERT_NE(frame, nullptr);
  EXPECT_GE(st30p_rx_put_frame_abort(rx_handle.get(), frame), 0);
  info("%s, st30p_rx_put_frame_abort succeeded\n", __func__);

  tx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30p, frame_is_late) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  struct st30_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.tfmt = ST10_TIMESTAMP_FMT_TAI;
  uint64_t now = mtl_ptp_read_time(st);
  frame.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st30_frame_is_late(st, &frame));

  frame.timestamp = now + static_cast<uint64_t>(10) * NS_PER_S;
  EXPECT_FALSE(st30_frame_is_late(st, &frame));

  frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame.timestamp = 0;
  EXPECT_FALSE(st30_frame_is_late(st, &frame));

  info("%s, st30_frame_is_late tests passed\n", __func__);
}

/*
 * Regression test for issue #1305: verify redundant TX and RX port stats.
 * TX frames must be counted on both ports (was 0 on redundant port).
 * RX bytes must be approximately equal on both ports (was ~93%/7% split).
 * Requires 4 ports: TX on ports 0+1, RX on ports 2+3.
 */
TEST(St30p, redundant_stats) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  const St30pStreamSpec spec;
  const uint16_t udp_port = ST30P_TEST_UDP_PORT + 100;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  ScopedSt30pRx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_redundant_tx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/true);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);
  tx_worker = mtl_test::ScopedWorker(
      tx_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_redundant_rx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/true);
  rx_handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  rx_worker = mtl_test::ScopedWorker(
      rx_ctx.get(), std::thread(test_st30p_rx_frame_thread, rx_ctx.get()),
      [h = rx_handle.get()]() { st30p_rx_wake_block(h); });

  EXPECT_GE(mtl_guard.start(), 0);
  /* Wait until both redundant RX legs report traffic before counting target frames. */
  const bool both_legs_active = mtl_test::wait_until(
      [&]() {
        struct st30_rx_user_stats stats;
        if (st30p_rx_get_session_stats(rx_handle.get(), &stats) < 0) return false;
        return stats.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               stats.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      kSt30pPhaseTimeoutMs);
  EXPECT_TRUE(both_legs_active) << "Both redundant RX legs must receive traffic";

  /* 30 frames (~300ms at 10ms/frame = 300 pkts/port) stabilizes port balance. */
  const int base_rx = mtl_test::rx_frames(rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return mtl_test::rx_frames(rx_ctx.get()) - base_rx >= kSt30pPhaseTargetFrames;
      },
      kSt30pPhaseTimeoutMs));

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  struct st30_tx_user_stats tx_stats;
  EXPECT_GE(st30p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);

  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);

  mtl_test::expect_redundant_port_stats(tx_stats, rx_stats, /*check_tx_packets=*/true);
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 ", RX bytes port0=%" PRIu64
       " port1=%" PRIu64 "\n",
       __func__, tx_stats.common.port[MTL_SESSION_PORT_P].frames,
       tx_stats.common.port[MTL_SESSION_PORT_R].frames,
       rx_stats.common.port[MTL_SESSION_PORT_P].bytes,
       rx_stats.common.port[MTL_SESSION_PORT_R].bytes);

  const int fb_rec = mtl_test::rx_frames(rx_ctx.get());
  const int fb_send = mtl_test::tx_frames(tx_ctx.get());
  EXPECT_GT(fb_rec, 0) << "Must have received frames";
  EXPECT_EQ(rx_stats.common.stat_frames_corrupted, static_cast<uint64_t>(0))
      << "clean redundant session must not report corrupted frames";
  info("%s, TX sent %d, RX received %d frames\n", __func__, fb_send, fb_rec);
}

/*
 * Verify ST30p RX reports per-frame corruption when post-redundancy packet
 * loss occurs. ST30P_RX_FLAG_SIMULATE_PKT_LOSS drops packets on the RX side
 * so frames close short of full coverage; with
 * ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME those frames must be delivered to
 * the app with status ST_FRAME_STATUS_CORRUPTED and counted in
 * common.stat_frames_corrupted.
 */
TEST(St30p, rx_simulate_pkt_loss) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  const uint16_t udp_port = ST30P_TEST_UDP_PORT + 200;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  ScopedSt30pRx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_sim_loss_tx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);
  tx_worker = mtl_test::ScopedWorker(
      tx_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_sim_loss_rx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  ops_rx.flags |=
      ST30P_RX_FLAG_SIMULATE_PKT_LOSS | ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  rx_handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  rx_worker = mtl_test::ScopedWorker(
      rx_ctx.get(), std::thread(test_st30p_rx_frame_thread, rx_ctx.get()),
      [h = rx_handle.get()]() { st30p_rx_wake_block(h); });

  EXPECT_GE(mtl_guard.start(), 0);

  /* Wait for >= 20 RX frames (~200ms) and at least one simulated corrupted frame. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        if (mtl_test::rx_frames(rx_ctx.get()) < 20) return false;
        struct st30_rx_user_stats stats;
        return st30p_rx_get_session_stats(rx_handle.get(), &stats) >= 0 &&
               stats.common.stat_frames_corrupted > 0;
      },
      kSt30pPhaseTimeoutMs));

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);

  const int fb_rec = mtl_test::rx_frames(rx_ctx.get());
  EXPECT_GT(fb_rec, 0) << "Must have received frames";
  EXPECT_GT(rx_stats.common.stat_frames_corrupted, static_cast<uint64_t>(0))
      << "simulated packet loss must produce corrupted frames";
  info("%s, RX received %d frames, %" PRIu64 " corrupted\n", __func__, fb_rec,
       rx_stats.common.stat_frames_corrupted);
}

static void st30p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  const St30pStreamSpec spec;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_epoch_drop_test",
                    EPOCH_DROP_TEST_UDP_PORT_ST30P, ST30P_TEST_PAYLOAD_TYPE, 0,
                    /*block_get=*/true, /*redundant=*/false, /*tx_only_mcast=*/true);
  if (user_pacing) ops_tx.flags |= ST30P_TX_FLAG_USER_PACING;

  /* Create after start, the path that reported bogus epoch drops. */
  EXPECT_GE(mtl_guard.start(), 0);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedWorker tx_worker(
      tx_ctx.get(),
      std::thread(
          [user_pacing](tests_context* s) {
            auto handle = static_cast<st30p_tx_handle>(
                __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
            auto mtl = s->ctx->handle;
            while (!s->stop) {
              auto frame = st30p_tx_get_frame(handle);
              if (!frame) continue;
              if (user_pacing) {
                frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
                frame->timestamp = mtl_ptp_read_time(mtl) + 20 * 1000 * 1000; /* +20ms */
              }
              st30p_tx_put_frame(handle, frame);
              __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
            }
          },
          tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  /* Wait for 30 TX frames (~300ms at 10ms/frame) to exercise epoch pacing. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(tx_ctx.get()) >= kSt30pPhaseTargetFrames; },
      kSt30pPhaseTimeoutMs));

  tx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  struct st30_tx_user_stats stats;
  EXPECT_GE(st30p_tx_get_session_stats(tx_handle.get(), &stats), 0);
  const int fb_send = mtl_test::tx_frames(tx_ctx.get());
  EXPECT_GT(fb_send, 0);
  info("%s, user_pacing %d, fb_send %d, epoch_drop %" PRIu64 "\n", __func__, user_pacing,
       fb_send, stats.common.stat_epoch_drop);
  EXPECT_EQ(stats.common.stat_epoch_drop, static_cast<uint64_t>(0));
}

TEST(St30p, tx_no_epoch_drop) {
  st30p_tx_epoch_drop_test(false);
}

TEST(St30p, tx_user_pacing_no_epoch_drop) {
  st30p_tx_epoch_drop_test(true);
}

TEST(St30p, frame_size) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  constexpr uint16_t kFbCnt = 3;

  const St30pStreamSpec specs[] = {
      {ST30_FMT_PCM16, 2, ST30_SAMPLING_48K, ST30_PTIME_1MS},
      {ST30_FMT_PCM24, 4, ST30_SAMPLING_48K, ST30_PTIME_125US},
      {ST31_FMT_AM824, 2, ST30_SAMPLING_96K, ST30_PTIME_1MS},
  };

  for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
    const auto& spec = specs[i];
    const uint16_t udp_port = ST30P_TEST_UDP_PORT + 250 + static_cast<uint16_t>(i * 2);
    const int expected_sz = st30_calculate_framebuff_size(
        spec.fmt, spec.ptime, spec.sampling, spec.channel, 10 * NS_PER_MS, NULL);
    ASSERT_GT(expected_sz, 0);

    auto tx_ctx = mtl_test::make_context(ctx, static_cast<int>(i), kFbCnt);
    auto rx_ctx = mtl_test::make_context(ctx, static_cast<int>(i), kFbCnt);
    ScopedSt30pTx tx_handle;
    ScopedSt30pRx rx_handle;

    struct st30p_tx_ops ops_tx;
    init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_fsz_tx", udp_port,
                      ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/false,
                      /*redundant=*/false, /*tx_only_mcast=*/true);
    tx_handle.reset(st30p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx_handle));
    EXPECT_EQ(st30p_tx_frame_size(tx_handle.get()), static_cast<size_t>(expected_sz));

    std::vector<void*> fb_addrs;
    fb_addrs.reserve(kFbCnt);
    for (uint16_t idx = 0; idx < kFbCnt; idx++) {
      void* addr = st30p_tx_get_fb_addr(tx_handle.get(), idx);
      EXPECT_NE(addr, nullptr);
      EXPECT_EQ(std::find(fb_addrs.begin(), fb_addrs.end(), addr), fb_addrs.end());
      fb_addrs.push_back(addr);
    }
    EXPECT_EQ(st30p_tx_get_fb_addr(tx_handle.get(), kFbCnt), nullptr);
    EXPECT_EQ(st30p_tx_get_fb_addr(tx_handle.get(), static_cast<uint16_t>(kFbCnt * 2)),
              nullptr);

    struct st30p_rx_ops ops_rx;
    init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_fsz_rx", udp_port,
                      ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/false);
    if (ctx->para.num_ports < 2) {
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
      snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
               ctx->para.port[MTL_PORT_P]);
    }
    rx_handle.reset(st30p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(rx_handle));
    EXPECT_EQ(st30p_rx_frame_size(rx_handle.get()), static_cast<size_t>(expected_sz));
  }
}

TEST(St30p, session_stats_reset) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  const uint16_t udp_port = ST30P_TEST_UDP_PORT + 280;

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ScopedSt30pTx tx_handle;
  ScopedSt30pRx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx_ctx.get(), spec, "st30p_stats_tx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  tx_handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx_ctx->handle, tx_handle.get(), __ATOMIC_RELEASE);
  tx_worker = mtl_test::ScopedWorker(
      tx_ctx.get(), std::thread(test_st30p_tx_frame_thread, tx_ctx.get()),
      [h = tx_handle.get()]() { st30p_tx_wake_block(h); });

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_stats_rx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  rx_handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  rx_worker = mtl_test::ScopedWorker(
      rx_ctx.get(), std::thread(test_st30p_rx_frame_thread, rx_ctx.get()),
      [h = rx_handle.get()]() { st30p_rx_wake_block(h); });

  struct st30_tx_user_stats tx_stats;
  struct st30_rx_user_stats rx_stats;
  EXPECT_LT(st30p_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st30p_tx_get_session_stats(tx_handle.get(), nullptr), 0);
  EXPECT_LT(st30p_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st30p_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st30p_rx_get_session_stats(rx_handle.get(), nullptr), 0);
  EXPECT_LT(st30p_rx_reset_session_stats(nullptr), 0);

  EXPECT_GE(mtl_guard.start(), 0);
  /* 30 frames (~300ms at 10ms/frame) populates pipeline TX and RX stats. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= kSt30pPhaseTargetFrames; },
      kSt30pPhaseTimeoutMs));

  tx_worker.stop();
  rx_worker.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  EXPECT_EQ(st30p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.stat_frames_sent, static_cast<uint64_t>(0));
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));

  EXPECT_EQ(st30p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.stat_frames_received, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));

  EXPECT_EQ(st30p_tx_reset_session_stats(tx_handle.get()), 0);
  EXPECT_EQ(st30p_rx_reset_session_stats(rx_handle.get()), 0);

  EXPECT_EQ(st30p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.stat_frames_sent, static_cast<uint64_t>(0));
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));

  EXPECT_EQ(st30p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.stat_frames_received, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, static_cast<uint64_t>(0));
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].frames, static_cast<uint64_t>(0));
}

static void st30p_update_stream_test(bool tx_update_dst) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  const int tx_sessions = tx_update_dst ? 1 : 2;
  const uint16_t base_port = ST30P_TEST_UDP_PORT + 300;
  double expect_fps = 100.0;

  std::vector<mtl_test::ScopedContext> tx_ctxs;
  std::vector<tests_context*> raw_tx;
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  std::vector<ScopedSt30pTx> tx_handles;
  ScopedSt30pRx rx_handle;
  mtl_test::ScopedMtlStart mtl_guard(st);
  std::vector<mtl_test::ScopedWorker> tx_workers;
  mtl_test::ScopedWorker rx_worker;
  tx_ctxs.reserve(tx_sessions);
  raw_tx.reserve(tx_sessions);
  tx_handles.reserve(tx_sessions);
  tx_workers.reserve(tx_sessions);

  for (int i = 0; i < tx_sessions; i++) {
    tx_ctxs.push_back(mtl_test::make_context(ctx, i, 3));
    raw_tx.push_back(tx_ctxs.back().get());

    struct st30p_tx_ops ops_tx;
    init_st30p_tx_ops(&ops_tx, raw_tx[i], spec, "st30p_update_tx",
                      base_port + static_cast<uint16_t>(i * 2), ST30P_TEST_PAYLOAD_TYPE,
                      0, /*block_get=*/true, /*redundant=*/false,
                      /*tx_only_mcast=*/false, i == 0 ? &expect_fps : nullptr);
    if (i == 1) {
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    } else if (ctx->mcast_only) {
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }

    ScopedSt30pTx handle(st30p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(handle));
    EXPECT_EQ(st30p_tx_set_block_timeout(handle.get(), NS_PER_S), 0);
    st30p_tx_handle raw_h = handle.get();
    __atomic_store_n(&raw_tx[i]->handle, raw_h, __ATOMIC_RELEASE);
    tx_handles.push_back(std::move(handle));
    tx_workers.emplace_back(raw_tx[i], std::thread(test_st30p_tx_frame_thread, raw_tx[i]),
                            [raw_h]() { st30p_tx_wake_block(raw_h); });
  }

  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx_ctx.get(), spec, "st30p_update_rx", base_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  if (ctx->mcast_only) {
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
           MTL_IP_ADDR_LEN);
  }
  rx_handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx_ctx->handle, rx_handle.get(), __ATOMIC_RELEASE);
  rx_worker = mtl_test::ScopedWorker(
      rx_ctx.get(), std::thread(test_st30p_rx_frame_thread, rx_ctx.get()),
      [h = rx_handle.get()]() { st30p_rx_wake_block(h); });

  EXPECT_GE(mtl_guard.start(), 0);
  std::vector<tests_context*> rx_ctxs{rx_ctx.get()};
  /* Wait for initial stream establishment (~300ms at 100 fps). */
  EXPECT_TRUE(mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames,
                                     kSt30pPhaseTargetFrames, kSt30pPhaseTimeoutMs));

  auto verify_switch = [&](const char* label, const uint8_t* src_ip, uint16_t udp_port) {
    struct st_rx_source_info src;
    memset(&src, 0, sizeof(src));
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);

    EXPECT_GE(st30p_rx_update_source(rx_handle.get(), &src), 0);
    const int baseline = mtl_test::rx_frames(rx_ctx.get());
    auto delta_frames = [baseline](const tests_context* c) {
      return mtl_test::rx_frames(c) - baseline;
    };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta_frames, kSt30pRateWarmupFrames,
                                      kSt30pRateMinDeltaFrames, kSt30pRateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up) << label;
    EXPECT_TRUE(rate_res.complete) << label;
    const int fb_rec = mtl_test::rx_frames(rx_ctx.get());
    EXPECT_GT(fb_rec, baseline);
    info("%s, %s delta_rec %d framerate %f:%f\n", __func__, label, fb_rec - baseline,
         rate_res.rate[0], expect_fps);
    EXPECT_NEAR(rate_res.rate[0], expect_fps, expect_fps * 0.1);
  };

  /* Switch to multicast stream (port P). */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = base_port + 2;
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    EXPECT_GE(st30p_tx_update_destination(tx_handles[0].get(), &dst), 0);
  }
  verify_switch("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P], base_port + 2);

  /* Switch back to initial stream (unicast or mcast port 2 on mcast-only hosts). */
  if (tx_update_dst) {
    struct st_tx_dest_info dst;
    memset(&dst, 0, sizeof(dst));
    dst.udp_port[MTL_SESSION_PORT_P] = base_port;
    const uint8_t* dip =
        ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_R];
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st30p_tx_update_destination(tx_handles[0].get(), &dst), 0);
  }
  const uint8_t* back_ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P];
  verify_switch(ctx->mcast_only ? "mcast 0" : "unicast 0", back_ip, base_port);

  rx_worker.stop();
  for (auto& w : tx_workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

TEST(St30p, rx_update_source) {
  st30p_update_stream_test(/*tx_update_dst=*/false);
}

TEST(St30p, tx_update_destination) {
  st30p_update_stream_test(/*tx_update_dst=*/true);
}
