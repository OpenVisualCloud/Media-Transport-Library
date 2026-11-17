/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The audio transmitter reads the link of the port once a hang threshold passes with no
 * burst. The queue of a port with no link is not hung: no recovery, and the pkt stays
 * for the next burst, which drops it. One log line a down period, one link read until
 * a burst checks the link again.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxLinkDownTest.*'
 */

#include <gtest/gtest.h>

#include <string>

#include "session/st30_tx_harness.h"
#include "session/stderr_capture.h"

namespace {

constexpr int kPort = 0;
constexpr uint64_t kThreshNs = 1000;
const std::string kDownLog = "link down, TX drops the pkts until the link is up";

class St30TxLinkDownTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txa30_destroy(ctx_);
  }
  uint16_t FailAt(uint64_t tsc_ns) {
    ut_txa30_set_mock_tsc_time(ctx_, tsc_ns);
    return ut_txa30_trs_burst_fail(ctx_, kPort, kThreshNs);
  }
  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_F(St30TxLinkDownTest, HangWithTheLinkDownKeepsThePkt) {
  ut_txa30_set_link_down(ctx_, true);

  EXPECT_EQ(FailAt(5000), 0);
  EXPECT_EQ(ut_txa30_last_burst_succ_tsc(ctx_, kPort), 5000u);
  EXPECT_EQ(ut_txa30_link_reads(ctx_), 1);
}

TEST_F(St30TxLinkDownTest, HangWithTheLinkUpTakesTheRecovery) {
  EXPECT_EQ(FailAt(5000), 1);
}

TEST_F(St30TxLinkDownTest, BurstFailUnderTheThresholdReadsNoLink) {
  EXPECT_EQ(FailAt(kThreshNs), 0);
  EXPECT_EQ(ut_txa30_link_reads(ctx_), 0);
}

TEST_F(St30TxLinkDownTest, LinkDownLogsOnceADownPeriod) {
  ut_txa30_set_link_down(ctx_, true);

  std::string log = ut_session::capture_stderr([&] {
    FailAt(5000);
    FailAt(10000);
  });

  EXPECT_EQ(ut_txa30_link_reads(ctx_), 1);
  size_t first = log.find(kDownLog);
  ASSERT_NE(first, std::string::npos) << log;
  EXPECT_EQ(log.find(kDownLog, first + 1), std::string::npos) << log;
}

}  // namespace
