/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the interlaced field/frame-grid schedule of an ST 2110-20 TX session
 * (`tv_sync_pacing()` in st_tx_video_session.c): pacing->frame_time is one FIELD
 * period, so the ST 2110-21:2022 6.2 frame grid is every second epoch slot -- an
 * even slot carries a first field, an odd one a second. Also pins that MTL adds
 * no further T_LINE/2 term to second fields. Reasoning in doc/design.md 6.6.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest \
 *          --gtest_filter='St20TxInterlacedFieldEpochTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {
/* 1080i50: ops.fps is the field rate, so one epoch slot is one 20ms field and
 * the ST 2110-21 frame grid (T_Frame) is every second slot. */
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFieldPeriodNs = 20 * kNanosecondsPerMillisecond;
constexpr uint64_t kFrameGridNs = 2 * kFieldPeriodNs;
constexpr uint32_t kMaxOnwardEpochs = 3;
/* TROFFSET for 1125-line interlaced: 22/1125 of T_Frame per ST 2110-21. */
constexpr uint64_t kTrOffsetNs = 782222;
constexpr uint32_t kVirtualReceiverBufferPackets = 0;
constexpr uint64_t kPacketIntervalNs = 0;
constexpr uint32_t kSamplingClockRate = ST10_VIDEO_SAMPLING_RATE_90K;

constexpr uint64_t kEvenSlot = 2000;
constexpr uint64_t kOddSlot = 2001;
constexpr uint64_t kLateOddSlot = 2009;
constexpr uint64_t kOnwardOddSlot = 2005;
constexpr uint64_t kOnwardStaleEpoch = 2010;
constexpr uint64_t kTotalLines1125 = 1125;
constexpr uint64_t kTotalLines625 = 625;
constexpr uint64_t kTotalLines525 = 525;
constexpr uint32_t kHeight1080 = 1080;
constexpr uint32_t kHeight576 = 576;
constexpr uint32_t kHeight480 = 480;
}  // namespace

class St20TxInterlacedFieldEpochTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ResetSession();
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  /* Fresh session with the 1080i50 geometry above. Called again between the
   * branches of a multi-branch case so each starts from zeroed stats. */
  void ResetSession() {
    ut_txv_destroy(ctx_);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFieldPeriodNs);
    ut_txv_set_max_onward_epochs(ctx_, kMaxOnwardEpochs);
    ut_txv_set_tr_offset(ctx_, kTrOffsetNs);
    ut_txv_set_vrx(ctx_, kVirtualReceiverBufferPackets);
    ut_txv_set_trs(ctx_, kPacketIntervalNs);
    ut_txv_set_sampling_clock_rate(ctx_, kSamplingClockRate);
    ut_txv_set_interlaced(ctx_, true);
  }
  /* Runs one frame through tvs_tasklet_handler(), including production's own
   * s->second_field bookkeeping, at the given PTP instant. */
  void RunFieldAt(uint64_t ptp_ns, uint64_t timestamp = 0) {
    ut_txv_set_mock_ptp_time(ctx_, ptp_ns);
    ut_txv_set_mock_tsc_time(ctx_, ptp_ns);
    ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, timestamp,
                                       &packet_tsc_, &packet_ptp_),
              0);
    ASSERT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  }
  void ExpectNoResyncStats() {
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
    EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  }
  ut_txv_ctx* ctx_ = nullptr;
  uint64_t packet_tsc_ = 0;
  uint64_t packet_ptp_ = 0;
};

/* An odd candidate slot carries a second field, so a first field must wait for
 * the next frame grid point instead of starting half a frame early. */
TEST_F(St20TxInterlacedFieldEpochTest, FirstFieldOnOddSlotDefersToFrameGrid) {
  ut_txv_set_cur_epochs(ctx_, kEvenSlot);

  RunFieldAt(kOddSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kOddSlot + 1);
  EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
  ExpectNoResyncStats();
}

/* The mirror image: the rule is parity-matching, not "always even". */
TEST_F(St20TxInterlacedFieldEpochTest, SecondFieldOnEvenSlotDefersToFrameGrid) {
  ut_txv_set_cur_epochs(ctx_, kEvenSlot - 1);
  ut_txv_set_second_field(ctx_, true);

  RunFieldAt(kEvenSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kEvenSlot + 1);
  EXPECT_TRUE(ut_txv_notify_frame_done_second_field(ctx_));
  ExpectNoResyncStats();
}

