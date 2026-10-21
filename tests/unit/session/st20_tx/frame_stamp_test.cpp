/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the TSC/PTP stamps tv_tasklet_frame() writes into its pkts and the pacing
 * cursors it leaves behind. RL transmitters read the stamps of a frame's first pkt
 * only; TSC and PTP read the first pkt of each bulk, TSN every pkt.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxFrameStamp*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

constexpr uint64_t kFramePeriodNs = uint64_t{1000} * 1000;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
/* a fraction that a sum of whole-ns steps drifts away from */
constexpr long double kTrs = 10000.0L / 3;
constexpr unsigned int kBulk = 4;
/* two builds, the second ending in two dummies */
constexpr int kTotalPkts = 6;
constexpr int kBuiltPkts = 8;

/* the cursor of pkt idx, forwarded by trs one pkt at a time from the frame start */
long double Cursor(uint64_t frame_start, int idx) {
  long double cursor = frame_start;
  for (int i = 0; i < idx; i++) cursor += kTrs;
  return cursor;
}

class St20TxFrameStampTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFramePeriodNs);
    /* the frame starts on the current epoch, so the cursors start at the mocked clocks */
    ut_txv_set_cur_epochs(ctx_, kCurrentTai / kFramePeriodNs - 1);
    ut_txv_set_trs(ctx_, kTrs);
    ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
    ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  int Run(enum st21_tx_pacing_way way_p, enum st21_tx_pacing_way way_r, int num_port,
          uint64_t build_ns = 0) {
    const enum st21_tx_pacing_way ways[MTL_SESSION_PORT_MAX] = {way_p, way_r};
    return ut_txv_run_frame_stamps(ctx_, ways, num_port, kBulk, kTotalPkts, build_ns,
                                   &stamps_);
  }
  void ExpectOnlyFirstPktStamped(int port) {
    EXPECT_EQ(stamps_.tsc[port][0], kCurrentTsc) << "port " << port;
    EXPECT_EQ(stamps_.ptp[port][0], kCurrentTai) << "port " << port;
    for (int i = 1; i < kBuiltPkts; i++) {
      EXPECT_EQ(stamps_.tsc[port][i], UT_TXV_NO_STAMP) << "port " << port << " pkt " << i;
      EXPECT_EQ(stamps_.ptp[port][i], UT_TXV_NO_STAMP) << "port " << port << " pkt " << i;
    }
  }
  void ExpectEveryPktStamped(int port) {
    for (int i = 0; i < kBuiltPkts; i++) {
      EXPECT_EQ(stamps_.tsc[port][i], (uint64_t)Cursor(kCurrentTsc, i))
          << "port " << port << " pkt " << i;
      EXPECT_EQ(stamps_.ptp[port][i], (uint64_t)Cursor(kCurrentTai, i))
          << "port " << port << " pkt " << i;
    }
  }
  ut_txv_ctx* ctx_ = nullptr;
  ut_txv_stamps stamps_ = {};
};

TEST_F(St20TxFrameStampTest, AllRlSessionStampsOnlyTheFirstPkt) {
  ASSERT_EQ(Run(ST21_TX_PACING_WAY_RL, ST21_TX_PACING_WAY_RL, 2), kBuiltPkts);

  ExpectOnlyFirstPktStamped(MTL_SESSION_PORT_P);
  ExpectOnlyFirstPktStamped(MTL_SESSION_PORT_R);
}

/* tv_attach() sets the pacing way of the used ports only, so an unused R stays AUTO */
TEST_F(St20TxFrameStampTest, SinglePortRlSessionStampsOnlyTheFirstPkt) {
  ASSERT_EQ(Run(ST21_TX_PACING_WAY_RL, ST21_TX_PACING_WAY_AUTO, 1), kBuiltPkts);

  ExpectOnlyFirstPktStamped(MTL_SESSION_PORT_P);
}

/* tv_init_pacing() never leaves a session like this; the builder must not rely on it */
TEST_F(St20TxFrameStampTest, RlPortBesideATscPortStampsEveryPkt) {
  ASSERT_EQ(Run(ST21_TX_PACING_WAY_RL, ST21_TX_PACING_WAY_TSC, 2), kBuiltPkts);

  ExpectEveryPktStamped(MTL_SESSION_PORT_P);
  ExpectEveryPktStamped(MTL_SESSION_PORT_R);
}

TEST_F(St20TxFrameStampTest, CursorsEndOneTrsPerBuiltPktPastTheFrameStart) {
  ASSERT_EQ(Run(ST21_TX_PACING_WAY_RL, ST21_TX_PACING_WAY_RL, 2), kBuiltPkts);

  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), Cursor(kCurrentTsc, kBuiltPkts));
  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), Cursor(kCurrentTai, kBuiltPkts));
}

/* the frame ends past the last bulk's start cursor but before its end cursor */
TEST_F(St20TxFrameStampTest, FrameEndBeforeTheEndCursorIsNotLate) {
  ASSERT_EQ(Run(ST21_TX_PACING_WAY_RL, ST21_TX_PACING_WAY_RL, 2,
                static_cast<uint64_t>(6 * kTrs)),
            kBuiltPkts);

  EXPECT_EQ(ut_txv_stat_exceed_frame_time(ctx_), 0u);
}

class St20TxFrameStampWayTest : public St20TxFrameStampTest,
                                public ::testing::WithParamInterface<st21_tx_pacing_way> {
};

TEST_P(St20TxFrameStampWayTest, StampsEveryPktWithItsCursor) {
  ASSERT_EQ(Run(GetParam(), ST21_TX_PACING_WAY_AUTO, 1), kBuiltPkts);

  ExpectEveryPktStamped(MTL_SESSION_PORT_P);
}

INSTANTIATE_TEST_SUITE_P(NonRl, St20TxFrameStampWayTest,
                         ::testing::Values(ST21_TX_PACING_WAY_TSC,
                                           ST21_TX_PACING_WAY_TSC_NARROW,
                                           ST21_TX_PACING_WAY_BE, ST21_TX_PACING_WAY_PTP,
                                           ST21_TX_PACING_WAY_TSN),
                         [](const ::testing::TestParamInfo<st21_tx_pacing_way>& info) {
                           switch (info.param) {
                             case ST21_TX_PACING_WAY_TSC:
                               return "Tsc";
                             case ST21_TX_PACING_WAY_TSC_NARROW:
                               return "TscNarrow";
                             case ST21_TX_PACING_WAY_BE:
                               return "Be";
                             case ST21_TX_PACING_WAY_PTP:
                               return "Ptp";
                             default:
                               return "Tsn";
                           }
                         });

}  // namespace
