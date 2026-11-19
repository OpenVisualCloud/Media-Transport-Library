/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins ST20_TX_FLAG_USER_PACING / ST20_TX_FLAG_EXACT_USER_PACING on RTP-level TX
 * sessions: the TAI handed to st20_tx_put_mbuf_tai() / st22_tx_put_mbuf_tai() rides
 * on the app mbuf and paces the frame that mbuf starts, through the real
 * get_mbuf/put_mbuf API and tv_build_rtp() / tv_build_rtp_chain().
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRtpUserPacingTest.*'
 */

#include <gtest/gtest.h>

#include <string>

#include "session/st20_tx_harness.h"
#include "session/stderr_capture.h"

namespace {
constexpr uint64_t kFrameNs = 1'000'000;
constexpr uint64_t kNowEpoch = 1000;
constexpr uint64_t kNowTai = kNowEpoch * kFrameNs;
constexpr uint64_t kNowTsc = 5 * kFrameNs;
constexpr uint64_t kTrOffsetNs = 100'000;
constexpr uint64_t kOneSecondNs = 1000 * kFrameNs;
constexpr char kMissingTaiLog[] = "frame has no TAI";
}  // namespace

class St20TxRtpUserPacingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    Reset();
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  void Reset() {
    ut_txv_destroy(ctx_);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFrameNs);
    ut_txv_set_tr_offset(ctx_, kTrOffsetNs);
    ut_txv_set_cur_epochs(ctx_, kNowEpoch - 1);
    ut_txv_set_mock_ptp_time(ctx_, kNowTai);
    ut_txv_set_mock_tsc_time(ctx_, kNowTsc);
  }
  void SetFlags(bool user_pacing, bool exact) {
    ut_txv_set_user_pacing(ctx_, user_pacing);
    ut_txv_set_exact_user_pacing(ctx_, exact);
  }
  template <int N>
  void Run(bool tx_no_chain, const ut_txv_rtp_pkt (&pkts)[N]) {
    ASSERT_EQ(ut_txv_run_rtp_pkts(ctx_, tx_no_chain, pkts, N, sent_, nullptr), N);
  }
  /* where default pacing starts the frame of an epoch */
  static uint64_t DefaultStart(uint64_t epoch) {
    return ut_txv_round_to_media_clk(epoch * kFrameNs + kTrOffsetNs,
                                     ST10_VIDEO_SAMPLING_RATE_90K);
  }
  void ExpectStartsAt(int pkt, uint64_t tai) {
    EXPECT_EQ(sent_[pkt].ptp, tai);
    EXPECT_EQ(sent_[pkt].tsc, kNowTsc + (tai - kNowTai));
  }
  ut_txv_ctx* ctx_ = nullptr;
  ut_txv_rtp_sent sent_[UT_TXV_RTP_PKTS_MAX] = {};
};

TEST_F(St20TxRtpUserPacingTest, ExactUserPacingSendsTheFirstPacketAtTheTai) {
  constexpr uint64_t kTai = kNowTai + 300'000 + 7; /* off the media clock grid */
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);

    Run(tx_no_chain, {{100, kTai}, {100, 0}});

    ExpectStartsAt(0, kTai);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* 0.6 frame past an epoch: the nearest is the next one, which floor would miss */