/* A gratuitous +1 on an already-aligned field would add a whole field period of
 * latency. */
TEST_F(St20TxInterlacedFieldEpochTest, AlreadyAlignedFieldIsNotDeferred) {
  ut_txv_set_cur_epochs(ctx_, kEvenSlot - 1);

  RunFieldAt(kEvenSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kEvenSlot);
  EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
  ExpectNoResyncStats();
}

/* Once aligned, fields keep advancing one slot per run with alternating parity
 * and the alignment never fires again -- no drift, no stall. */
TEST_F(St20TxInterlacedFieldEpochTest, SteadyStateFieldsAlternateWithoutCorrection) {
  constexpr uint64_t kFirstSlot = kEvenSlot;
  ut_txv_set_cur_epochs(ctx_, kFirstSlot - 1);

  for (uint64_t i = 0; i < 4; i++) {
    RunFieldAt((kFirstSlot - 1 + i) * kFieldPeriodNs);
    EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kFirstSlot + i);
    EXPECT_EQ(ut_txv_notify_frame_done_second_field(ctx_), (i % 2) == 1);
  }
  ExpectNoResyncStats();
}

/* Three branches abandon the steady-state slot and resync to real time, which can
 * land on the wrong parity. Each must restore it without disturbing its own
 * accounting, which is computed from the pre-alignment candidate. */
TEST_F(St20TxInterlacedFieldEpochTest, ResyncToRealTimeRestoresFieldParity) {
  {
    SCOPED_TRACE("late: no ready frame from the app");
    constexpr uint64_t kSkippedSlots = kLateOddSlot - (kEvenSlot + 1);
    ut_txv_set_cur_epochs(ctx_, kEvenSlot);

    RunFieldAt(kLateOddSlot * kFieldPeriodNs);

    EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kLateOddSlot + 1);
    EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
    EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), kSkippedSlots);
    EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 1);
    EXPECT_EQ(ut_txv_notify_late_last_delta(ctx_), kSkippedSlots);
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  }

  ResetSession();
  {
    SCOPED_TRACE("wraparound: cur_epochs == UINT64_MAX, its own early exit");
    ut_txv_set_cur_epochs(ctx_, UINT64_MAX);

    RunFieldAt(kOddSlot * kFieldPeriodNs);

    EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kOddSlot + 1);
    EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
    ExpectNoResyncStats();
  }

  ResetSession();
  {
    SCOPED_TRACE("onward: PTP stepped back past max_onward_epochs");
    constexpr uint64_t kOnwardGap = kOnwardStaleEpoch + 1 - kOnwardOddSlot;
    ut_txv_set_cur_epochs(ctx_, kOnwardStaleEpoch);
    ASSERT_GT(kOnwardGap, kMaxOnwardEpochs);

    RunFieldAt(kOnwardOddSlot * kFieldPeriodNs);

    EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kOnwardOddSlot + 1);
    EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), kOnwardGap);
    EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
    EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  }
}

/* An ST20 RTP-level session carries its F bit where struct st20_rfc4175_rtp_hdr
 * reads it and MTL still owns the pacing, so it gets the same alignment as frame
 * level. Driven through the real builders, for both parities, so the F-bit
 * extraction feeding tv_sync_pacing() is itself covered. */
TEST_F(St20TxInterlacedFieldEpochTest, St20RtpLevelSessionIsAlignedFromTheFBit) {
  for (bool tx_no_chain : {true, false}) {
    for (bool second_field : {false, true}) {
      SCOPED_TRACE(tx_no_chain ? "tv_build_rtp" : "tv_build_rtp_chain");
      SCOPED_TRACE(second_field ? "second field" : "first field");
      /* the candidate slot is the wrong parity for the field the F bit names */
      const uint64_t candidate = second_field ? kEvenSlot : kOddSlot;
      uint64_t epoch = 0;
      ut_txv_set_mock_ptp_time(ctx_, candidate * kFieldPeriodNs);
      ut_txv_set_mock_tsc_time(ctx_, candidate * kFieldPeriodNs);
      ut_txv_set_cur_epochs(ctx_, candidate - 1);

      ASSERT_EQ(ut_txv_run_rtp_tasklet(ctx_, second_field, tx_no_chain, &epoch), 0);

      EXPECT_EQ(epoch, candidate + 1);
      ExpectNoResyncStats();
    }
  }
}

