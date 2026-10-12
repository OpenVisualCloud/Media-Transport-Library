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

/* Destroy must end every blocked get, not just one.
 *
 * st20p_rx_free() sets lc_destroying, calls wake_on_destroy() once, then
 * mt_handle_drain() waits until every in-flight get_frame has returned.
 * wake_on_destroy is rx_st20p_block_wake(). A pthread_cond_signal there would
 * wake one waiter. With two threads blocked in get_frame, the woken one sees
 * the flag and returns; the other sleeps on and only re-checks lc_destroying
 * when its own block_timeout_ns runs out. free() stalls for that whole time:
 * 1 s by default, longer after st20p_rx_set_block_timeout().
 *
 * With a signal, getter 0 returns in about kWakeDelay and getter 1 at kLongTimeoutNs.
 * A pthread_cond_broadcast on destroy ends both waits; every waiter re-checks
 * lc_destroying, so waking them all is safe. The st22p, st30p and st40p RX and
 * all TX pipelines still signal once on destroy.
 */
TEST_P(St20PipelineRxBlocking, DestroyEndsEveryBlockedGet) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  struct st_frame* frames[2] = {};
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    frames[i] = ut20p_get_frame(ctx_);
    elapsed_ns[i] = ns_since(t0);
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);

  std::this_thread::sleep_for(kWakeDelay);
  ut20p_begin_destroy(ctx_);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(frames[i], nullptr) << "getter " << i;
    EXPECT_LT(elapsed_ns[i], kLongTimeoutNs / 2)
        << "destroy did not end the wait of getter " << i;
  }
}

/* A wake_block() posted while no get waits must not end a later, unrelated wait.
 *
 * st20p_rx_wake_block() sets block_wake_pending, and only the slow path of
 * get_frame (no frame found, about to wait) reads and clears it. A get that
 * finds a frame at once never looks at the flag, so a wake posted while the
 * app was busy outlives any number of successful gets. The next get that finds
 * no frame then returns NULL without waiting, possibly frames after the wake,
 * which is the stale token this PR removes from frame-ready notifies, now from
 * wake_block(). An app that treats NULL from a blocking get as "stopped" or as
 * an error (the FFmpeg demuxers return EIO) stops early.
 *
 * This pins the contract "a wake_block is used up by the next get, whatever it
 * returns". The PR's WakePostedBeforeWaitIsNotLost and
 * OneWakeSatisfiesOnlyOneWait hold under it too: in both, the get after the
 * wake finds no frame and waits. Clearing block_wake_pending when the fast
 * path succeeds is enough. Whether to adopt this contract is a design choice:
 * the TX pipelines instead ignore a wake_block() that no get is waiting for
 * (wake_block_cnt).
 */
TEST_P(St20PipelineRxBlocking, WakeBlockIsSpentByAFastPathGet) {
  ut20p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  ut20p_wake_block(ctx_);
  ASSERT_EQ(ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, kTimestamp), 0);
  struct st_frame* frame = ut20p_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut20p_put_frame(ctx_, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "a wake_block spent by a fast-path get ended a later wait";
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
