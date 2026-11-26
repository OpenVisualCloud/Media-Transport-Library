/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <mtl/mtl_sch_api.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "tests.hpp"

namespace mtl_test {

/* RAII guard for mtl_start / mtl_stop. */
class ScopedMtlStart {
 public:
  explicit ScopedMtlStart(mtl_handle h) : handle_(h) {
  }
  ~ScopedMtlStart() {
    EXPECT_GE(stop(), 0);
  }
  ScopedMtlStart(const ScopedMtlStart&) = delete;
  ScopedMtlStart& operator=(const ScopedMtlStart&) = delete;

  int start() {
    if (!handle_) return -EINVAL;
    const int ret = mtl_start(handle_);
    if (ret >= 0) started_ = true;
    return ret;
  }

  int stop() {
    if (!started_ || !handle_) return 0;
    started_ = false;
    return mtl_stop(handle_);
  }

 private:
  mtl_handle handle_ = nullptr;
  bool started_ = false;
};

/* Move-only RAII wrapper for any MTL session, DMA, or tasklet handle. */
template <typename Handle, int (*FreeFn)(Handle)>
class ScopedHandle {
 public:
  ScopedHandle() = default;
  explicit ScopedHandle(Handle h) : handle_(h) {
  }
  ~ScopedHandle() {
    reset();
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {
  }
  ScopedHandle& operator=(ScopedHandle&& o) noexcept {
    if (this != &o) {
      reset();
      handle_ = std::exchange(o.handle_, nullptr);
    }
    return *this;
  }

  Handle get() const {
    return handle_;
  }
  explicit operator bool() const {
    return handle_ != nullptr;
  }

  void reset(Handle h = nullptr) {
    if (handle_ == h) return;
    if (Handle old = std::exchange(handle_, h)) {
      EXPECT_GE(FreeFn(old), 0);
    }
  }

  Handle release() {
    return std::exchange(handle_, nullptr);
  }

 private:
  Handle handle_ = nullptr;
};

/* Deleter for tests_context that runs tests_context_unit() before delete. */
struct ContextDeleter {
  void operator()(tests_context* c) const {
    if (!c) return;
    tests_context_unit(c);
    delete c;
  }
};
using ScopedContext = std::unique_ptr<tests_context, ContextDeleter>;

inline ScopedContext make_context(struct st_tests_context* global_ctx, int idx = 0,
                                  uint16_t fb_cnt = 3, bool check_sha = false) {
  ScopedContext c(new tests_context());
  c->ctx = global_ctx;
  c->idx = idx;
  c->fb_cnt = fb_cnt;
  c->fb_idx = 0;
  c->check_sha = check_sha;
  return c;
}

/* Move-only RAII wrapper that stops, wakes, and joins a worker thread on exit. */
class ScopedWorker {
 public:
  ScopedWorker() = default;
  ScopedWorker(tests_context* ctx, std::thread th,
               std::function<void()> wake_fn = nullptr)
      : ctx_(ctx), th_(std::move(th)), wake_fn_(std::move(wake_fn)) {
  }
  ~ScopedWorker() {
    stop();
  }
  ScopedWorker(const ScopedWorker&) = delete;
  ScopedWorker& operator=(const ScopedWorker&) = delete;
  ScopedWorker(ScopedWorker&& o) noexcept
      : ctx_(std::exchange(o.ctx_, nullptr)),
        th_(std::move(o.th_)),
        wake_fn_(std::exchange(o.wake_fn_, nullptr)) {
  }
  ScopedWorker& operator=(ScopedWorker&& o) noexcept {
    if (this != &o) {
      stop();
      ctx_ = std::exchange(o.ctx_, nullptr);
      th_ = std::move(o.th_);
      wake_fn_ = std::exchange(o.wake_fn_, nullptr);
    }
    return *this;
  }

  void stop() {
    tests_context* ctx = std::exchange(ctx_, nullptr);
    std::function<void()> wake_fn = std::exchange(wake_fn_, nullptr);
    if (ctx) ctx->stop = true;
    if (wake_fn) wake_fn();
    if (ctx) {
      std::lock_guard<std::mutex> lck(ctx->mtx);
      ctx->cv.notify_all();
    }
    if (th_.joinable()) th_.join();
  }