TEST_F(St20TxRtpUserPacingTest, UserPacingWithoutExactLandsOnTheNearestEpoch) {
  constexpr uint64_t kEpoch = kNowEpoch + 3;
  constexpr uint64_t kTai = (kEpoch - 1) * kFrameNs + 600'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, false);

    Run(tx_no_chain, {{100, kTai}});

    EXPECT_EQ(sent_[0].epoch, kEpoch);
    ExpectStartsAt(0, DefaultStart(kEpoch));
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* All three frames are queued before the tasklet builds the first, so a TAI held
 * per session rather than per mbuf would pace every frame with the last one. The second
 * packet of a frame shares its RTP timestamp, so it starts no frame and its tai is not
 * read. */
TEST_F(St20TxRtpUserPacingTest, EachQueuedFrameIsPacedByItsOwnTai) {
  constexpr uint64_t kTai0 = kNowTai + 200'000;
  constexpr uint64_t kTai1 = kNowTai + 3 * kFrameNs + 50'000;
  constexpr uint64_t kTai2 = kNowTai + 2 * kFrameNs + 1;
  constexpr uint64_t kIgnoredTai = 1; /* in the past: an error, were it read */
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);

    Run(tx_no_chain, {{100, kTai0},
                      {100, kIgnoredTai},
                      {200, kTai1},
                      {200, kIgnoredTai},
                      {300, kTai2},
                      {300, kIgnoredTai}});

    ExpectStartsAt(0, kTai0);
    ExpectStartsAt(2, kTai1);
    ExpectStartsAt(4, kTai2);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* The app did not supply a TAI for a frame: default pacing, one error a frame, and
 * one log line a session rather than one a frame from the tasklet. */
TEST_F(St20TxRtpUserPacingTest, FrameWithoutTaiFallsBackToDefaultPacing) {
  for (bool exact : {false, true}) {
    for (bool tx_no_chain : {true, false}) {
      SCOPED_TRACE(exact ? "EXACT_USER_PACING" : "USER_PACING");
      SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
      Reset();
      SetFlags(true, exact);

      std::string log =
          ut_session::capture_stderr([&] { Run(tx_no_chain, {{100, 0}, {200, 0}}); });

      ExpectStartsAt(0, DefaultStart(kNowEpoch));
      ExpectStartsAt(1, DefaultStart(kNowEpoch + 1));
      EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 2u);
      size_t first = log.find(kMissingTaiLog);
      EXPECT_NE(first, std::string::npos);
      EXPECT_EQ(log.find(kMissingTaiLog, first + 1), std::string::npos);
    }
  }
}

TEST_F(St20TxRtpUserPacingTest, ExactTaiOutsideToleranceFallsBackToDefaultPacing) {
  constexpr uint32_t kWarmPkts = 10;
  constexpr uint64_t kTrsNs = 1000; /* a 10us warm-up lead */
  const struct {
    const char* name;
    uint64_t tai;
  } cases[] = {
      {"in the past", kNowTai - 1},
      {"more than 1s ahead", kNowTai + kOneSecondNs + 1},
      {"inside the warm-up lead", kNowTai + kWarmPkts * kTrsNs / 2},
  };
  for (const auto& c : cases) {
    for (bool tx_no_chain : {true, false}) {
      SCOPED_TRACE(c.name);
      SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
      Reset();
      SetFlags(true, true);
      ut_txv_set_warm_pkts(ctx_, kWarmPkts);
      ut_txv_set_trs(ctx_, kTrsNs);

      Run(tx_no_chain, {{100, c.tai}});

      ExpectStartsAt(0, DefaultStart(kNowEpoch));
      EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
    }
  }
}

/* Without EXACT an out-of-range TAI is only counted: the frame keeps the epoch nearest
 * to it, so a past one goes out at once and a far one is held, as at frame level. */
TEST_F(St20TxRtpUserPacingTest, NonExactTaiOutOfRangeKeepsItsNearestEpoch) {
  constexpr uint64_t kPastEpoch = kNowEpoch - 5;
  constexpr uint64_t kFarEpoch = kNowEpoch + 2000; /* 2s ahead */
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    {
      SCOPED_TRACE("in the past");
      Reset();
      SetFlags(true, false);

      Run(tx_no_chain, {{100, kPastEpoch * kFrameNs}});

      EXPECT_EQ(sent_[0].epoch, kPastEpoch);
      EXPECT_EQ(sent_[0].ptp, DefaultStart(kPastEpoch));
      EXPECT_EQ(sent_[0].tsc, kNowTsc);
      EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
    }
    {
      SCOPED_TRACE("more than 1s ahead");
      Reset();
      SetFlags(true, false);

      Run(tx_no_chain, {{100, kFarEpoch * kFrameNs}});

      EXPECT_EQ(sent_[0].epoch, kFarEpoch);
      ExpectStartsAt(0, DefaultStart(kFarEpoch));
      EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
    }
  }
}

