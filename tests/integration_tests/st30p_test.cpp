/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <algorithm>
#include <chrono>
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
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
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
    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->fmt != s->audio_fmt) s->incomplete_frame_cnt++;
    if (frame->channel != s->audio_channel) s->incomplete_frame_cnt++;
    if (frame->ptime != s->audio_ptime) s->incomplete_frame_cnt++;
    if (frame->sampling != s->audio_sampling) s->incomplete_frame_cnt++;
    if (s->user_timestamp) {
      const int sent = mtl_test::tx_frames(s);
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = sent;
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, sent);
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

    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->fmt != s->audio_fmt) s->incomplete_frame_cnt++;
    if (frame->channel != s->audio_channel) s->incomplete_frame_cnt++;
    if (frame->ptime != s->audio_ptime) s->incomplete_frame_cnt++;
    if (frame->sampling != s->audio_sampling) s->incomplete_frame_cnt++;
    if (!st_is_frame_complete(frame->status)) s->incomplete_frame_cnt++;

    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == timestamp) s->incomplete_frame_cnt++;
    timestamp = frame->timestamp;
    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp) {
        /* SHA256 can drop frames under load; check timestamp step stays small. */
        if ((static_cast<uint32_t>(frame->timestamp) - s->pre_timestamp) > 4) {
          s->incomplete_frame_cnt++;
          err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
              s->idx, frame->timestamp, s->pre_timestamp);
        }
      }
      s->pre_timestamp = static_cast<uint32_t>(frame->timestamp);
    }

    if (s->check_sha) {
      unsigned char result[SHA256_DIGEST_LENGTH];
      SHA256(static_cast<unsigned char*>(frame->addr), s->frame_size, result);
      int i = 0;
      for (i = 0; i < TEST_MAX_SHA_HIST_NUM; i++) {
        if (!memcmp(result, s->shas[i], SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_MAX_SHA_HIST_NUM) {
        test_sha_dump("st30p_rx_error_sha", result);
        s->sha_fail_cnt++;
      }
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
  enum st_test_level level = ST_TEST_LEVEL_MANDATORY;
  int fb_cnt = TEST_MAX_SHA_HIST_NUM;
  uint32_t ssrc = 0;
  bool block_get = false;
  bool dedicated_tx_queue = false;
  bool zero_payload_type = false;
};

template <typename Handle, int (*FreeFn)(Handle), int (*WakeFn)(Handle)>
struct St30pSession {
  mtl_test::ScopedContext ctx;
  mtl_test::ScopedHandle<Handle, FreeFn> handle;
  std::thread worker;
  bool block_get = false;

  St30pSession() = default;
  St30pSession(St30pSession&&) noexcept = default;
  St30pSession& operator=(St30pSession&& other) noexcept {
    if (this != &other) {
      stop();
      handle = std::move(other.handle);
      ctx = std::move(other.ctx);
      worker = std::move(other.worker);
      block_get = other.block_get;
    }
    return *this;
  }
  St30pSession(const St30pSession&) = delete;
  St30pSession& operator=(const St30pSession&) = delete;
  ~St30pSession() {
    stop();
  }
  void stop() {
    auto h = handle.get();
    mtl_test::stop_worker(ctx.get(), worker, [this, h]() {
      if (block_get && h) WakeFn(h);
    });
  }
};

using St30pTxSession = St30pSession<st30p_tx_handle, st30p_tx_free, st30p_tx_wake_block>;
using St30pRxSession = St30pSession<st30p_rx_handle, st30p_rx_free, st30p_rx_wake_block>;

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
  MTL_TEST_REQUIRE_LEVEL(ctx, opts.level);

  const size_t sessions = specs.size();
  const uint8_t pt = opts.zero_payload_type ? 0 : ST30P_TEST_PAYLOAD_TYPE;

  mtl_test::ScopedMtlStart mtl_guard(st);
  std::vector<St30pTxSession> tx;
  std::vector<St30pRxSession> rx;
  tx.reserve(sessions);
  rx.reserve(sessions);
  std::vector<double> expect_fps(sessions, 0.0);

  for (size_t i = 0; i < sessions; i++) {
    const uint16_t udp_port = ST30P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    tx.emplace_back();
    auto& s = tx.back();
    s.block_get = opts.block_get;
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), opts.fb_cnt);

    struct st30p_tx_ops ops_tx;
    init_st30p_tx_ops(&ops_tx, s.ctx.get(), specs[i], "st30p_test", udp_port, pt,
                      opts.ssrc, opts.block_get, /*redundant=*/false,
                      /*tx_only_mcast=*/false, &expect_fps[i]);
    if (opts.dedicated_tx_queue) ops_tx.flags |= ST30P_TX_FLAG_DEDICATE_QUEUE;
    ops_tx.notify_frame_done = test_st30p_tx_frame_done;

    s.handle.reset(st30p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    if (opts.block_get) {
      EXPECT_EQ(st30p_tx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
    }

    const size_t frame_size = s.ctx->frame_size;
    for (int frame = 0; frame < s.ctx->fb_cnt; frame++) {
      auto fb = static_cast<uint8_t*>(st30p_tx_get_fb_addr(s.handle.get(), frame));
      ASSERT_TRUE(fb != nullptr);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = s.ctx->shas[frame];
      SHA256(fb, frame_size, result);
      test_sha_dump("st30p_tx", result);
    }

    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    s.worker = std::thread(test_st30p_tx_frame_thread, s.ctx.get());
  }

  for (size_t i = 0; i < sessions; i++) {
    const uint16_t udp_port = ST30P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);
    rx.emplace_back();
    auto& s = rx.back();
    s.block_get = opts.block_get;
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), opts.fb_cnt,
                                   /*check_sha=*/true);

    struct st30p_rx_ops ops_rx;
    init_st30p_rx_ops(&ops_rx, s.ctx.get(), specs[i], "st30p_test", udp_port, pt,
                      opts.ssrc, opts.block_get);
    ops_rx.framebuff_size = static_cast<uint32_t>(tx[i].ctx->frame_size);
    s.ctx->frame_size = tx[i].ctx->frame_size;
    memcpy(s.ctx->shas, tx[i].ctx->shas, TEST_MAX_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    s.handle.reset(st30p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    if (opts.block_get) {
      EXPECT_EQ(st30p_rx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
    }

    __atomic_store_n(&s.ctx->handle, s.handle.get(), __ATOMIC_RELEASE);
    s.worker = std::thread(test_st30p_rx_frame_thread, s.ctx.get());
  }

  EXPECT_GE(mtl_guard.start(), 0);

  std::vector<tests_context*> tx_ctxs, rx_ctxs;
  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  for (size_t i = 0; i < sessions; i++) {
    tx_ctxs.push_back(tx[i].ctx.get());
    rx_ctxs.push_back(rx[i].ctx.get());
  }

  auto win =
      mtl_test::measure_pipeline_window(tx_ctxs, rx_ctxs, kSt30pRateWarmupFrames,
                                        kSt30pRateMinDeltaFrames, kSt30pRateMinWindowMs);

  for (auto& s : tx) s.stop();
  for (auto& s : rx) s.stop();

  for (size_t i = 0; i < sessions; i++) {
    const int fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    info("%s, session %zu fb_send %d framerate %f:%f\n", __func__, i, fb_send,
         win.tx_fps[i], expect_fps[i]);
    EXPECT_GT(fb_send, 0);
  }
  for (size_t i = 0; i < sessions; i++) {
    const int fb_rec = mtl_test::rx_frames(rx[i].ctx.get());
    info("%s, session %zu fb_rec %d framerate %f:%f\n", __func__, i, fb_rec,
         win.rx_fps[i], expect_fps[i]);
    EXPECT_GT(fb_rec, 0);
    EXPECT_LE(rx[i].ctx->incomplete_frame_cnt, 4);
    size_t pkt_len =
        st30_get_packet_size(rx[i].ctx->audio_fmt, rx[i].ctx->audio_ptime,
                             rx[i].ctx->audio_sampling, rx[i].ctx->audio_channel);
    if (pkt_len == rx[i].ctx->frame_size) {
      EXPECT_EQ(rx[i].ctx->sha_fail_cnt, 0);
    }
    EXPECT_LE(rx[i].ctx->user_meta_fail_cnt, 2);
    if (opts.check_fps) {
      EXPECT_NEAR(win.rx_fps[i], expect_fps[i], expect_fps[i] * 0.1);
    }
  }
}

TEST(St30p, digest_s3) {
  St30pDigestOptions opts;
  opts.level = ST_TEST_LEVEL_MANDATORY;
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
  opts.level = ST_TEST_LEVEL_MANDATORY;
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

  St30pTxSession tx;
  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx.ctx.get(), spec, "st30p_abort_test", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/false,
                    /*tx_only_mcast=*/true);

  tx.handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx.handle.get(), NS_PER_S), 0);

  struct st30_frame* frame = st30p_tx_get_frame(tx.handle.get());
  if (frame) {
    EXPECT_GE(st30p_tx_put_frame_abort(tx.handle.get(), frame), 0);
    info("%s, st30p_tx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no frame available for st30p TX abort test\n", __func__);
  }
}

