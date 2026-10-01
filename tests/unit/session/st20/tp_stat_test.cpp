/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * 10 s statistics of the ST 2110-21 RX timing parser: the MAX logged for a
 * period is the largest per-frame MAX in it, whatever the per-frame MINs are.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTpStatTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxTpStatTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/false), 0);
    ut20_ctx_enable_timing_parser_stat(ctx_);
  }
};

TEST_F(St20RxTpStatTest, VrxMaxKeepsLargestFrameMax) {
  ut20_tp_stat_add_frame(ctx_, MTL_SESSION_PORT_P, 2, 7, 100, 200);
  ut20_tp_stat_add_frame(ctx_, MTL_SESSION_PORT_P, 1, 3, 100, 200);

  EXPECT_EQ(ut20_tp_stat_vrx_max(ctx_, MTL_SESSION_PORT_P), 7);
}

TEST_F(St20RxTpStatTest, IptMaxKeepsLargestFrameMax) {
  ut20_tp_stat_add_frame(ctx_, MTL_SESSION_PORT_P, 2, 3, 100, 900);
  ut20_tp_stat_add_frame(ctx_, MTL_SESSION_PORT_P, 2, 3, 50, 300);

  EXPECT_EQ(ut20_tp_stat_ipt_max(ctx_, MTL_SESSION_PORT_P), 900);
}