 private:
  tests_context* ctx_ = nullptr;
  std::thread th_;
  std::function<void()> wake_fn_;
};

/* RAII guard that configures and resets mock plugin fault-injection knobs. */
class ScopedPluginFaults {
 public:
  ScopedPluginFaults(struct st_tests_context* ctx, int fail_interval = 0,
                     int timeout_interval = 0, int timeout_ms = 0, int rand_ratio = 0,
                     bool use_block_get = false)
      : ctx_(ctx) {
    if (!ctx_) return;
    prev_fail_interval_ = std::exchange(ctx_->plugin_fail_interval, fail_interval);
    prev_timeout_interval_ =
        std::exchange(ctx_->plugin_timeout_interval, timeout_interval);
    prev_timeout_ms_ = std::exchange(ctx_->plugin_timeout_ms, timeout_ms);
    prev_rand_ratio_ = std::exchange(ctx_->plugin_rand_ratio, rand_ratio);
    prev_encoder_use_block_get_ =
        std::exchange(ctx_->encoder_use_block_get, use_block_get);
    prev_decoder_use_block_get_ =
        std::exchange(ctx_->decoder_use_block_get, use_block_get);
  }
  ~ScopedPluginFaults() {
    if (!ctx_) return;
    ctx_->plugin_fail_interval = prev_fail_interval_;
    ctx_->plugin_timeout_interval = prev_timeout_interval_;
    ctx_->plugin_timeout_ms = prev_timeout_ms_;
    ctx_->plugin_rand_ratio = prev_rand_ratio_;
    ctx_->encoder_use_block_get = prev_encoder_use_block_get_;
    ctx_->decoder_use_block_get = prev_decoder_use_block_get_;
  }
  ScopedPluginFaults(const ScopedPluginFaults&) = delete;
  ScopedPluginFaults& operator=(const ScopedPluginFaults&) = delete;

 private:
  struct st_tests_context* ctx_ = nullptr;
  int prev_fail_interval_ = 0;
  int prev_timeout_interval_ = 0;
  int prev_timeout_ms_ = 0;
  int prev_rand_ratio_ = 0;
  bool prev_encoder_use_block_get_ = false;
  bool prev_decoder_use_block_get_ = false;
};

/* Move-only RAII guard for an lcore acquired via mtl_get_lcore(). */
class ScopedLcore {
 public:
  ScopedLcore() = default;
  ScopedLcore(mtl_handle h, unsigned int lcore)
      : handle_(h), lcore_(lcore), owned_(h != nullptr) {
  }
  ~ScopedLcore() {
    if (owned_) {
      EXPECT_GE(reset(), 0);
    }
  }
  ScopedLcore(const ScopedLcore&) = delete;
  ScopedLcore& operator=(const ScopedLcore&) = delete;
  ScopedLcore(ScopedLcore&& o) noexcept
      : handle_(std::exchange(o.handle_, nullptr)),
        lcore_(std::exchange(o.lcore_, 0)),
        owned_(std::exchange(o.owned_, false)) {
  }
  ScopedLcore& operator=(ScopedLcore&& o) noexcept {
    if (this != &o) {
      if (owned_) {
        EXPECT_GE(reset(), 0);
      }
      handle_ = std::exchange(o.handle_, nullptr);
      lcore_ = std::exchange(o.lcore_, 0);
      owned_ = std::exchange(o.owned_, false);
    }
    return *this;
  }

  unsigned int id() const {
    return lcore_;
  }
  explicit operator bool() const {
    return owned_;
  }

  int reset() {
    if (!owned_ || !handle_) {
      owned_ = false;
      return 0;
    }
    owned_ = false;
    return mtl_put_lcore(handle_, lcore_);
  }

 private:
  mtl_handle handle_ = nullptr;
  unsigned int lcore_ = 0;
  bool owned_ = false;
};

using ScopedTasklet = ScopedHandle<mtl_tasklet_handle, mtl_sch_unregister_tasklet>;

/* Move-only RAII guard for mtl_sch_handle: stops (if started) and frees on exit. */
class ScopedSch {
 public:
  ScopedSch() = default;
  explicit ScopedSch(mtl_sch_handle h) : handle_(h) {
  }
  ~ScopedSch() {
    reset();
  }
  ScopedSch(const ScopedSch&) = delete;
  ScopedSch& operator=(const ScopedSch&) = delete;
  ScopedSch(ScopedSch&& o) noexcept
      : handle_(std::exchange(o.handle_, nullptr)),
        started_(std::exchange(o.started_, false)) {
  }
  ScopedSch& operator=(ScopedSch&& o) noexcept {
    if (this != &o) {
      reset();
      handle_ = std::exchange(o.handle_, nullptr);
      started_ = std::exchange(o.started_, false);
    }
    return *this;
  }

  mtl_sch_handle get() const {
    return handle_;
  }
  explicit operator bool() const {
    return handle_ != nullptr;
  }

  int start() {
    if (!handle_) return -EINVAL;
    const int ret = mtl_sch_start(handle_);
    if (ret >= 0) started_ = true;
    return ret;
  }