/* st22_tx_create() maps an ST22 RTP-level session onto this same path, but its
 * 16-byte rfc9134 header puts the first codestream byte where
 * struct st20_rfc4175_rtp_hdr expects the F bit -- and JPEG-XS opens with SOC
 * 0xFF10, whose MSB aliases ST20_SECOND_FIELD. That parity is unreadable, so it
 * must not move the schedule. Contrasted against the ST20 shape through the same
 * driver, so this cannot pass merely because nothing was ever aligned. */
TEST_F(St20TxInterlacedFieldEpochTest, St22RtpLevelSessionIsNotAligned) {
  constexpr bool kTxNoChain = true;
  uint64_t epoch = 0;
  ut_txv_set_mock_ptp_time(ctx_, kEvenSlot * kFieldPeriodNs);
  ut_txv_set_mock_tsc_time(ctx_, kEvenSlot * kFieldPeriodNs);

  /* an F bit reading "second field" on an even candidate slot: wrong parity */
  ut_txv_set_cur_epochs(ctx_, kEvenSlot - 1);
  ASSERT_EQ(ut_txv_run_rtp_tasklet(ctx_, /*second_field=*/true, kTxNoChain, &epoch), 0);
  ASSERT_EQ(epoch, kEvenSlot + 1);

  ut_txv_set_st22_rtp_level(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kEvenSlot - 1);
  ASSERT_EQ(ut_txv_run_rtp_tasklet(ctx_, /*second_field=*/true, kTxNoChain, &epoch), 0);

  EXPECT_EQ(epoch, kEvenSlot);
  ExpectNoResyncStats();
}

/* Progressive sessions -- the overwhelming majority -- have no field parity to
 * honour and must keep the bare epoch arithmetic. */
TEST_F(St20TxInterlacedFieldEpochTest, ProgressiveSessionEpochIsUnchanged) {
  ut_txv_set_interlaced(ctx_, false);
  ut_txv_set_cur_epochs(ctx_, kEvenSlot);

  RunFieldAt(kOddSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kOddSlot);
  ExpectNoResyncStats();
}

/* ST20_TX_FLAG_USER_PACING promises a send time "aligned with virtual receiver
 * read schedule" (st20_api.h), which is the frame grid: the app names the window
 * and MTL still quantizes inside it, parity correction and TROFFSET included. */
TEST_F(St20TxInterlacedFieldEpochTest, UserPacedFieldIsAlignedToFrameGrid) {
  constexpr uint64_t kAppSlot = kEvenSlot + 4;
  constexpr uint64_t kFrameCount = kAppSlot / 2;
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_second_field(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kEvenSlot);

  /* names an EVEN slot, i.e. the wrong parity for the second field being sent,
   * and one that neither real time nor the steady-state path would pick */
  RunFieldAt(kEvenSlot * kFieldPeriodNs, kAppSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kAppSlot + 1);
  EXPECT_TRUE(ut_txv_notify_frame_done_second_field(ctx_));
  /* T_VD for the second field of frame kFrameCount, TROFFSET included */
  const long double t_vd =
      (long double)kFrameCount * kFrameGridNs + kFrameGridNs / 2 + kTrOffsetNs;
  EXPECT_EQ(packet_ptp_, ut_txv_round_to_media_clk((uint64_t)t_vd, kSamplingClockRate));
  ExpectNoResyncStats();
}

/* EXACT_USER_PACING bypasses transmission_start_time() and transmits at the
 * supplied instant, so it must bypass the parity correction too. */
TEST_F(St20TxInterlacedFieldEpochTest, ExactUserPacedFieldIsNotRealigned) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_second_field(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kEvenSlot);

  /* names the EVEN slot, i.e. the wrong parity for the second field being sent */
  RunFieldAt(kEvenSlot * kFieldPeriodNs, kEvenSlot * kFieldPeriodNs);

  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kEvenSlot);
  EXPECT_TRUE(ut_txv_notify_frame_done_second_field(ctx_));
  /* verbatim: no TROFFSET, no media-clock snap */
  EXPECT_EQ(packet_ptp_, kEvenSlot * kFieldPeriodNs);
  ExpectNoResyncStats();
}

