/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The NIC TX queue is the only buffer while the RL transmitter's scheduler thread is
 * off its CPU: the builder and the transmitter stop with it, and the shaper keeps
 * sending what is queued. A zero-copy video pkt takes 2 descriptors (header and
 * payload segments). Once the queue runs dry the wire pauses, the pkts after the
 * pause leave behind their slots, and the receiver's VRX underflows.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxQueueStallTest.*'
 */

#include <gtest/gtest.h>

#include "session/st_video_transmitter_harness.h"

namespace {

/* 1080p59.94 GPM */
constexpr long double kTrs = 16683333.333L * 1080 / 1125 / 4320;
constexpr uint16_t kChainedSegs = 2;
constexpr uint64_t kCallNs = 500;
constexpr uint64_t kSteadyNs = 5000000;
/* an assumed slice a busy CPU gives another runnable task: EEVDF's base slice is
 * 0.75 ms * (1 + log2(min(cpus, 8))), 3 ms from 8 CPUs on, and tunable */
constexpr uint64_t kSchedulerSliceNs = 3000000;

class St20TxQueueStallTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  /* how late the stream leaves after the tasklet loses its CPU for stall_ns */
  long double LatenessAfterStall(uint16_t nic_desc, uint64_t stall_ns) {
    EXPECT_EQ(0, ut_trs_setup_stream(ctx_, kTrs, nic_desc, kChainedSegs));
    ut_trs_stream(ctx_, kSteadyNs, kCallNs);
    /* the transmitter keeps the queue full */
    EXPECT_GT(ut_trs_stream_nic_desc_used(ctx_), nic_desc - kChainedSegs - 1u);
    ut_trs_stall_stream(ctx_, stall_ns);
    ut_trs_stream(ctx_, kSteadyNs, kCallNs);
    return ut_trs_stream_max_late_ns(ctx_);
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxQueueStallTest, StallTheQueuedPktsCoverLeavesNoWireGap) {
  constexpr uint16_t kNicDesc = 512;
  /* 256 chained pkts, ~0.95 ms */
  const uint64_t covered_ns = (uint64_t)(kNicDesc / kChainedSegs * kTrs);

  EXPECT_LT(LatenessAfterStall(kNicDesc, covered_ns - 2 * kTrs), kTrs);
}

TEST_F(St20TxQueueStallTest, StallBeyondTheQueuedPktsDelaysTheStream) {
  constexpr uint16_t kNicDesc = 512;
  const uint64_t covered_ns = (uint64_t)(kNicDesc / kChainedSegs * kTrs);

  EXPECT_GT(LatenessAfterStall(kNicDesc, covered_ns + 100000), 90000);
}

/* the default must ride out one lost scheduler slice at 1080p59.94 */
TEST_F(St20TxQueueStallTest, DefaultTxQueueRidesOutASchedulerSlice) {
  EXPECT_LT(LatenessAfterStall(ut_trs_default_nb_tx_desc(), kSchedulerSliceNs), kTrs)
      << "nb_tx_desc " << ut_trs_default_nb_tx_desc();
}

TEST_F(St20TxQueueStallTest, StreamModeLeavesThePrivPoolWhole) {
  ut_trs_destroy(ctx_);
  const unsigned int avail = ut_trs_priv_pool_avail();
  ctx_ = ut_trs_create();
  ASSERT_NE(nullptr, ctx_);

  LatenessAfterStall(512, 0);
  ut_trs_destroy(ctx_);
  ctx_ = nullptr;

  EXPECT_EQ(avail, ut_trs_priv_pool_avail());
}

}  // namespace
