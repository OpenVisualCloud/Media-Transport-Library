/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The TX hang detector reads the link of the port once a threshold passes with no
 * burst. The queue of a port with no link is not hung: no recovery. Until the link is
 * up again the transmitter drops each burst at its time, as if the NIC took it. While
 * down it reads the link at most every 100 ms; once up, the bursts go to the queue again.
 * A burst fail under the threshold reads no link.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxLinkDownTest.*'
 */

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "session/st_video_transmitter_harness.h"
#include "session/stderr_capture.h"

namespace {

constexpr uint64_t kThreshNs = 1000;
constexpr uint64_t kDownNs = 5000;
constexpr uint64_t kCheckNs = 100000000; /* the link read period while down */
const std::string kDownLog = "link down, TX drops the pkts until the link is up";
const std::string kResumeLog = "link up, TX resumes";

int Count(const std::string& log, const std::string& line) {
  int n = 0;
  for (size_t pos = log.find(line); pos != std::string::npos;
       pos = log.find(line, pos + 1))
    n++;
  return n;
}

class St20TxLinkDownTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_hang_detect_thresh_ns(ctx_, kThreshNs);
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  /* one pad burst at each tsc, the queue refusing them or not */
  std::string BurstPadsAt(bool fail, std::initializer_list<uint64_t> tsc) {
    std::vector<uint64_t> script(tsc);
    ut_trs_set_mock_tsc_script(ctx_, script.data(), (int)script.size());
    ut_trs_set_burst_force_fail(ctx_, fail);
    return ut_session::capture_stderr([&] {
      for (size_t i = 0; i < script.size(); i++) ut_trs_call_burst_pad(ctx_);
    });
  }

  /* the queue hangs and the threshold read finds the link down at kDownNs */
  void GoDown() {
    ut_trs_set_link_down(ctx_, true);
    BurstPadsAt(true, {kDownNs});
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLinkDownTest, HangWithTheLinkDownDoesNotRecover) {
  GoDown();

  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
  EXPECT_EQ(1, ut_trs_link_reads(ctx_));
}

TEST_F(St20TxLinkDownTest, BurstWhileTheLinkIsDownDropsThePkts) {
  GoDown();
  uint32_t bursts = ut_trs_burst_call_count(ctx_);
  uint16_t refcnt = ut_trs_pad_refcnt(ctx_);

  ut_trs_set_burst_force_fail(ctx_, true);
  uint64_t tsc[] = {kDownNs + kThreshNs};
  ut_trs_set_mock_tsc_script(ctx_, tsc, 1);
  EXPECT_EQ(1u, ut_trs_call_burst_pad(ctx_));

  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(refcnt, ut_trs_pad_refcnt(ctx_));
  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, DroppedPktsAreCounted) {
  GoDown();

  BurstPadsAt(true, {kDownNs + kThreshNs, kDownNs + 2 * kThreshNs});

  EXPECT_EQ(2, ut_trs_link_down_drops(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkIsReadAtMostOnceACheckPeriod) {
  GoDown();

  BurstPadsAt(true, {kDownNs + kCheckNs / 2});
  EXPECT_EQ(1, ut_trs_link_reads(ctx_));
  BurstPadsAt(true, {kDownNs + kCheckNs});
  EXPECT_EQ(2, ut_trs_link_reads(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkUpSendsTheBurstsToTheQueueAgain) {
  GoDown();
  ut_trs_set_link_down(ctx_, false);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  std::string log = BurstPadsAt(false, {kDownNs + kCheckNs});

  EXPECT_EQ(2, ut_trs_link_reads(ctx_));
  EXPECT_EQ(bursts + 1, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, Count(log, kResumeLog)) << log;
}

TEST_F(St20TxLinkDownTest, LinkDownLogsOnceADownPeriod) {
  ut_trs_set_link_down(ctx_, true);

  std::string log =
      BurstPadsAt(true, {kDownNs, kDownNs + kThreshNs * 2, kDownNs + kThreshNs * 4});

  EXPECT_EQ(1, Count(log, kDownLog)) << log;
}

/* the resume restarts the hang time: a queue still hung recovers a threshold later */
TEST_F(St20TxLinkDownTest, LinkUpWithTheQueueStillHungRecovers) {
  GoDown();
  ut_trs_set_link_down(ctx_, false);

  BurstPadsAt(true, {kDownNs + kCheckNs});
  ASSERT_FALSE(ut_trs_recovery_pending(ctx_));
  BurstPadsAt(true, {kDownNs + kCheckNs + 2 * kThreshNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

/* with the default 1 s threshold, a link down shows after one check period of fails */
TEST_F(St20TxLinkDownTest, LinkDownIsFoundAfterACheckPeriodOfFails) {
  ut_trs_set_hang_detect_thresh_ns(ctx_, 1000000000);
  ut_trs_set_link_down(ctx_, true);
  ut_trs_set_last_burst_succ_tsc(ctx_, kDownNs);

  BurstPadsAt(true, {kDownNs + kCheckNs / 2});
  EXPECT_EQ(0, ut_trs_link_reads(ctx_));
  BurstPadsAt(true, {kDownNs + kCheckNs + 1});

  EXPECT_EQ(1, ut_trs_link_reads(ctx_));
  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, BurstFailUnderTheThresholdReadsNoLink) {
  ut_trs_set_last_burst_succ_tsc(ctx_, 4950);

  BurstPadsAt(true, {5000});

  EXPECT_EQ(0, ut_trs_link_reads(ctx_));
}

}  // namespace
