/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The TX hang detector reads the link flag of the port, which the control path sets. A
 * queue of a port with no link is not hung: a failed burst while the flag says down
 * restarts the hang time, so a queue still hung after the link is up recovers a hang
 * threshold later. The transmitter itself makes no link call. While the flag says down
 * the transmitter drops each burst at its time, as if the NIC took it, from the first
 * burst on; once up, the bursts go to the queue again from a frame start the stale pkts
 * of the ring cannot delay. That holds for each pacing way, RL, TSC, TSN and PTP.
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

  /* packet 0 of a frame through the TSC path at tsc, the queue refusing it or not */
  std::string Pkt0At(bool fail, uint64_t tsc) {
    ut_trs_set_mock_tsc_script(ctx_, &tsc, 1);
    ut_trs_set_burst_force_fail(ctx_, fail);
    return ut_session::capture_stderr([&] { ut_trs_call_burst_pkt0(ctx_, tsc); });
  }

  /* the flag says down at kDownNs: the pad burst there drops */
  void GoDown() {
    ut_trs_set_link_down(ctx_, true);
    BurstPadsAt(true, {kDownNs});
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLinkDownTest, HangWithTheLinkDownDoesNotRecover) {
  ut_trs_set_link_down(ctx_, true);

  BurstPadsAt(true, {kDownNs, kDownNs + 2 * kThreshNs});

  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, HangWithTheLinkUpRecovers) {
  BurstPadsAt(true, {kDownNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkUpWithTheQueueStillHungRecoversAThresholdLater) {
  ut_trs_set_link_down(ctx_, true);
  BurstPadsAt(true, {kDownNs});
  ut_trs_set_link_down(ctx_, false);

  Pkt0At(true, kDownNs + kThreshNs / 2);
  ASSERT_FALSE(ut_trs_recovery_pending(ctx_));
  BurstPadsAt(true, {kDownNs + 2 * kThreshNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, HangDetectionMakesNoLinkCall) {
  int calls = ut_trs_link_calls();

  BurstPadsAt(true, {kDownNs, kDownNs + 2 * kThreshNs});
  ut_trs_set_link_down(ctx_, true);
  BurstPadsAt(true, {kDownNs + 4 * kThreshNs, kDownNs + 6 * kThreshNs});

  EXPECT_EQ(calls, ut_trs_link_calls());
}

TEST_F(St20TxLinkDownTest, FirstBurstWithTheFlagDownDropsThePkts) {
  ut_trs_set_link_down(ctx_, true);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  BurstPadsAt(false, {kDownNs});

  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, ut_trs_link_down_drops(ctx_));
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

/* the NIC ring has room: only the drop keeps the pkt off it */
TEST_F(St20TxLinkDownTest, RlPktBurstWhileTheLinkIsDownDropsThePkts) {
  ut_trs_set_link_down(ctx_, true);
  ut_trs_set_rtcp(ctx_, true);
  ut_trs_hold_pkt(ctx_);
  ut_trs_enqueue_mid_pkt(ctx_);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);
  uint64_t tsc[] = {kDownNs + kThreshNs};
  ut_trs_set_mock_tsc_script(ctx_, tsc, 1);

  ut_session::capture_stderr([&] { ut_trs_call_rl_tasklet(ctx_); });

  EXPECT_EQ(1, ut_trs_link_down_drops(ctx_));
  EXPECT_EQ(1u, ut_trs_pkt_refcnt(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_rtcp_buffered(ctx_, nullptr));
}

/* packet 0 is due with the last pre-arm pads, which fit in absorb_ns */
TEST_F(St20TxLinkDownTest, RlTailBurstWhileTheLinkIsDownDropsPacket0AndNoPad) {
  constexpr int kP = 0;
  constexpr uint64_t kTargetTsc = 1000000;
  ut_trs_set_trs(ctx_, 1000);
  ut_trs_set_drain(ctx_, kP, 1000);
  ut_trs_set_credit(ctx_, kP, 2000);
  ut_trs_set_absorb(ctx_, kP, 1000);
  ut_trs_hold_pkt(ctx_);
  uint64_t entry[] = {kTargetTsc - 50000};
  ut_trs_set_mock_tsc_script(ctx_, entry, 1);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(1u, ut_trs_inflight_num(ctx_));
  ut_trs_set_rl_state_port(ctx_, kP, 0);
  ut_trs_set_rl_tail(ctx_, kTargetTsc - 9950); /* 10 pads to the target */
  ut_trs_set_link_down(ctx_, true);
  ut_trs_set_rtcp(ctx_, true);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);
  uint64_t tail[] = {kTargetTsc - 20000};
  ut_trs_set_mock_tsc_script(ctx_, tail, 1);

  ut_session::capture_stderr([&] { ut_trs_call_rl_tasklet(ctx_); });

  EXPECT_EQ(1, ut_trs_link_down_drops(ctx_));
  EXPECT_EQ(1u, ut_trs_pkt_refcnt(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0));
  EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1));
  EXPECT_EQ(0u, ut_trs_rtcp_buffered(ctx_, nullptr));
}

/* the launch time path hands each pkt its time for the NIC: it drops at the flag too */
TEST_F(St20TxLinkDownTest, LaunchTimeBurstWhileTheLinkIsDownDropsThePkts) {
  ut_trs_set_link_down(ctx_, true);
  ut_trs_hold_pkt(ctx_);
  ut_trs_enqueue_mid_pkt(ctx_);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  ut_session::capture_stderr([&] { ut_trs_call_launch_time_tasklet(ctx_); });

  EXPECT_EQ(1, ut_trs_link_down_drops(ctx_));
  EXPECT_EQ(1u, ut_trs_pkt_refcnt(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
}

TEST_F(St20TxLinkDownTest, PtpBurstWhileTheLinkIsDownDropsThePkts) {
  ut_trs_set_link_down(ctx_, true);
  ut_trs_hold_pkt(ctx_);
  ut_trs_enqueue_mid_pkt(ctx_);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  ut_session::capture_stderr([&] { ut_trs_call_ptp_tasklet(ctx_); });

  EXPECT_EQ(1, ut_trs_link_down_drops(ctx_));
  EXPECT_EQ(1u, ut_trs_pkt_refcnt(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
}

TEST_F(St20TxLinkDownTest, DroppedPktsAreCounted) {
  GoDown();

  BurstPadsAt(true, {kDownNs + kThreshNs, kDownNs + 2 * kThreshNs});

  EXPECT_EQ(3, ut_trs_link_down_drops(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkUpSendsTheBurstsToTheQueueAgainFromAFrameStart) {
  GoDown();
  ut_trs_set_link_down(ctx_, false);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  std::string log = BurstPadsAt(false, {kDownNs + kThreshNs});
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  Pkt0At(false, kDownNs + 2 * kThreshNs);
  BurstPadsAt(false, {kDownNs + 3 * kThreshNs});

  EXPECT_EQ(bursts + 2, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, Count(log, kResumeLog)) << log;
}

/* the stale pkts of the ring leave first: a frame that starts before they do drops */
TEST_F(St20TxLinkDownTest, LinkUpDropsAFrameThatStartsBeforeTheRingDrained) {
  constexpr uint64_t kTrsNs = 1000;
  constexpr uint64_t kUpNs = kDownNs + kThreshNs;
  ut_trs_set_trs(ctx_, kTrsNs);
  ut_trs_set_nb_tx_desc(ctx_, 512);
  GoDown();
  ut_trs_set_link_down(ctx_, false);
  BurstPadsAt(false, {kUpNs});
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  Pkt0At(false, kUpNs + 511 * kTrsNs);
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));
  Pkt0At(false, kUpNs + 512 * kTrsNs);

  EXPECT_EQ(bursts + 1, ut_trs_burst_call_count(ctx_));
}

/* RL warm-up/pre-arm starts warm_pkts * trs before packet 0: a frame whose packet 0 falls
 * inside [link_up + nb_tx_desc * trs, link_up + (nb_tx_desc + warm_pkts) * trs) must
 * stay dropped so pads do not queue behind stale descriptors (ST 2110-21 6.6.2 p11). */
TEST_F(St20TxLinkDownTest, RlLinkUpDropsAFrameWhoseWarmUpOverlapsTheRingDrain) {
  constexpr uint64_t kTrsNs = 4000;
  constexpr uint64_t kUpNs = kDownNs + kThreshNs;
  constexpr uint32_t kWarmPkts = 128;
  ut_trs_set_rl_port(ctx_, 0);
  ut_trs_set_trs(ctx_, kTrsNs);
  ut_trs_set_nb_tx_desc(ctx_, 512);
  ut_trs_set_warm_pkts_cap(ctx_, kWarmPkts);
  GoDown();
  ut_trs_set_link_down(ctx_, false);
  BurstPadsAt(false, {kUpNs});

  uint64_t drain_end = kUpNs + 512 * kTrsNs;
  uint64_t early_target = drain_end + (kWarmPkts - 1) * kTrsNs;
  uint64_t safe_target = drain_end + kWarmPkts * kTrsNs;

  uint64_t tsc_early[] = {kUpNs + 1000, kUpNs + 2000, kUpNs + 3000};
  ut_trs_set_mock_tsc_script(ctx_, tsc_early, 3);
  ut_trs_enqueue_first_pkt(ctx_, early_target);
  ut_trs_call_rl_tasklet(ctx_);
  /* warm-up window overlaps ring drain: pad burst must still drop */
  uint32_t bursts = ut_trs_burst_call_count(ctx_);
  BurstPadsAt(false, {kUpNs + 4000});
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));

  /* clear inflight from early_target and send a frame whose warm-up starts after drain */
  ut_trs_set_inflight_num(ctx_, 0);
  ut_trs_set_rl_state_port(ctx_, 0, 0);
  ut_trs_set_target_tsc(ctx_, 0);
  uint64_t tsc_safe[] = {kUpNs + 5000, kUpNs + 6000, kUpNs + 7000};
  ut_trs_set_mock_tsc_script(ctx_, tsc_safe, 3);
  ut_trs_enqueue_first_pkt(ctx_, safe_target);
  ut_trs_call_rl_tasklet(ctx_);
  bursts = ut_trs_burst_call_count(ctx_);
  BurstPadsAt(false, {kUpNs + 8000});
  EXPECT_EQ(bursts + 1, ut_trs_burst_call_count(ctx_));
}

/* Armed RL plan: draining stale descriptors empties the shaper token bucket, so pre-arm
 * also needs refill_ns before its warm-up window opens (ST 2110-21 6.6.2 p11, 7.1.2 p12).
 */
TEST_F(St20TxLinkDownTest, RlLinkUpDropsAFrameWhoseRefillOverlapsTheRingDrain) {
  constexpr uint64_t kTrsNs = 4000;
  constexpr uint64_t kUpNs = kDownNs + kThreshNs;
  constexpr uint32_t kWarmPkts = 128;
  constexpr uint64_t kRefillNs = 8 * kTrsNs;
  ut_trs_set_rl_port(ctx_, 0);
  ut_trs_set_trs(ctx_, kTrsNs);
  ut_trs_set_nb_tx_desc(ctx_, 512);
  ut_trs_set_warm_pkts_cap(ctx_, kWarmPkts);
  ut_trs_set_vrx_detect(ctx_, 0, 12000, kRefillNs);
  GoDown();
  ut_trs_set_link_down(ctx_, false);
  BurstPadsAt(false, {kUpNs});

  uint64_t drain_end = kUpNs + 512 * kTrsNs;
  uint64_t early_target = drain_end + kWarmPkts * kTrsNs + kRefillNs - kTrsNs;
  uint64_t safe_target = drain_end + kWarmPkts * kTrsNs + kRefillNs;

  uint64_t tsc_early[] = {kUpNs + 1000, kUpNs + 2000, kUpNs + 3000};
  ut_trs_set_mock_tsc_script(ctx_, tsc_early, 3);
  ut_trs_enqueue_first_pkt(ctx_, early_target);
  ut_trs_call_rl_tasklet(ctx_);
  uint32_t bursts = ut_trs_burst_call_count(ctx_);
  BurstPadsAt(false, {kUpNs + 4000});
  EXPECT_EQ(bursts, ut_trs_burst_call_count(ctx_));

  ut_trs_set_inflight_num(ctx_, 0);
  ut_trs_set_rl_state_port(ctx_, 0, 0);
  ut_trs_set_target_tsc(ctx_, 0);
  uint64_t tsc_safe[] = {kUpNs + 5000, kUpNs + 6000, kUpNs + 7000};
  ut_trs_set_mock_tsc_script(ctx_, tsc_safe, 3);
  ut_trs_enqueue_first_pkt(ctx_, safe_target);
  ut_trs_call_rl_tasklet(ctx_);
  bursts = ut_trs_burst_call_count(ctx_);
  BurstPadsAt(false, {kUpNs + 8000});
  EXPECT_EQ(bursts + 1, ut_trs_burst_call_count(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkDownLogsOnceADownPeriod) {
  ut_trs_set_link_down(ctx_, true);

  std::string log =
      BurstPadsAt(true, {kDownNs, kDownNs + kThreshNs * 2, kDownNs + kThreshNs * 4});

  EXPECT_EQ(1, Count(log, kDownLog)) << log;
}

TEST_F(St20TxLinkDownTest, RlCatchUpDoesNotDrainTheRingWhileTheLinkIsDown) {
  ASSERT_EQ(0, ut_trs_setup_stream(ctx_, 4000.0L, 512, 1));
  ut_trs_set_sch_schedule_ns(ctx_, 100000);
  ut_trs_stream_fill(ctx_);
  ut_trs_set_link_down(ctx_, true);

  ut_trs_ctx* list[] = {ctx_};
  ut_session::capture_stderr([&] { ut_trs_call_transmitter(list, 1); });

  /* one RL visit does at most two bulks (8 pkts), never a catch-up loop over the ring */
  EXPECT_LE(ut_trs_link_down_drops(ctx_), 8);
}

/* the drops send no burst, so only the up restarts the hang time */
TEST_F(St20TxLinkDownTest, LinkUpAfterALongDownRestartsTheHangTime) {
  GoDown();
  BurstPadsAt(true, {kDownNs + 10 * kThreshNs});
  ut_trs_set_link_down(ctx_, false);

  Pkt0At(true, kDownNs + 10 * kThreshNs + kThreshNs / 2);
  ASSERT_FALSE(ut_trs_recovery_pending(ctx_));
  BurstPadsAt(true, {kDownNs + 12 * kThreshNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

/* An RL leg of 1080p59.94 BPM, its plan armed, on a shaper with a bucket of 8 pkts, as
 * an ice PF with an rl_burst_size of 10 KB: an idle one sends 8 pkts at once. While the
 * link is down the leg drops each frame as fast as the builder adds 4 pkts a call. The
 * link comes up mid-frame. The leg resumes at the next frame start, whose pre-arm pads
 * take the burst, so the network compatibility bucket stays within CMAX, 6 for this
 * format (ST 2110-21:2022 6.6.1 p9-10, 7.1.2 p12). */
class St20TxRlLinkUpTest : public ::testing::Test {
 protected:
  static constexpr long double kTrsNs = 16683333.0L * 1080 / 1125 / 4320;
  static constexpr uint32_t kFramePkts = 4320;
  static constexpr uint64_t kFrameNs = 16683333;
  static constexpr uint64_t kCallNs = 1000;
  static constexpr uint32_t kCmax = 6;

  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ASSERT_EQ(0, ut_trs_setup_stream(ctx_, kTrsNs, 512, 1));
    ut_trs_set_nb_tx_desc(ctx_, 512);
    ut_trs_set_warm_pkts_cap(ctx_, 128);
    ut_trs_set_drain(ctx_, 0, kTrsNs);
    ut_trs_set_credit(ctx_, 0, 8 * kTrsNs);
    ut_trs_set_absorb(ctx_, 0, 2000);
    ut_trs_set_stream_shaper(ctx_, 8 * kTrsNs);
    ut_trs_set_stream_build(ctx_, 4);
  }
  void TearDown() override {
    ut_trs_destroy(ctx_);
  }
  /* until ns past the first packet 0 */
  void RunTo(uint64_t ns) {
    uint64_t now = ut_trs_last_tsc(ctx_);
    ut_trs_stream_frames(ctx_, kFrameNs + ns - now, kCallNs, kFramePkts, kFrameNs,
                         kFrameNs);
  }
  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxRlLinkUpTest, LinkUpMidFrameKeepsCinstWithinCmax) {
  constexpr long double kDrainNs = 16683333.0L / 4320 / 1.1L;
  RunTo(kFrameNs / 2);
  ASSERT_GT(ut_trs_stream_pkts_left(ctx_), 0u);
  ut_session::capture_stderr([&] {
    ut_trs_stream_link(ctx_, false);
    RunTo(2 * kFrameNs + 200000);
    ut_trs_stream_link(ctx_, true);
    ut_trs_set_stream_cinst(ctx_, kDrainNs);
    RunTo(5 * kFrameNs);
  });

  EXPECT_GE(ut_trs_stream_pkts_left(ctx_), kFramePkts);
  EXPECT_LE(ut_trs_stream_cinst_max(ctx_), kCmax);
}

}  // namespace