  int stop() {
    if (!started_ || !handle_) return 0;
    started_ = false;
    return mtl_sch_stop(handle_);
  }

  void reset() {
    if (started_ && handle_) {
      EXPECT_GE(stop(), 0);
    }
    if (mtl_sch_handle old = std::exchange(handle_, nullptr)) {
      EXPECT_GE(mtl_sch_free(old), 0);
    }
  }

 private:
  mtl_sch_handle handle_ = nullptr;
  bool started_ = false;
};

/* Standard raw and pipeline port fill helpers (eliminates copy-pasted IP/port setup). */
template <typename TxOps>
inline void fill_raw_tx_port(TxOps& ops, const struct st_tests_context* ctx,
                             uint16_t udp_port, uint8_t pt, uint32_t ssrc = 0,
                             bool redundant = false) {
  ops.num_port = redundant ? 2 : 1;
  const uint8_t* dip = (redundant || ctx->mcast_only) ? ctx->mcast_ip_addr[MTL_PORT_P]
                                                      : ctx->para.sip_addr[MTL_PORT_R];
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  if (redundant) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = udp_port;
  }
  ops.payload_type = pt;
  ops.ssrc = ssrc;
}

inline void fill_tx_port(struct st_tx_port& p, struct st_tests_context* ctx,
                         uint16_t udp_port, uint8_t pt, uint32_t ssrc = 0,
                         bool redundant = false) {
  fill_raw_tx_port(p, ctx, udp_port, pt, ssrc, redundant);
}

template <typename RxOps>
inline void fill_raw_rx_port(RxOps& ops, const struct st_tests_context* ctx,
                             uint16_t udp_port, uint8_t pt, uint32_t ssrc = 0,
                             bool redundant = false) {
  ops.num_port = redundant ? 2 : 1;
  if (redundant) {
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_2]);
    ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(ops.ip_addr[MTL_SESSION_PORT_R], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_3]);
    ops.udp_port[MTL_SESSION_PORT_R] = udp_port;
  } else {
    const uint8_t* src_ip =
        ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P];
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], src_ip, MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  }
  ops.payload_type = pt;
  ops.ssrc = ssrc;
}

inline void fill_rx_port(struct st_rx_port& p, struct st_tests_context* ctx,
                         uint16_t udp_port, uint8_t pt, uint32_t ssrc = 0,
                         bool redundant = false) {
  fill_raw_rx_port(p, ctx, udp_port, pt, ssrc, redundant);
}

inline bool is_kernel_loopback(const struct st_tests_context* ctx) {
  return (strcmp(ctx->para.port[MTL_PORT_P], "kernel:lo") == 0) ||
         (strcmp(ctx->para.port[MTL_PORT_R], "kernel:lo") == 0);
}

/* Shared ST 2022-7 redundant TX/RX session stats balance check. */
template <typename TxStats, typename RxStats>
inline void expect_redundant_port_stats(const TxStats& tx_stats, const RxStats& rx_stats,
                                        bool check_tx_packets = false) {
  const uint64_t tx_frames_p = tx_stats.common.port[MTL_SESSION_PORT_P].frames;
  const uint64_t tx_frames_r = tx_stats.common.port[MTL_SESSION_PORT_R].frames;
  EXPECT_GT(tx_frames_p, static_cast<uint64_t>(0)) << "TX port 0 frames must be > 0";
  EXPECT_GT(tx_frames_r, static_cast<uint64_t>(0)) << "TX port 1 frames must be > 0";
  EXPECT_EQ(tx_frames_p, tx_frames_r)
      << "TX port frames must match (port0=" << tx_frames_p << " port1=" << tx_frames_r
      << ")";

  auto expect_within_20pct = [](uint64_t a, uint64_t b, const char* what) {
    EXPECT_GT(a, static_cast<uint64_t>(0)) << what << " port 0 must be > 0";
    EXPECT_GT(b, static_cast<uint64_t>(0)) << what << " port 1 must be > 0";
    EXPECT_GE(std::min(a, b) * 10, std::max(a, b) * 8)
        << what << " differ by more than 20% (port0=" << a << " port1=" << b << ")";
  };

  if (check_tx_packets) {
    expect_within_20pct(tx_stats.common.port[MTL_SESSION_PORT_P].packets,
                        tx_stats.common.port[MTL_SESSION_PORT_R].packets,
                        "TX port packets");
  }
  expect_within_20pct(rx_stats.common.port[MTL_SESSION_PORT_P].bytes,
                      rx_stats.common.port[MTL_SESSION_PORT_R].bytes, "RX port bytes");
}

}  // namespace mtl_test
