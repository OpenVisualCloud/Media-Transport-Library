/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_mempool_init() against mt_rtcp_tx_create(): the RTCP retransmit ring keeps
 * a reference to every pkt it buffers, so the session pools must cover the ring it
 * really gets, never smaller than the TX queue, also when the app leaves the size to
 * the library.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRtcpPoolTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

constexpr unsigned int kRingCount = 512; /* ST_TX_VIDEO_SESSIONS_RING_SIZE */
constexpr unsigned int kDefaultRtcpBuffer = 1024;

class St20TxRtcpPoolTest : public ::testing::TestWithParam<uint16_t> {
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

TEST_P(St20TxRtcpPoolTest, PoolsCoverTheDefaultRetransmitRing) {
  const uint16_t nb_tx_desc = GetParam();
  const unsigned int ring = std::max<unsigned int>(kDefaultRtcpBuffer, nb_tx_desc);
  unsigned int hdr = 0, chain = 0;

  ASSERT_EQ(0, ut_txv_rtcp_mempool_sizes(ctx_, nb_tx_desc, 0, &hdr, &chain));

  EXPECT_GE(hdr, nb_tx_desc + kRingCount + ring);
  EXPECT_GE(chain, nb_tx_desc + kRingCount + ring);
}

TEST_P(St20TxRtcpPoolTest, PoolsCoverASmallUserRingRaisedToTheQueue) {
  const uint16_t nb_tx_desc = GetParam();
  unsigned int hdr = 0, chain = 0;

  ASSERT_EQ(0, ut_txv_rtcp_mempool_sizes(ctx_, nb_tx_desc, 256, &hdr, &chain));

  EXPECT_GE(hdr, nb_tx_desc + kRingCount + nb_tx_desc);
  EXPECT_GE(chain, nb_tx_desc + kRingCount + nb_tx_desc);
}

INSTANTIATE_TEST_SUITE_P(NbTxDesc, St20TxRtcpPoolTest, ::testing::Values(512, 2048));

}  // namespace
