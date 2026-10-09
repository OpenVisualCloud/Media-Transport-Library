/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_pkts_capable_chain(): zero copy keeps the app a free frame buffer only while
 * the pkts still holding a frame cannot span every other buffer. Those are the pkts the
 * builder ring holds unsent, ST_TX_VIDEO_SESSIONS_RING_SIZE, and the sent ones the TX
 * queue holds until it frees them: a zero-copy pkt chains a header and a payload
 * segment, 2 descriptors, so nb_tx_desc / 2.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxZeroCopyTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

constexpr uint16_t kNbTxDesc = 2048;
constexpr int kBuilderRing = 512; /* ST_TX_VIDEO_SESSIONS_RING_SIZE */
constexpr int kHeld = kNbTxDesc / 2 + kBuilderRing;

class St20TxZeroCopyTest : public ::testing::Test {
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

TEST_F(St20TxZeroCopyTest, OneSpareBufferCoveringTheHeldPktsKeepsZeroCopy) {
  /* 720p 4:2:2 10-bit BPM, 2 buffers: 1829 pkts, more than the 1536 held */
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 1829, 2));
  EXPECT_TRUE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kHeld, 2));
}

TEST_F(St20TxZeroCopyTest, SpareBuffersShortOfTheHeldPktsCopy) {
  /* 576i fields of 432 pkts: 3 buffers hold 864, short of 1536 */
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, 432, 3));
  EXPECT_FALSE(ut_txv_pkts_capable_chain(ctx_, kNbTxDesc, kHeld - 1, 2));
}

}  // namespace
