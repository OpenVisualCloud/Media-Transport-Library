/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the epoch/onward-resync math in
 * `calc_frame_count_since_epoch()` (st_tx_video_session.c): steady-state
 * advance, the max_onward_epochs boundary, the PTP-step resync fix, the
 * late-frame drop path, and required_tai validation.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxEpochTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

class St20TxEpochTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, 1000000.0L);
    ut_txv_set_max_onward_epochs(ctx_, 3);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxEpochTest, SteadyStateAdvancesByOneEpoch) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next_free_frame_slot = 10 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, 0);
  EXPECT_EQ(frame_count, 10u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
}

TEST_F(St20TxEpochTest, OnwardGapAtBoundaryDoesNotResync) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next=10 */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 7000000, 0); /* tai=7, onward=3==max */
  EXPECT_EQ(frame_count, 10u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxEpochTest, OnwardGapBeyondBoundaryResyncs) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next=10 */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 6000000, 0); /* tai=6, onward=4>max */
  EXPECT_EQ(frame_count, 6u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 4u);
}

TEST_F(St20TxEpochTest, LargePtpStepResyncsInOneCall) {
  ut_txv_set_cur_epochs(ctx_, 1000000); /* simulates a stale epoch after a PTP step */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 5000000, 0); /* tai=5 */
  EXPECT_EQ(frame_count, 5u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 999996u); /* next(1000001) - tai(5) */

  /* caller commits the resync (as tv_sync_pacing does) and time advances by
   * one frame -- confirm the gap stays closed, no further resync needed */
  ut_txv_set_cur_epochs(ctx_, frame_count);
  uint64_t frame_count2 = ut_txv_calc_frame_count_since_epoch(ctx_, 6000000, 0);
  EXPECT_EQ(frame_count2, 6u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 999996u); /* unchanged */
}

TEST_F(St20TxEpochTest, LatePathBumpsDropAndNotifiesFrameLate) {
  ut_txv_set_cur_epochs(ctx_, 2); /* next_free_frame_slot=3 */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, 0); /* tai=10 */
  EXPECT_EQ(frame_count, 10u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 7u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_late_last_delta(ctx_), 7u);
}

TEST_F(St20TxEpochTest, RequiredTaiInThePastFlagsStat) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next=10, onward=0 (normal path) */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, 5000000);
  EXPECT_EQ(frame_count, 5u); /* required_tai-derived value, not overridden */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St20TxEpochTest, RequiredTaiTooFarFutureFlagsStat) {
  ut_txv_set_cur_epochs(ctx_, 9); /* current frame count = 10, limit = +1000 */
  uint64_t required_tai = 1011ull * 1000000;
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, required_tai);
  EXPECT_EQ(frame_count, 1011u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St20TxEpochTest, RequiredTaiPreservedThroughOnwardResync) {
  ut_txv_set_cur_epochs(ctx_, 1000000); /* forces the onward-resync branch */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, 5000000, 2000000);
  EXPECT_EQ(frame_count, 2u); /* required_tai-derived value survives the resync */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 999996u);
}

TEST_F(St20TxEpochTest, RequiredTaiExactlyAtInThePastBoundaryDoesNotFlag) {
  ut_txv_set_cur_epochs(ctx_, 9); /* current frame count = 10 */
  ut_txv_calc_frame_count_since_epoch(ctx_, 10000000,
                                      10000000); /* required frame count == 10 */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, RequiredTaiExactlyAtFutureBoundaryDoesNotFlag) {
  ut_txv_set_cur_epochs(ctx_, 9); /* current frame count = 10, limit = +1000 */
  ut_txv_calc_frame_count_since_epoch(ctx_, 10000000,
                                      1010000000ull); /* required frame count == 1010 */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, CurEpochsWraparoundIsWellDefinedNotUB) {
  ut_txv_set_cur_epochs(ctx_, UINT64_MAX); /* next_free_frame_slot wraps to 0 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, 5000000, 0);
  EXPECT_EQ(frame_count, 5u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 5u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxEpochTest, CurTaiNearUint64MaxNoCrash) {
  ut_txv_set_cur_epochs(ctx_, 0); /* next_free_frame_slot=1 */
  uint64_t cur_tai = UINT64_MAX;
  uint64_t expected_frame_count_tai = cur_tai / 1000000ull;
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, cur_tai, 0);
  EXPECT_EQ(frame_count, expected_frame_count_tai);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), expected_frame_count_tai - 1);
}

/* Reproduces the required_tai asymmetry flagged in review: unlike the onward
 * branch (which now preserves a validated required_tai through resync, see
 * RequiredTaiPreservedThroughOnwardResync above), the late (else) branch has
 * no `if (!required_tai)` guard -- it unconditionally overwrites frame_count
 * with frame_count_tai, silently discarding a validated required_tai. This
 * asserts the CORRECT/expected behavior (symmetric with the onward branch)
 * and currently FAILS against production code, confirming the defect is
 * real rather than hypothetical. */
TEST_F(St20TxEpochTest, LatePathShouldPreserveRequiredTaiLikeOnwardBranch) {
  ut_txv_set_cur_epochs(ctx_, 2); /* next_free_frame_slot=3 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, 4000000);
  EXPECT_EQ(frame_count, 4u) << "late branch should preserve the required_tai-derived "
                                "frame_count (4), not overwrite it with frame_count_tai "
                                "(10)";
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 7u);
}

/* Reproduces a second, independent gap: validate_user_timestamp() flags an
 * out-of-range required_tai (e.g. a caller unit-conversion bug -- passing
 * milliseconds where nanoseconds are expected) via a stat counter, but the
 * wildly-wrong derived frame_count is still returned and used verbatim --
 * there is no fallback to real time once validation fails. Contrast with
 * LargePtpStepResyncsInOneCall, where a bad cur_epochs DOES self-heal via
 * the onward-resync branch: here, an out-of-range *required_tai* has no
 * equivalent self-healing path in the normal (non-onward, non-late) branch.
 * This asserts the safer/expected fallback behavior and currently FAILS. */
TEST_F(St20TxEpochTest, InvalidRequiredTaiIsFlaggedButNotClampedToRealTime) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next_free_frame_slot=10, onward=0 (normal path) */
  uint64_t required_tai = 10;     /* caller bug: ms instead of ns, ~1e6x too small */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, 10000000, required_tai);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u)
      << "validation should flag the out-of-range required_tai";
  EXPECT_EQ(frame_count, 10u) << "an out-of-range required_tai should fall back to "
                                 "real time (frame 10), not be honored verbatim (frame "
                              << frame_count << ")";
}