/* The interlaced TROFFSET numerators are ST 2110-21:2017 fixed values and the
 * 2022 Table 1 equation is defective for the 525/625 rows, so these are pinned
 * against a future "correction" to that formula. Expectations are built from the
 * FRAME period and Table 1's line totals, independently of how the library
 * factors the division. */
TEST_F(St20TxInterlacedFieldEpochTest, InitPacingDerivesInterlacedTrOffset) {
  const long double t_frame = kFrameGridNs;

  ASSERT_EQ(ut_txv_init_pacing(ctx_, kHeight1080, true, ST_FPS_P50), 0);
  EXPECT_DOUBLE_EQ((double)ut_txv_pacing_tr_offset(ctx_),
                   (double)(t_frame * 22 / kTotalLines1125));

  ASSERT_EQ(ut_txv_init_pacing(ctx_, kHeight576, true, ST_FPS_P50), 0);
  EXPECT_DOUBLE_EQ((double)ut_txv_pacing_tr_offset(ctx_),
                   (double)(t_frame * 26 / kTotalLines625));

  ASSERT_EQ(ut_txv_init_pacing(ctx_, kHeight480, true, ST_FPS_P50), 0);
  EXPECT_DOUBLE_EQ((double)ut_txv_pacing_tr_offset(ctx_),
                   (double)(t_frame * 20 / kTotalLines525));
}

/* ST 2110-10 7.6.1 SHALLs the second field's RTP timestamp be offset from the
 * first by exactly half the frame period, so at 1080i50 every field is 1800 ticks
 * after the last. MTL derives that timestamp from the transmission start, so any
 * parity-dependent schedule term -- ST 2110-21 6.3.3's T_LINE/2 above all --
 * would reach it and break the cadence; a constant term such as TROFFSET is fine.
 * Per 7.6.1 NOTE 1 a single uniform increment is only valid at integer rates, and
 * the fixture is P50 where 1800 ticks is exact. See doc/design.md 6.6. */
TEST_F(St20TxInterlacedFieldEpochTest, InterlacedRtpTimestampCadenceIsUniform) {
  constexpr uint64_t kFirstSlot = kEvenSlot;
  constexpr uint32_t kTicksPerField =
      kFieldPeriodNs * kSamplingClockRate / kNanosecondsPerSecond;
  uint32_t rtp_timestamps[4] = {0};
  ut_txv_set_cur_epochs(ctx_, kFirstSlot - 1);

  for (uint64_t i = 0; i < 4; i++) {
    RunFieldAt((kFirstSlot - 1 + i) * kFieldPeriodNs);
    ASSERT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kFirstSlot + i);
    rtp_timestamps[i] = ut_txv_notify_frame_done_rtp_timestamp(ctx_);
  }

  for (uint64_t i = 1; i < 4; i++) {
    EXPECT_EQ(rtp_timestamps[i] - rtp_timestamps[i - 1], kTicksPerField)
        << "field " << i << " breaks the uniform RTP timestamp cadence";
  }
}

/* An app may legitimately pin second_field, and an RTP-level app may never set
 * ST20_SECOND_FIELD at all. Then every frame defers, the session advances two
 * slots per frame and transmits at half the configured rate -- the only schedule
 * that keeps every frame on the frame grid. tv_stat()'s per-session fps and
 * "interlace first field N second field M" lines are what diagnose it. */
TEST_F(St20TxInterlacedFieldEpochTest, PinnedFieldParityHalvesRate) {
  constexpr uint64_t kFirstSlot = kEvenSlot;
  ut_txv_set_cur_epochs(ctx_, kFirstSlot - 1);

  for (uint64_t i = 0; i < 3; i++) {
    ut_txv_set_second_field(ctx_, false); /* app always claims a first field */
    RunFieldAt((kFirstSlot - 1 + i) * kFieldPeriodNs);
    EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kFirstSlot + 2 * i);
    EXPECT_FALSE(ut_txv_notify_frame_done_second_field(ctx_));
  }
  ExpectNoResyncStats();
}
