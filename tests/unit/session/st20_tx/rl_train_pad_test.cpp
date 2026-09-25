/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins that rate-limiter pacing trains on the bytes a real frame puts on the
 * wire. tv_train_pacing() times one frame's worth of pads and derives the pad
 * interval from that time, so pads heavier than the frame yield an interval that
 * is too large: frames then finish early and VRX climbs. GPM_SL ends every line
 * with a shorter LINE_TAIL packet unless the line splits evenly into packets.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlTrainPadTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

class St20TxRlTrainPadTest : public ::testing::Test {
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

TEST_F(St20TxRlTrainPadTest, GpmSl720pPadsMatchFrameBytes) {
  ASSERT_EQ(ut_txv_init_gpm_sl_pkt(ctx_, 1280, 720), 0);
  ASSERT_EQ(ut_txv_line_tail_pkts(ctx_), 720u);

  EXPECT_EQ(ut_txv_train_pad_bytes(ctx_), ut_txv_frame_bytes(ctx_))
      << "each line's short tail must be trained as a LINE_TAIL pad";
}

TEST_F(St20TxRlTrainPadTest, GpmSl2160pPadsMatchFrameBytes) {
  ASSERT_EQ(ut_txv_init_gpm_sl_pkt(ctx_, 3840, 2160), 0);
  ASSERT_EQ(ut_txv_line_tail_pkts(ctx_), 2160u);

  EXPECT_EQ(ut_txv_train_pad_bytes(ctx_), ut_txv_frame_bytes(ctx_))
      << "each line's short tail must be trained as a LINE_TAIL pad";
}

TEST_F(St20TxRlTrainPadTest, GpmSl1080pWithoutLineTailPadsMatchFrameBytes) {
  ASSERT_EQ(ut_txv_init_gpm_sl_pkt(ctx_, 1920, 1080), 0);
  ASSERT_EQ(ut_txv_line_tail_pkts(ctx_), 0u);

  EXPECT_EQ(ut_txv_train_pad_bytes(ctx_), ut_txv_frame_bytes(ctx_))
      << "1080p lines split evenly, so no LINE_TAIL pad exists to be trained";
}

}  // namespace
