/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Review tests for PR 1796: st20p RX wake_block() against concurrent getters.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "pipeline/st20p_harness.h"

namespace {

constexpr int kFrameCnt = 3;
constexpr uint64_t kLongTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */
constexpr auto kWaitDelay = std::chrono::milliseconds(80);

uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

class St20PipelineRxWakeReview : public ::testing::TestWithParam<bool> {
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

  void make_frame_available_without_notify(int idx) {
    if (GetParam())
      ut20p_set_frame_ready(ctx_, idx);
    else
      ut20p_set_frame_converted(ctx_, idx);
  }
};

/* Getter B blocks in st20p_rx_get_frame(). The app calls st20p_rx_wake_block()
 * to end that wait. A second consumer thread A then takes a frame on the fast
 * path. The fast-path "spend the token" store clears block_wake_pending outside
 * block_wake_mutex before B re-acquires the mutex, so B sees no wake and sleeps
 * on until block_timeout_ns. On upstream/main B returns at once.
 *
 * To force that order, the test holds block_wake_mutex while it posts the wake
 * and while A runs its fast-path get, so B can read the flag only after A. */
TEST_P(St20PipelineRxWakeReview, FastPathGetDoesNotStealTheWakeOfABlockedGet) {
  ut20p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  struct st_frame* b_frame = nullptr;
  uint64_t b_elapsed_ns = 0;
  std::thread b([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    b_frame = ut20p_get_frame(ctx_);
    b_elapsed_ns = ns_since(t0);
  });

  std::this_thread::sleep_for(kWaitDelay); /* B is in the timed wait */

  /* A frame lands with no notify (as in UnsignalledFrameIsClaimedAtTimeout),
   * then the app wakes the blocked get, then consumer A runs. */
  make_frame_available_without_notify(0);
  ut20p_lock_block_mutex(ctx_);
  ut20p_post_wake_locked(ctx_); /* B is woken, but needs the mutex */
  struct st_frame* a_frame = nullptr;
  std::atomic<bool> a_done{false};
  std::thread a([&]() {
    a_frame = ut20p_get_frame(ctx_); /* fast path: no block_wake_mutex */
    a_done.store(true);
  });
  const auto a_t0 = std::chrono::steady_clock::now();
  while (!a_done.load() &&
         std::chrono::steady_clock::now() - a_t0 < std::chrono::seconds(1))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ut20p_unlock_block_mutex(ctx_);
  a.join();
  b.join();

  ASSERT_TRUE(a_done.load()) << "the fast-path get of A waited for block_wake_mutex";

  if (a_frame) {
    EXPECT_EQ(ut20p_put_frame(ctx_, a_frame), 0);
  }
  if (b_frame) {
    EXPECT_EQ(ut20p_put_frame(ctx_, b_frame), 0);
  }
  EXPECT_LT(b_elapsed_ns, kLongTimeoutNs / 2)
      << "st20p_rx_wake_block() did not end the blocked get: a concurrent fast-path "
         "get spent the wake (B blocked "
      << b_elapsed_ns / 1000000 << " ms)";
}

INSTANTIATE_TEST_SUITE_P(WaitSites, St20PipelineRxWakeReview, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) -> std::string {
                           return info.param ? "InternalConverter" : "Derive";
                         });

/* Single consumer. The control thread calls st20p_rx_wake_block() while the
 * consumer's get is converting a frame it already claimed (the hook posts the
 * wake inside convert_func, which is exactly that moment). That get returns the
 * frame. The wake was posted after the frame was claimed, so by the PR's own
 * contract ("a wake_block is used up by the next get") the NEXT get must end
 * without waiting. The PR clears block_wake_pending after convert_func, so the
 * in-flight get spends the wake and the next empty get blocks for the full
 * block_timeout_ns. On upstream/main the next get returns at once. */
class St20PipelineRxWakeReviewConvert : public ::testing::Test {
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

TEST_F(St20PipelineRxWakeReviewConvert, WakePostedDuringAConversionEndsTheNextGet) {
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