TEST(St30p, rx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const St30pStreamSpec spec;
  mtl_test::ScopedMtlStart mtl_guard(st);
  St30pTxSession tx;
  St30pRxSession rx;

  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx.ctx.get(), spec, "st30p_abort_tx", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  tx.handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  __atomic_store_n(&tx.ctx->handle, tx.handle.get(), __ATOMIC_RELEASE);
  tx.worker = std::thread(test_st30p_tx_frame_thread, tx.ctx.get());

  rx.block_get = true;
  rx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx.ctx.get(), spec, "st30p_abort_rx", ST30P_TEST_UDP_PORT,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  rx.handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx.handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx.handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx.ctx->handle, rx.handle.get(), __ATOMIC_RELEASE);

  EXPECT_GE(mtl_guard.start(), 0);
  /* Wait until TX produces >= 5 frames (~50ms at 10ms per frame) before RX get. */
  mtl_test::wait_until([&]() { return mtl_test::tx_frames(tx.ctx.get()) >= 5; }, 2000);

  struct st30_frame* frame = st30p_rx_get_frame(rx.handle.get());
  if (frame) {
    EXPECT_GE(st30p_rx_put_frame_abort(rx.handle.get(), frame), 0);
    info("%s, st30p_rx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no rx frame available for st30p abort test\n", __func__);
  }

  tx.stop();
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

  mtl_test::ScopedMtlStart mtl_guard(st);
  St30pTxSession tx;
  St30pRxSession rx;

  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx.ctx.get(), spec, "st30p_redundant_tx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/true);
  tx.handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  __atomic_store_n(&tx.ctx->handle, tx.handle.get(), __ATOMIC_RELEASE);
  tx.worker = std::thread(test_st30p_tx_frame_thread, tx.ctx.get());

  rx.block_get = true;
  rx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx.ctx.get(), spec, "st30p_redundant_rx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true, /*redundant=*/true);
  rx.handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx.handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx.handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx.ctx->handle, rx.handle.get(), __ATOMIC_RELEASE);
  rx.worker = std::thread(test_st30p_rx_frame_thread, rx.ctx.get());

  EXPECT_GE(mtl_guard.start(), 0);
  /* 30 frames (~300ms at 10ms/frame = 300 pkts/port) stabilizes port balance. */
  std::vector<tests_context*> rx_ctxs{rx.ctx.get()};
  mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt30pPhaseTargetFrames,
                         kSt30pPhaseTimeoutMs);

  tx.stop();
  rx.stop();

  struct st30_tx_user_stats tx_stats;
  EXPECT_GE(st30p_tx_get_session_stats(tx.handle.get(), &tx_stats), 0);

  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_rx_get_session_stats(rx.handle.get(), &rx_stats), 0);

  uint64_t tx_frames_p = tx_stats.common.port[MTL_SESSION_PORT_P].frames;
  uint64_t tx_frames_r = tx_stats.common.port[MTL_SESSION_PORT_R].frames;
  EXPECT_GT(tx_frames_p, static_cast<uint64_t>(0)) << "TX port 0 frames must be > 0";
  EXPECT_GT(tx_frames_r, static_cast<uint64_t>(0)) << "TX port 1 frames must be > 0";
  EXPECT_EQ(tx_frames_p, tx_frames_r)
      << "TX port frames should be equal (port0=" << tx_frames_p
      << " port1=" << tx_frames_r << ")";
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, tx_frames_p,
       tx_frames_r);

  uint64_t tx_pkts_p = tx_stats.common.port[MTL_SESSION_PORT_P].packets;
  uint64_t tx_pkts_r = tx_stats.common.port[MTL_SESSION_PORT_R].packets;
  EXPECT_GT(tx_pkts_p, static_cast<uint64_t>(0)) << "TX port 0 packets must be > 0";
  EXPECT_GT(tx_pkts_r, static_cast<uint64_t>(0)) << "TX port 1 packets must be > 0";
  uint64_t tx_max_pkts = std::max(tx_pkts_p, tx_pkts_r);
  uint64_t tx_min_pkts = std::min(tx_pkts_p, tx_pkts_r);
  EXPECT_GT(tx_min_pkts, tx_max_pkts * 80 / 100)
      << "TX port packets should be approximately equal (port0=" << tx_pkts_p
      << " port1=" << tx_pkts_r << ")";

  uint64_t rx_bytes_p = rx_stats.common.port[MTL_SESSION_PORT_P].bytes;
  uint64_t rx_bytes_r = rx_stats.common.port[MTL_SESSION_PORT_R].bytes;
  EXPECT_GT(rx_bytes_p, static_cast<uint64_t>(0)) << "RX port 0 bytes must be > 0";
  EXPECT_GT(rx_bytes_r, static_cast<uint64_t>(0)) << "RX port 1 bytes must be > 0";
  uint64_t rx_max_bytes = std::max(rx_bytes_p, rx_bytes_r);
  uint64_t rx_min_bytes = std::min(rx_bytes_p, rx_bytes_r);
  EXPECT_GT(rx_min_bytes, rx_max_bytes * 80 / 100)
      << "RX port bytes should be approximately equal (port0=" << rx_bytes_p
      << " port1=" << rx_bytes_r << ")";
  info("%s, RX bytes port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, rx_bytes_p,
       rx_bytes_r);

  const int fb_rec = mtl_test::rx_frames(rx.ctx.get());
  const int fb_send = mtl_test::tx_frames(tx.ctx.get());
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

  mtl_test::ScopedMtlStart mtl_guard(st);
  St30pTxSession tx;
  St30pRxSession rx;

  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx.ctx.get(), spec, "st30p_sim_loss_tx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  tx.handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  __atomic_store_n(&tx.ctx->handle, tx.handle.get(), __ATOMIC_RELEASE);
  tx.worker = std::thread(test_st30p_tx_frame_thread, tx.ctx.get());

  rx.block_get = true;
  rx.ctx = mtl_test::make_context(ctx, 0, 3);
  struct st30p_rx_ops ops_rx;
  init_st30p_rx_ops(&ops_rx, rx.ctx.get(), spec, "st30p_sim_loss_rx", udp_port,
                    ST30P_TEST_PAYLOAD_TYPE, 0, /*block_get=*/true);
  ops_rx.flags |=
      ST30P_RX_FLAG_SIMULATE_PKT_LOSS | ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  rx.handle.reset(st30p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx.handle));
  EXPECT_EQ(st30p_rx_set_block_timeout(rx.handle.get(), NS_PER_S), 0);
  __atomic_store_n(&rx.ctx->handle, rx.handle.get(), __ATOMIC_RELEASE);
  rx.worker = std::thread(test_st30p_rx_frame_thread, rx.ctx.get());

  EXPECT_GE(mtl_guard.start(), 0);

  /* Wait for >= 20 RX frames (~200ms) and at least one simulated corrupted frame. */
  mtl_test::wait_until(
      [&]() {
        if (mtl_test::rx_frames(rx.ctx.get()) < 20) return false;
        struct st30_rx_user_stats stats;
        return st30p_rx_get_session_stats(rx.handle.get(), &stats) >= 0 &&
               stats.common.stat_frames_corrupted > 0;
      },
      kSt30pPhaseTimeoutMs);

  tx.stop();
  rx.stop();

  struct st30_rx_user_stats rx_stats;
  EXPECT_GE(st30p_rx_get_session_stats(rx.handle.get(), &rx_stats), 0);

  const int fb_rec = mtl_test::rx_frames(rx.ctx.get());
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

  mtl_test::ScopedMtlStart mtl_guard(st);
  St30pTxSession tx;
  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);

  struct st30p_tx_ops ops_tx;
  init_st30p_tx_ops(&ops_tx, tx.ctx.get(), spec, "st30p_epoch_drop_test",
                    EPOCH_DROP_TEST_UDP_PORT_ST30P, ST30P_TEST_PAYLOAD_TYPE, 0,
                    /*block_get=*/true, /*redundant=*/false, /*tx_only_mcast=*/true);
  if (user_pacing) ops_tx.flags |= ST30P_TX_FLAG_USER_PACING;

  /* Create after start, the path that reported bogus epoch drops. */
  EXPECT_GE(mtl_guard.start(), 0);
  tx.handle.reset(st30p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  EXPECT_EQ(st30p_tx_set_block_timeout(tx.handle.get(), NS_PER_S), 0);
  __atomic_store_n(&tx.ctx->handle, tx.handle.get(), __ATOMIC_RELEASE);

  tx.worker = std::thread(
      [user_pacing](tests_context* s) {
        auto handle =
            static_cast<st30p_tx_handle>(__atomic_load_n(&s->handle, __ATOMIC_ACQUIRE));
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
      tx.ctx.get());

  /* Wait for 30 TX frames (~300ms at 10ms/frame) to exercise epoch pacing. */
  mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(tx.ctx.get()) >= kSt30pPhaseTargetFrames; },
      kSt30pPhaseTimeoutMs);

  tx.stop();

  struct st30_tx_user_stats stats;
  EXPECT_GE(st30p_tx_get_session_stats(tx.handle.get(), &stats), 0);
  const int fb_send = mtl_test::tx_frames(tx.ctx.get());
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
