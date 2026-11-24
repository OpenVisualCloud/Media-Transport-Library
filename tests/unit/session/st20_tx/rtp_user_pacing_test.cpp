/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins ST20_TX_FLAG_USER_PACING / ST20_TX_FLAG_EXACT_USER_PACING on RTP-level TX
 * sessions: the TAI handed to st20_tx_put_mbuf_tai() / st22_tx_put_mbuf_tai() rides
 * on the app mbuf and paces the frame that mbuf starts, through the real
 * get_mbuf/put_mbuf API and tv_build_rtp() / tv_build_rtp_chain(). Also pins two
 * receiver rules (ST 2110-21:2022 6.2, 6.3.2): no start more than 1 s ahead reaches
 * the TSC transmitter, which would send the frame unpaced, and no default-paced frame
 * starts within one frame time of an EXACT frame before it.
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

/* The app did not supply a TAI for a frame: default pacing and one error a frame.
 * The builder tasklet logs nothing at warning level for it; the stat dump reports the
 * count. */
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
      EXPECT_EQ(log.find(kMissingTaiLog), std::string::npos);
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

/* Without EXACT a past TAI is only counted: the frame keeps the epoch nearest to it and
 * goes out at once, as at frame level. */
TEST_F(St20TxRtpUserPacingTest, NonExactTaiInThePastKeepsItsNearestEpoch) {
  constexpr uint64_t kPastEpoch = kNowEpoch - 5;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, false);

    Run(tx_no_chain, {{100, kPastEpoch * kFrameNs}});

    EXPECT_EQ(sent_[0].epoch, kPastEpoch);
    EXPECT_EQ(sent_[0].ptp, DefaultStart(kPastEpoch));
    EXPECT_EQ(sent_[0].tsc, kNowTsc);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  }
}

/* The transmitter does not wait over 1 s: it sends such a frame at once, unpaced. So a
 * TAI whose start would be more than 1 s ahead is refused and the frame gets default
 * pacing, with or without EXACT. Without EXACT, 1 ns under 1 s still overshoots: its
 * nearest epoch is 1 s ahead and TROFFSET comes on top. */
TEST_F(St20TxRtpUserPacingTest, NoStartMoreThanOneSecondAheadReachesTheTransmitter) {
  const struct {
    const char* name;
    bool exact;
    uint64_t tai;
  } cases[] = {
      {"USER_PACING, 1 ns under 1 s", false, kNowTai + kOneSecondNs - 1},
      {"USER_PACING, 2 s", false, kNowTai + 2 * kOneSecondNs},
      {"EXACT_USER_PACING, 1 ns over 1 s", true, kNowTai + kOneSecondNs + 1},
      {"EXACT_USER_PACING, 2 s", true, kNowTai + 2 * kOneSecondNs},
  };
  for (const auto& c : cases) {
    for (bool tx_no_chain : {true, false}) {
      SCOPED_TRACE(c.name);
      SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
      Reset();
      SetFlags(true, c.exact);

      Run(tx_no_chain, {{100, c.tai}});

      EXPECT_LE(sent_[0].tsc - kNowTsc, kOneSecondNs)
          << "the transmitter sends a frame with a wait over 1 s at once, unpaced";
      ExpectStartsAt(0, DefaultStart(kNowEpoch));
      EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
    }
  }
}

/* The refusal is for starts over 1 s only: without EXACT the last epoch whose start is
 * still inside 1 s keeps its TAI. */
TEST_F(St20TxRtpUserPacingTest, NonExactTaiStartingInsideOneSecondIsKept) {
  constexpr uint64_t kEpoch = kNowEpoch + 999;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, false);

    Run(tx_no_chain, {{100, kEpoch * kFrameNs}});

    EXPECT_EQ(sent_[0].epoch, kEpoch);
    ExpectStartsAt(0, DefaultStart(kEpoch));
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  }
}

/* With EXACT, frame 0 starts 0.37 TFRAME past epoch N, so its nearest epoch is N. Frame 1
 * has no TAI; default pacing would give it epoch N+1, TROFFSET (0.1 TFRAME) after it:
 * only 0.73 TFRAME after frame 0 started, while frame 0 is still on the wire. A frame
 * must not start before one frame period after the start of the frame before, so frame 1
 * takes N+2 and frame 2 the next epoch. */
TEST_F(St20TxRtpUserPacingTest, ExactFallbackFrameDoesNotOverlapThePreviousFrame) {
  constexpr uint64_t kTai0 = kNowTai + 370'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);

    ut_session::capture_stderr(
        [&] { Run(tx_no_chain, {{100, kTai0}, {200, 0}, {300, 0}}); });

    ExpectStartsAt(0, kTai0);
    EXPECT_GE(sent_[1].ptp, sent_[0].ptp + kFrameNs)
        << "frame 1 starts " << (sent_[0].ptp + kFrameNs - sent_[1].ptp)
        << " ns before frame 0 has had its frame period";
    ExpectStartsAt(1, DefaultStart(kNowEpoch + 2));
    ExpectStartsAt(2, DefaultStart(kNowEpoch + 3));
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 2u);
  }
}

/* The spacing is judged on the start after the 90 kHz snap. Frame 0's TAI is epoch N
 * + 100000 + 2 ns; TR_offset is 100003 ns, 3 ns off the tick grid (100000 is on it).
 * Epoch N+1's raw start, (N+1) * T + 100003, is 1 ns past kTai0 + T, so it would pass,
 * but the snap moves it 3 ns earlier onto the grid, 2 ns inside that frame time. */
