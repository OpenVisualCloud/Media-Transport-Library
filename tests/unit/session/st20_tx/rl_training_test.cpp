/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_train_pacing() in st_tx_video_session.c: the RL training at session create
 * fills the queue with a whole ring of pads and keeps it full, and the session then
 * paces packet 0 of the first frames as if that queue were idle. Training traffic still
 * queued at return makes them leave late, behind it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlTrainingTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
/* shaper time of one pad: the 1080p59.94 GPM_SL trs, ~3707 ns, a pad interval of ~440
 * short */
constexpr long double kPadNs = 3699;
}  // namespace

class St20TxRlTrainingTest : public ::testing::TestWithParam<uint16_t> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxRlTrainingTest, TrainingTrafficDrainsBeforeTheSessionStarts) {
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, GetParam(), kPadNs, false));
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32); /* the training measured the queue */

  EXPECT_EQ(0u, ut_txv_train_queued_ns(ctx_));
  /* no longer than the ring needs */
  EXPECT_LE(ut_txv_slept_ns(ctx_), GetParam() * kPadNs + kPadNs);
}

/* the pre-arm queues NORMAL pads only, but BPM trains on a frame's pads, a quarter of
 * them EXTRA and 6 bytes longer: the plan needs the NORMAL pad's drain, not the mean */
TEST_P(St20TxRlTrainingTest, PreArmDrainIsTheNormalPads) {
  /* the 1080p59.94 BPM trs, ~3892 ns, a pad interval of ~500 short */
  constexpr long double kBpmPadNs = 3884;
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, GetParam(), kBpmPadNs, true));
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32);

  /* 0.1 ns a pad, 13 ns over the 128 pads of a warm-up */
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), kBpmPadNs, 0.1L);
}

INSTANTIATE_TEST_SUITE_P(NbTxDesc, St20TxRlTrainingTest, ::testing::Values(512, 2048));
