/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) RX pipeline blocking get_frame() tests, run for both wait
 * sites: internal converter (READY->IN_USER) and derive (CONVERTED->IN_USER).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "pipeline/st20p_harness.h"

namespace {

constexpr int kFrameCnt = 3;
constexpr uint32_t kTimestamp = 1000;
constexpr uint64_t kShortTimeoutNs = 300ULL * 1000 * 1000;     /* 300 ms */
constexpr uint64_t kLongTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */
constexpr auto kWakeDelay = std::chrono::milliseconds(80);
constexpr auto kNotifyPeriod = std::chrono::milliseconds(20);

uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

class St20PipelineRxBlocking : public ::testing::TestWithParam<bool> {
 protected:
  ut20p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
    ctx_ = ut20p_ctx_create(kFrameCnt);
    ASSERT_NE(ctx_, nullptr);
    if (GetParam()) ut20p_ctx_set_internal_converter(ctx_);
  }

  void TearDown() override {
    ut20p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* A fast-path get never reaches the wait, so a token left by the frame's notify
 * would make the next empty get return NULL without waiting. */
TEST_P(St20PipelineRxBlocking, StaleFrameWakeDoesNotSkipTheWait) {
  ut20p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  ASSERT_EQ(ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, kTimestamp), 0);
  struct st_frame* frame = ut20p_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut20p_put_frame(ctx_, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "the empty get returned without waiting for block_timeout_ns";
}

TEST_P(St20PipelineRxBlocking, FrameReadyWakesABlockedGet) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  int inject_ret = -1;
  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    inject_ret = ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, kTimestamp);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);
  producer.join();

  EXPECT_EQ(inject_ret, 0);
  ASSERT_NE(frame, nullptr) << "the frame that arrived during the wait was not returned";
  EXPECT_EQ(frame->timestamp, kTimestamp);
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2)
      << "the frame-ready notify did not wake the get";
  EXPECT_EQ(ut20p_put_frame(ctx_, frame), 0);
}

TEST_P(St20PipelineRxBlocking, WakeBlockEndsTheWait) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  std::thread waker([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    ut20p_wake_block(ctx_);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);
  waker.join();

  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2)
      << "st20p_rx_wake_block() did not end the wait";
}

TEST_P(St20PipelineRxBlocking, WakePostedBeforeWaitIsNotLost) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  ut20p_wake_block(ctx_);

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2) << "the wake posted before the get was lost";
}

TEST_P(St20PipelineRxBlocking, OneWakeSatisfiesOnlyOneWait) {
  ut20p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  ut20p_wake_block(ctx_);
  EXPECT_EQ(ut20p_get_frame(ctx_), nullptr);

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  /* the deadline is read from CLOCK_MONOTONIC after t0, so the full timeout elapses */
  EXPECT_GE(elapsed_ns, kShortTimeoutNs) << "the first get did not spend the wake";
}

TEST_P(St20PipelineRxBlocking, RepeatedNotifiesDoNotMoveTheDeadline) {
  ut20p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  std::atomic<bool> stop{false};
  /* bounded: a get that re-arms per notify only returns once notifies stop */
  const auto notify_until =
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(3 * kShortTimeoutNs);
  std::thread notifier([&]() {
    while (!stop && std::chrono::steady_clock::now() < notify_until) {
      std::this_thread::sleep_for(kNotifyPeriod);
      ut20p_notify_frame_available(ctx_);
    }
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);
  stop = true;
  notifier.join();

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "a notify without a frame ended the wait early";
  EXPECT_LT(elapsed_ns, kShortTimeoutNs * 3 / 2)
      << "notifies without a frame extended the wait past block_timeout_ns";
}

TEST_P(St20PipelineRxBlocking, DestroyEndsTheWaitWithoutAFrame) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  int inject_ret = -1;
  std::thread destroyer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    ut20p_force_destroying(ctx_);
    inject_ret = ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, kTimestamp);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);
  destroyer.join();

  EXPECT_EQ(inject_ret, 0);
  EXPECT_EQ(frame, nullptr) << "a destroying session handed out a frame";
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2) << "destroy did not end the wait";
}

/* A timed-out pthread_cond_timedwait() may consume a concurrent signal (POSIX). */
TEST_P(St20PipelineRxBlocking, UnsignalledFrameIsClaimedAtTimeout) {
  ut20p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    if (GetParam())
      ut20p_set_frame_ready(ctx_, 0);
    else
      ut20p_set_frame_converted(ctx_, 0);
  });

  struct st_frame* frame = ut20p_get_frame(ctx_);
  producer.join();

  ASSERT_NE(frame, nullptr) << "the timed-out wait did not look for a frame again";
  EXPECT_EQ(ut20p_put_frame(ctx_, frame), 0);
}

INSTANTIATE_TEST_SUITE_P(WaitSites, St20PipelineRxBlocking, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) -> std::string {
                           return info.param ? "InternalConverter" : "Derive";
                         });