TEST_F(St20TxRtpUserPacingTest, ExactFallbackFrameSpacingHoldsAfterTheSnap) {
  constexpr uint64_t kOffGridTrOffsetNs = kTrOffsetNs + 3;
  constexpr uint64_t kTai0 = kNowTai + kTrOffsetNs + 2;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);
    ut_txv_set_tr_offset(ctx_, kOffGridTrOffsetNs);
    ASSERT_LT(ut_txv_round_to_media_clk((kNowEpoch + 1) * kFrameNs + kOffGridTrOffsetNs,
                                        ST10_VIDEO_SAMPLING_RATE_90K),
              kTai0 + kFrameNs);

    ut_session::capture_stderr([&] { Run(tx_no_chain, {{100, kTai0}, {200, 0}}); });

    ExpectStartsAt(0, kTai0);
    EXPECT_GE(sent_[1].ptp, sent_[0].ptp + kFrameNs);
  }
}

/* Frame 0's next epoch is the last inside the onward range of 1 s of frames: the
 * spacing step takes frame 1 one epoch past that range, and frame 1 is kept there
 * rather than resynced, as frame 0's user start is held (less than 1 s ahead). */
TEST_F(St20TxRtpUserPacingTest, ExactFallbackFrameSteppedPastTheOnwardRangeIsKept) {
  constexpr uint64_t kOnwardEpochs = kOneSecondNs / kFrameNs;
  constexpr uint64_t kTai0 = kNowTai + (kOnwardEpochs - 1) * kFrameNs + 370'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);
    ut_txv_set_max_onward_epochs(ctx_, kOnwardEpochs);

    ut_session::capture_stderr([&] { Run(tx_no_chain, {{100, kTai0}, {200, 0}}); });

    ExpectStartsAt(0, kTai0);
    ExpectStartsAt(1, DefaultStart(kNowEpoch + kOnwardEpochs + 1));
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  }
}

/* An EXACT TAI 30 us under 1 s ahead is accepted and rounds to the epoch 1 s ahead, so
 * the next epoch is past the onward range. The ring is FIFO, so resyncing frame 1 to now
 * would send it after frame 0, unpaced and with its RTP timestamp about 1 s back. */
TEST_F(St20TxRtpUserPacingTest,
       ExactFallbackFrameAfterATaiNearOneSecondAheadKeepsItsSpacing) {
  constexpr uint64_t kOnwardEpochs = kOneSecondNs / kFrameNs;
  constexpr uint64_t kTai0 = kNowTai + kOneSecondNs - 30'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);
    ut_txv_set_max_onward_epochs(ctx_, kOnwardEpochs);

    ut_session::capture_stderr([&] { Run(tx_no_chain, {{100, kTai0}, {200, 0}}); });

    ExpectStartsAt(0, kTai0);
    EXPECT_GE(sent_[1].ptp, sent_[0].ptp + kFrameNs);
    ExpectStartsAt(1, DefaultStart(kNowEpoch + kOnwardEpochs + 1));
  }
}

/* Without EXACT a start kept just under 1 s ahead holds the frames after it: the ring
 * is FIFO, so resyncing one of them to now would send it after that frame, unpaced and
 * with its RTP timestamp about 1 s back. ST22 runs the same path, and its frames have
 * few packets, so several frames can sit in the ring behind the held one. */
TEST_F(St20TxRtpUserPacingTest, NonExactStartNearOneSecondAheadHoldsTheFramesAfterIt) {
  constexpr uint64_t kOnwardEpochs = kOneSecondNs / kFrameNs;
  constexpr uint64_t kEpoch = kNowEpoch + kOnwardEpochs - 1;
  for (bool st22 : {false, true}) {
    for (bool tx_no_chain : {true, false}) {
      SCOPED_TRACE(st22 ? "ST22" : "ST20");
      SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
      Reset();
      ut_txv_set_st22_rtp_level(ctx_, st22);
      SetFlags(true, false);
      ut_txv_set_max_onward_epochs(ctx_, kOnwardEpochs);

      ut_session::capture_stderr([&] {
        Run(tx_no_chain, {{100, kEpoch * kFrameNs}, {200, 0}, {300, 0}, {400, 0}});
      });

      for (int i = 0; i < 4; i++) {
        SCOPED_TRACE("frame " + std::to_string(i));
        ExpectStartsAt(i, DefaultStart(kEpoch + i));
      }
      EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
    }
  }
}

/* The same holds after an EXACT start 30 us under 1 s ahead: the spacing step moves only
 * the first frame after it, and the second must not resync to now either. */
TEST_F(St20TxRtpUserPacingTest, ExactStartNearOneSecondAheadHoldsTheSecondFrameAfterIt) {
  constexpr uint64_t kOnwardEpochs = kOneSecondNs / kFrameNs;
  constexpr uint64_t kTai0 = kNowTai + kOneSecondNs - 30'000;
  for (bool tx_no_chain : {true, false}) {
    SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
    Reset();
    SetFlags(true, true);
    ut_txv_set_max_onward_epochs(ctx_, kOnwardEpochs);

    ut_session::capture_stderr(
        [&] { Run(tx_no_chain, {{100, kTai0}, {200, 0}, {300, 0}}); });

    ExpectStartsAt(0, kTai0);
    ExpectStartsAt(1, DefaultStart(kNowEpoch + kOnwardEpochs + 1));
    ExpectStartsAt(2, DefaultStart(kNowEpoch + kOnwardEpochs + 2));
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
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
