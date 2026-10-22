/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * One bulk per tasklet call for the builder, and one or two for the RL transmitter, limit
 * a tx video session to that many packets per scheduler loop. A session that is late
 * does more bulks in one call, up to ST_TX_VIDEO_LATE_BULKS_MAX: the builder while its
 * next packet is due, the transmitter while the last packet it sent was due. A session
 * that is on time keeps the old count, so a scheduler with spare time gets no longer
 * tasklet calls.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxLateBulks*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

namespace {
constexpr uint64_t kNow = 1000000;
constexpr uint64_t kPktNs = 100;
}  // namespace

class St20TxLateBulksTrsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    /* a NIC queue that takes all the pkts of the ring */
    ASSERT_EQ(0, ut_trs_setup_stream(ctx_, kPktNs, 2048, 1));
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  /* one RL tasklet call, the pkts it gave to the NIC */
  uint64_t SentInOneCall(uint64_t first_tsc) {
    EXPECT_EQ(0, ut_trs_fill_stream(ctx_, 64, first_tsc, kPktNs, kNow));
    ut_trs_call_rl_tasklet(ctx_);
    return ut_trs_stream_nic_pkts(ctx_);
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLateBulksTrsTest, LateSessionSendsAllBulksOfOneCall) {
  EXPECT_EQ((uint64_t)ut_trs_bulks_per_call() * ut_trs_max_bulk(), SentInOneCall(1000));
}

TEST_F(St20TxLateBulksTrsTest, OnTimeSessionSendsTwoBulks) {
  EXPECT_EQ(2u * ut_trs_max_bulk(), SentInOneCall(2 * kNow));
}

TEST_F(St20TxLateBulksTrsTest, StopsAfterTheFirstBulkThatIsNotLate) {
  /* the last pkt of bulk 2 is due 300 ns before now, of bulk 3 100 ns after */
  EXPECT_EQ(3u * ut_trs_max_bulk(), SentInOneCall(kNow - 1000));
}

/* A retry of an inflight pkt is not a sent bulk, so it never counts as late: the NIC
 * takes 3 of the 4 late pkts of the first bulk, the retry sends the 4th, and the call
 * ends. */
TEST_F(St20TxLateBulksTrsTest, InflightRetryIsNeverLate) {
  ut_trs_set_burst_accept_limit(ctx_, 3);
  EXPECT_EQ((uint64_t)ut_trs_max_bulk(), SentInOneCall(1000));
}

class St20TxLateBulksBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }

  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLateBulksBuilderTest, LateSessionBuildsAllBulksOfOneCall) {
  EXPECT_EQ(ut_txv_bulks_per_call(), ut_txv_run_mid_frame(ctx_, kNow - 100000, kNow));
}

TEST_F(St20TxLateBulksBuilderTest, OnTimeSessionBuildsOneBulk) {
  EXPECT_EQ(1, ut_txv_run_mid_frame(ctx_, kNow + 1000000, kNow));
}

TEST_F(St20TxLateBulksBuilderTest, StopsWhenTheNextPacketIsNotDue) {
  /* 1 us per packet: due 2.5 us ago, after 3 packets the next one is due 0.5 us later */
  EXPECT_EQ(3, ut_txv_run_mid_frame(ctx_, kNow - 2500, kNow));
}

/* A late session at the last 2 pkts of a frame: the loop ends the frame and starts the
 * next one, whose first pkt is due in 100 us, and then stops. */
TEST_F(St20TxLateBulksBuilderTest, StopsAtANextFrameThatIsNotDue) {
  uint64_t first_tsc = 0;
  int gets = 0;

  EXPECT_EQ(3, ut_txv_run_frame_end(ctx_, kNow - 100000, kNow, 4900000, 4, false,
                                    &first_tsc, &gets));
  EXPECT_EQ(1, gets);
  EXPECT_GT(first_tsc, kNow) << "the first pkt of the next frame is not due yet";
}

/* The app has no next frame: the loop ends the frame and stops. */
TEST_F(St20TxLateBulksBuilderTest, StopsWhenTheAppHasNoNextFrame) {
  uint64_t first_tsc = 0;
  int gets = 0;

  EXPECT_EQ(2, ut_txv_run_frame_end(ctx_, kNow - 100000, kNow, 4900000, 4, true,
                                    &first_tsc, &gets));
  EXPECT_EQ(1, gets);
}
