/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "test_fixture.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include "strategies/st20p_strategies.hpp"

namespace {

struct TestPtpClockState {
  std::atomic<uint64_t> start_ns{0};
  std::atomic<bool> running{false};
};

TestPtpClockState g_test_ptp_clock;

} /* namespace */

void NoCtxTest::SetUp() {
  const auto* base = st_test_ctx();
  if (!base) {
    throw std::runtime_error("NoCtxTest::SetUp: base test context is not initialized");
  }
  if (base->handle) {
    throw std::runtime_error(
        "NoCtxTest::SetUp: shared MTL handle is already active; NoCtxTest cases must "
        "run in their own process (remove --single_process or let KahawaiTest dispatch "
        "automatically).");
  }

  ctx = new st_tests_context;
  memcpy(ctx, base, sizeof(*ctx));
  ctx->handle = nullptr;

  ctx->para.flags |= MTL_FLAG_RANDOM_SRC_PORT;
  if (!ctx->user_log_level) ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.priv = ctx;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->para.tx_queues_cnt[i] = 16;
    ctx->para.rx_queues_cnt[i] = 16;
  }
  ResetFakePtpClock();
  defaultTestDuration = 20;
  if (const char* env_dur = std::getenv("NOCTX_TEST_DURATION"); env_dur && *env_dur) {
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(env_dur, &end, 10);
    if (errno == 0 && end != env_dur && *end == '\0' && parsed >= 1 && parsed <= 3600) {
      defaultTestDuration = static_cast<uint>(parsed);
    }
  }
}

void NoCtxTest::TearDown() {
  for (auto& h : handlers_) {
    if (h) h->stopSession();
  }

  if (ctx && ctx->handle) {
    mtl_stop(ctx->handle);
  }

  handlers_.clear();
  frameTestStrategies.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      ctx->handle = nullptr;
    }

    delete ctx;
    ctx = nullptr;
  }
}

uint64_t NoCtxTest::FakePtpClockNow(void* priv) {
  (void)priv;
  if (!g_test_ptp_clock.running.load(std::memory_order_acquire)) {
    StartFakePtpClock();
  }

  const uint64_t start = g_test_ptp_clock.start_ns.load(std::memory_order_acquire);
  const uint64_t now = monotonicRawNs();
  if (now <= start) {
    return 0;
  }

  return now - start;
}

void NoCtxTest::StartFakePtpClock() {
  g_test_ptp_clock.start_ns.store(monotonicRawNs(), std::memory_order_release);
  g_test_ptp_clock.running.store(true, std::memory_order_release);
}

void NoCtxTest::ResetFakePtpClock() {
  g_test_ptp_clock.running.store(false, std::memory_order_release);
  g_test_ptp_clock.start_ns.store(0, std::memory_order_release);
}

void NoCtxTest::sleepUntilFailure(int sleep_duration) {
  if (!sleep_duration) {
    sleep_duration = defaultTestDuration;
  }

  for (int i = 0; i < sleep_duration * 10; ++i) {
    if (HasFailure()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void NoCtxTest::initDefaultContext(PtpTimeFn ptpTimeFn) {
  if (!ctx) {
    throw std::runtime_error("initDefaultContext expects initialized ctx");
  }

  ctx->para.ptp_get_time_fn = ptpTimeFn;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);
}

void NoCtxTest::requireStrictTopology(const std::string& unsupported) {
  if (unsupported.empty()) return;
  if (strictPacingRequired()) FAIL() << unsupported;
  GTEST_SKIP() << unsupported;
}

void NoCtxTest::initStrictPacingContext(PtpTimeFn ptpTimeFn) {
  requireStrictTopology(
      strictPacingTopologyError(ctx->para.port[MTL_PORT_P], ctx->para.port[MTL_PORT_R]));
  if (IsSkipped() || HasFatalFailure()) return;

  /* NIC RX timestamps are the pacing oracle. The TX tasklet launches each frame in
   * software (audio, and RL video's WAIT_TARGET gate), so keep the CNI and RX video
   * tasklets off its scheduler. */
  ctx->para.flags &= ~MTL_FLAG_CNI_TASKLET;
  ctx->para.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  ctx->para.flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;
  /* isolate.sh gives CPUs on the TX ports' nodes only; RX is timed by its NIC. */
  ctx->para.flags |= MTL_FLAG_ALLOW_ACROSS_NUMA_CORE;
  initDefaultContext(ptpTimeFn);
}
