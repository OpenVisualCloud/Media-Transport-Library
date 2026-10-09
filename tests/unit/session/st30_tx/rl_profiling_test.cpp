/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tx_audio_session_profiling_rl_bps() in st_tx_audio_session.c: the rate
 * training at session create fills the RL queue with a whole ring of pads, and the
 * session then paces each chunk as if that queue were idle. Training traffic still
 * queued at return makes the first chunks on that queue leave late, behind it, and
 * the other RL queue overtakes them.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxRlProfilingTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30_tx_harness.h"

namespace {
/* ptime 0.33 ms, the shortest the acceptance suite runs on RL */
constexpr long double kTrsNs = 1000000.0L / 3;
constexpr uint64_t kInitialBytesPerSec = 1000000;
}  // namespace

class St30TxRlProfilingTest : public ::testing::TestWithParam<uint16_t> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(0, ut_txa30_setup_rl(ctx_, kTrsNs, GetParam()));
  }

  void TearDown() override {
    ut_txa30_teardown_rl(ctx_);
    ut_txa30_destroy(ctx_);
  }

  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_P(St30TxRlProfilingTest, TrainingTrafficDrainsBeforeTheSessionStarts) {
  /* the modeled queue drains at the initial rate, so training keeps it */
  ASSERT_NEAR(kInitialBytesPerSec, ut_txa30_profile_rl(ctx_, kInitialBytesPerSec),
              kInitialBytesPerSec / 1000);

  EXPECT_EQ(0u, ut_txa30_rl_queued_ns(ctx_));
  /* no longer than the ring needs: nb_tx_desc pkts and pads of trs / 4 each */
  EXPECT_LE(ut_txa30_slept_ns(ctx_), GetParam() * kTrsNs / 4 + kTrsNs);
}

INSTANTIATE_TEST_SUITE_P(NbTxDesc, St30TxRlProfilingTest, ::testing::Values(512, 2048));
