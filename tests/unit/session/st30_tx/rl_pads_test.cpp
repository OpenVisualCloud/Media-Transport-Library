/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the rate-limit pacing of the ST30 TX session: every packet is followed by the
 * pads that tx_audio_session_initial_rl_bps() counts in the queue rate, or the queue
 * drains early or late against the packet time.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxRlPadsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30_tx_harness.h"

class St30TxRlPadsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_txa30_destroy(ctx_);
  }

  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_F(St30TxRlPadsTest, EachPacketIsFollowedByThePadsOfItsRateBudget) {
  int budget_pads = 0;

  int pads = ut_txa30_rl_pads_after_pkt(ctx_, &budget_pads);
  ASSERT_GT(budget_pads, 0);
  EXPECT_EQ(pads, budget_pads);
}
