/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Review of PR #1957: three cases that fail on the PR code. Each one pins what a
 * receiver needs: a frame goes out at its start time, paced at TRS, and does not
 * overlap the frame before it (ST 2110-21:2022 6.2 p6, 6.3.2 p7).
 *
 * Found with the sample of the branch DawidWesierski4:pr-1957-demo on E830 VFs, TSC
 * pacing:
 *  - without EXACT, a TAI 1.5 s ahead: the 4320 packets of the frame arrive in 2977 us
 *    and not in 16002 us, with 1080 "invalid tsc cur .. target .." errors;
 *  - with EXACT, a frame with no TAI after a frame at 0.37 TFRAME: the frame arrives in
 *    14111 us and not in 16003 us, because its start is in the past.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRtpUserPacingBugs*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

namespace {
constexpr uint64_t kFrameNs = 1'000'000;
constexpr uint64_t kNowEpoch = 1000;
constexpr uint64_t kNowTai = kNowEpoch * kFrameNs;
constexpr uint64_t kNowTsc = 5 * kFrameNs;
constexpr uint64_t kTrOffsetNs = 100'000;
constexpr uint64_t kOneSecondNs = 1000 * kFrameNs;
}  // namespace

class St20TxRtpUserPacingBugsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFrameNs);
    ut_txv_set_tr_offset(ctx_, kTrOffsetNs);
    ut_txv_set_cur_epochs(ctx_, kNowEpoch - 1);
    ut_txv_set_mock_ptp_time(ctx_, kNowTai);
    ut_txv_set_mock_tsc_time(ctx_, kNowTsc);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  template <int N>
  void Run(bool tx_no_chain, const ut_txv_rtp_pkt (&pkts)[N]) {
    ASSERT_EQ(ut_txv_run_rtp_pkts(ctx_, tx_no_chain, pkts, N, sent_, nullptr), N);
  }
  ut_txv_ctx* ctx_ = nullptr;
  ut_txv_rtp_sent sent_[UT_TXV_RTP_PKTS_MAX] = {};
};

/* Without EXACT a TAI 2 s ahead keeps its nearest epoch, and st20_api.h says so. But
 * the TSC transmitter does not wait more than 1 s (next case), so the session must not
 * give it a start more than 1 s ahead: refuse the TAI as EXACT does, or clamp it. */
TEST_F(St20TxRtpUserPacingBugsTest, NonExactTaiFarAheadNeverAsksForAWaitOverOneSecond) {
  constexpr uint64_t kFarTai = kNowTai + 2 * kOneSecondNs;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    ut_txv_set_user_pacing(ctx_, true);
    ut_txv_set_exact_user_pacing(ctx_, false);

    Run(tx_no_chain, {{100, kFarTai}});

    EXPECT_LE(sent_[0].tsc - kNowTsc, kOneSecondNs)
        << "the transmitter sends a frame with a wait over 1 s at once, unpaced";
  }
}

/* With EXACT, frame 0 starts at 0.37 TFRAME after epoch N, the value of the run on the
 * NIC. Its nearest epoch is N, so frame 1, which has no TAI, gets epoch N+1 from default
 * pacing, TROFFSET (0.1 TFRAME here) after it: only 0.73 TFRAME after frame 0 started.
 * Frame 0 is still on the wire, so frame 1 starts late and its first packets go out in
 * a burst to catch up. A TAI above 0.5 TFRAME rounds up and hides the fault. The next
 * frame must not start before one frame period after the start of the frame before. */
TEST_F(St20TxRtpUserPacingBugsTest, ExactFallbackFrameDoesNotOverlapThePreviousFrame) {
  constexpr uint64_t kTai0 = kNowTai + 370'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    ut_txv_set_user_pacing(ctx_, true);
    ut_txv_set_exact_user_pacing(ctx_, true);
    ut_txv_set_cur_epochs(ctx_, kNowEpoch - 1);

    Run(tx_no_chain, {{100, kTai0}, {200, 0}});

    ASSERT_EQ(sent_[0].ptp, kTai0);
    EXPECT_GE(sent_[1].ptp, sent_[0].ptp + kFrameNs)
        << "frame 1 starts " << (sent_[0].ptp + kFrameNs - sent_[1].ptp)
        << " ns before frame 0 has had its frame period";
  }
}

class St20TxRtpUserPacingBugsTrsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_trs_destroy(ctx_);
  }
  ut_trs_ctx* ctx_ = nullptr;
};

/* video_trs_tsc_tasklet() logs "invalid tsc cur .. target .." and sends a packet whose
 * target is more than 1 s ahead at once. For a frame, that is each bulk of the frame:
 * the whole frame goes out at the rate of the tasklet, not at TRS. */
TEST_F(St20TxRtpUserPacingBugsTrsTest, TscTransmitterDoesNotSendAPacketBeforeItsTarget) {
  const uint64_t now = 10 * kOneSecondNs;
  ut_trs_set_mock_tsc_script(ctx_, &now, 1);
  ut_trs_enqueue_first_pkt(ctx_, now + kOneSecondNs + kOneSecondNs / 2);

  ut_trs_call_tsc_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_)) << "packet 0 left 1.5 s before its target";
}
