/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * video_trs_burst() takes packet 0's mbuf priv as the frame to send user meta for, but
 * only tv_tasklet_frame() sets that priv. tv_tasklet_rtp() and tv_tasklet_st22() leave
 * whatever the mbuf held before, and with MTL_FLAG_TX_MONO_POOL that is a frame of
 * another session, freed after its test: KahawaiTest --tx_mono_pool crashes in
 * st20_frame_tx_start() in St20_rx.digest_*rtp*, on main too.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxStaleFramePrivTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

class St20TxStaleFramePrivTest : public ::testing::Test {
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

TEST_F(St20TxStaleFramePrivTest, RtpPacketZeroSendsNoUserMetaOfAnotherSession) {
  GTEST_SKIP() << "known bug, not fixed yet: an RTP-level or ST22 packet 0 sends the "
                  "user meta of the frame its mbuf priv last pointed at";
  EXPECT_EQ(0u, ut_txv_run_rtp_pkt0_after_frame_session(ctx_));
}