/* The R leg of a redundant session goes out on the same schedule as P. */
TEST_F(St20TxRtpUserPacingTest, RedundantLegCarriesThePrimaryPacing) {
  constexpr uint64_t kTai0 = kNowTai + 200'000 + 3;
  constexpr uint64_t kTai1 = kNowTai + 2 * kFrameNs + 5;
  const ut_txv_rtp_pkt pkts[] = {{100, kTai0}, {100, 0}, {200, kTai1}};
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_st20_redundant"
                             : "tv_build_rtp_redundant_chain");
    ut_txv_rtp_sent sent_r[UT_TXV_RTP_PKTS_MAX] = {};
    Reset();
    SetFlags(true, true);

    ASSERT_EQ(ut_txv_run_rtp_pkts(ctx_, tx_no_chain, pkts, 3, sent_, sent_r), 3);

    ExpectStartsAt(0, kTai0);
    ExpectStartsAt(2, kTai1);
    for (int i = 0; i < 3; i++) {
      SCOPED_TRACE(i);
      EXPECT_EQ(sent_r[i].ptp, sent_[i].ptp);
      EXPECT_EQ(sent_r[i].tsc, sent_[i].tsc);
      EXPECT_EQ(sent_r[i].rtp_ts, sent_[i].rtp_ts);
    }
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* USER_TIMESTAMP keeps the app RTP timestamp; the TAI only moves the send time. The two
 * frames vary them in opposite directions. */
TEST_F(St20TxRtpUserPacingTest, UserTimestampKeepsTheRtpTimestampTheTaiSetsTheStart) {
  constexpr uint32_t kRtpTs0 = 0x12345678;
  constexpr uint32_t kRtpTs1 = 0x10;
  constexpr uint64_t kTai0 = kNowTai + 4 * kFrameNs + 3;
  constexpr uint64_t kTai1 = kNowTai + kFrameNs + 9;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);
    ut_txv_set_user_timestamp(ctx_, true);

    Run(tx_no_chain, {{kRtpTs0, kTai0}, {kRtpTs1, kTai1}});

    EXPECT_EQ(sent_[0].rtp_ts, kRtpTs0);
    ExpectStartsAt(0, kTai0);
    EXPECT_EQ(sent_[1].rtp_ts, kRtpTs1);
    ExpectStartsAt(1, kTai1);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

TEST_F(St20TxRtpUserPacingTest, TaiIsIgnoredWithoutUserPacing) {
  constexpr uint64_t kTai = (kNowEpoch + 5) * kFrameNs;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(false, false);

    Run(tx_no_chain, {{100, kTai}});

    EXPECT_EQ(sent_[0].epoch, kNowEpoch);
    ExpectStartsAt(0, DefaultStart(kNowEpoch));
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* st22_tx_create() maps ST22_TX_FLAG_USER_PACING onto ST20_TX_FLAG_USER_PACING. */
TEST_F(St20TxRtpUserPacingTest, St22RtpLevelLandsOnTheEpochNearestItsTai) {
  constexpr uint64_t kEpoch = kNowEpoch + 2;
  constexpr uint64_t kTai = kEpoch * kFrameNs - 300'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    ut_txv_set_st22_rtp_level(ctx_, true);
    SetFlags(true, false);

    Run(tx_no_chain, {{100, kTai}});

    EXPECT_EQ(sent_[0].epoch, kEpoch);
    ExpectStartsAt(0, DefaultStart(kEpoch));
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}
