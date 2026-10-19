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
 * lc_destroying, so waking them all is safe.
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

/* Getter B blocks in st20p_rx_get_frame(). The app calls st20p_rx_wake_block()
 * to end that wait. A second consumer thread A then takes a frame on the fast
 * path. The fast-path "spend the token" store clears block_wake_pending outside
 * block_wake_mutex before B re-acquires the mutex, so B sees no wake and sleeps
 * on until block_timeout_ns. Without the fault B returns at once.
 *
 * To force that order, the test holds block_wake_mutex while it posts the wake
 * and while A runs its fast-path get, so B can read the flag only after A. */
TEST_P(St20PipelineRxBlocking, FastPathGetDoesNotStealTheWakeOfABlockedGet) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  struct st_frame* b_frame = nullptr;
  uint64_t b_elapsed_ns = 0;
  std::thread b([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    b_frame = ut20p_get_frame(ctx_);
    b_elapsed_ns = ns_since(t0);
  });

  std::this_thread::sleep_for(kWakeDelay); /* B is in the timed wait */

  /* A frame lands with no notify (as in UnsignalledFrameIsClaimedAtTimeout),
   * then the app wakes the blocked get, then consumer A runs. */
  if (GetParam())
    ut20p_set_frame_ready(ctx_, 0);
  else
    ut20p_set_frame_converted(ctx_, 0);
  ut20p_lock_block_mutex(ctx_);
  ut20p_post_wake_locked(ctx_); /* B is woken, but needs the mutex */
  struct st_frame* a_frame = nullptr;
  std::atomic<bool> a_done{false};
  std::thread a([&]() {
    a_frame = ut20p_get_frame(ctx_); /* fast path: no block_wake_mutex */
    a_done.store(true);
  });
  /* B's budget kLongTimeoutNs / 2 = 1 s > kWakeDelay 80 ms + this 300 ms spin bound */
  const auto a_t0 = std::chrono::steady_clock::now();
  while (!a_done.load() &&
         std::chrono::steady_clock::now() - a_t0 < std::chrono::milliseconds(300))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool a_done_locked = a_done.load();
  ut20p_unlock_block_mutex(ctx_);
  a.join();
  b.join();

  if (b_frame) { /* B took the frame itself */
    EXPECT_EQ(ut20p_put_frame(ctx_, b_frame), 0);
    GTEST_SKIP() << "B took the frame itself; the order the test needs did not form";
  }
  /* A skip on every unloaded run means the fast path now takes block_wake_mutex. */
  if (!a_done_locked) { /* A was not scheduled within the bound */
    if (a_frame) {
      EXPECT_EQ(ut20p_put_frame(ctx_, a_frame), 0);
    }
    GTEST_SKIP() << "the fast-path get of A did not finish within its bound; the race "
                    "did not form";
  }
  ASSERT_NE(a_frame, nullptr);
  EXPECT_EQ(ut20p_put_frame(ctx_, a_frame), 0);
  EXPECT_LT(b_elapsed_ns, kLongTimeoutNs / 2)
      << "st20p_rx_wake_block() did not end the blocked get: a concurrent fast-path "
         "get spent the wake (B blocked "
      << b_elapsed_ns / 1000000 << " ms)";
}

INSTANTIATE_TEST_SUITE_P(WaitSites, St20PipelineRxBlocking, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) -> std::string {
                           return info.param ? "InternalConverter" : "Derive";
                         });

/* Single consumer. The control thread calls st20p_rx_wake_block() while the
 * consumer's get is converting a frame it already claimed (the hook posts the
 * wake inside convert_func, which is exactly that moment). That get returns the
 * frame. The wake was posted after the frame was claimed, and a wake_block()
 * ends the next get that waits, so the NEXT get must end without
 * waiting. A get that clears block_wake_pending after convert_func
 * spends the wake, and the next empty get blocks for the full
 * block_timeout_ns. Without the fault the next get returns at once. */
class St20PipelineRxBlockingConvert : public ::testing::Test {
 protected:
  ut20p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
    ctx_ = ut20p_ctx_create(kFrameCnt);
    ASSERT_NE(ctx_, nullptr);
    ut20p_ctx_set_internal_converter(ctx_);
  }

  void TearDown() override {
    ut20p_set_convert_hook(nullptr, nullptr);
    ut20p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

TEST_F(St20PipelineRxBlockingConvert, WakePostedDuringAConversionEndsTheNextGet) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  ut20p_set_frame_ready(ctx_, 0);
  ut20p_set_convert_hook(
      [](void* arg) { ut20p_wake_block(static_cast<ut20p_ctx*>(arg)); }, ctx_);
  struct st_frame* frame = ut20p_get_frame(ctx_);
  ut20p_set_convert_hook(nullptr, nullptr);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut20p_put_frame(ctx_, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut20p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2)
      << "the wake_block() posted during the previous get's conversion was spent by "
         "that get; the next get blocked "
      << elapsed_ns / 1000000 << " ms";
}
