/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the RL warm-up compensation of the RTP timestamp in
 * tv_update_rtp_time_stamp(): a frame whose first real packet is handed to the
 * rate limiter from behind a warm-up pad train leaves the wire before
 * pacing->ptp_time_cursor, so the RTP timestamp derived from that cursor must be
 * shifted back or the receiver measures a negative packet-time-vs-RTP-time. See
 * tv_rl_rtp_shift_ticks() for why the shift is what it is.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlRtpShiftTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerSecond = 1000 * 1000 * 1000;
constexpr uint32_t kVideoClockHz = ST10_VIDEO_SAMPLING_RATE_90K;
/* A real TAI instant, exactly on a 90kHz tick, so the 32-bit RTP timestamp is a
 * wrapped value and the shift has to be read back modulo 2^32 the way a
 * receiver does. */
constexpr uint64_t kCursorTai = 1775000000000000000ULL;
constexpr uint64_t kFramePeriodNs = 1000 * 1000;
constexpr uint64_t kCursorEpoch = kCursorTai / kFramePeriodNs;
/* Mirrors RL_RTP_SHIFT_PKTS, so changing the library constant fails this suite. */
constexpr long double kShiftPkts = 5.0L;
/* The rate limiter releases the pad train early: it starts draining the pads
 * holding a full max_burst_size token bucket, and overruns further where
 * pad_interval is small. That bias measures up to 1.99 packet intervals on RL
 * hardware across the rx_timing formats, so the shift has to cover at least that
 * much for the receiver's latency to stay non-negative. */
constexpr long double kBurstCreditTrs = 2.0L;
/* A 90kHz tick is 100000/9 ns, so a cursor's sub-tick position -- the only thing
 * the tick conversion rounds away -- repeats every 100000ns. Sweeping one whole
 * period at 1ns resolution therefore visits every rounding residue there is. */
constexpr uint64_t kTickResiduePeriodNs = 100000;
/* 1080p60: trs 3888.21ns, the pad train is capped at 128 packets. */
constexpr long double kTrs1080p60 = 3888.21L;
constexpr uint32_t kWarmPkts = 128;
/* 45 whole 90kHz ticks of lead over the epoch, which no format's five intervals
 * can exhaust, so only the tests about the cap have to reason about it. The tests
 * that go through tv_sync_pacing() pass it as tr_offset with vrx left at zero,
 * where it is the same quantity. */
constexpr uint64_t kWideLeadNs = 500000;
constexpr uint64_t kWideLeadCursorTai = kCursorTai + kWideLeadNs;
/* Six tenths of a 90kHz tick: inside 1080p60's two-tick worth of five packet
 * intervals, so it is the lead and not that amount which caps the shift. */
constexpr uint64_t kSubCapLeadNs = 6667;

/* An independent model of st_muldiv_u64_round_closest(), ties included, so that the
 * expected tick counts below are not derived by the code under test. */
uint64_t roundClosest(uint64_t value, uint64_t multiplier, uint64_t divisor) {
  __uint128_t product = static_cast<__uint128_t>(value) * multiplier;
  __uint128_t quotient = product / divisor;
  __uint128_t remainder = product % divisor;
  if (remainder > divisor / 2) quotient++;
  return static_cast<uint64_t>(quotient);
}

/* The 32-bit media-clock image of a TAI instant, the way both the session and a
 * receiver derive it. */
uint32_t tickOf(uint64_t tai) {
  return static_cast<uint32_t>(roundClosest(tai, kVideoClockHz, kNanosecondsPerSecond));
}

uint32_t toTicks(long double ns) {
  return static_cast<uint32_t>(
      roundClosest(static_cast<uint64_t>(ns), kVideoClockHz, kNanosecondsPerSecond));
}

/* Media-clock ticks between the scheduled launch instant and the RTP timestamp
 * the session derived from it, read as the 32-bit difference so a timestamp that
 * wrapped mid-sweep still resolves. */
uint32_t shiftTicks(uint64_t cursor_tai, uint32_t rtp_ts) {
  return tickOf(cursor_tai) - rtp_ts;
}

/* What tv_rl_rtp_shift_ticks() owes a format whose lead is wide enough not to cap
 * it: kShiftPkts intervals as whole ticks, floored at one. */
uint32_t expectedShiftTicks(long double trs) {
  uint32_t ticks = toTicks(kShiftPkts * trs);
  return ticks ? ticks : 1;
}
}  // namespace

class St20TxRlRtpShiftTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_sampling_clock_rate(ctx_, kVideoClockHz);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  /* Media-clock ticks the session moved the RTP timestamp back from a launch
   * instant sitting a given lead past its epoch. */
  uint32_t measureShiftTicksAtLead(long double trs, uint32_t warm_pkts,
                                   long double lead_ns, uint64_t timestamp = 0) {
    uint64_t cursor = kCursorTai + static_cast<int64_t>(lead_ns);
    ut_txv_set_frame_time(ctx_, kFramePeriodNs);
    ut_txv_set_cur_epochs(ctx_, kCursorEpoch);
    ut_txv_set_ptp_time_cursor(ctx_, cursor);
    ut_txv_set_trs(ctx_, trs);
    ut_txv_set_warm_pkts(ctx_, warm_pkts);
    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, timestamp);
    return shiftTicks(cursor, ut_txv_rtp_time_stamp(ctx_));
  }

  uint32_t measureShiftTicks(long double trs, uint32_t warm_pkts,
                             uint64_t timestamp = 0) {
    return measureShiftTicksAtLead(trs, warm_pkts, kWideLeadNs, timestamp);
  }

  uint64_t measureShiftNs(long double trs, uint32_t warm_pkts) {
    return st10_media_clk_to_ns(measureShiftTicks(trs, warm_pkts), kVideoClockHz);
  }

  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxRlRtpShiftTest, NoPadTrainLeavesTheRtpTimestampOnTheLaunchInstant) {
  /* warm_pkts 0 is every non-RL session plus ST22 and wide pacing: nothing
   * precedes the frame in the queue, so the transmitter itself releases the
   * first packet at the launch instant and the timestamp must not move. */
  EXPECT_EQ(measureShiftTicks(kTrs1080p60, 0), 0u);
}

TEST_F(St20TxRlRtpShiftTest, ShiftCoversTheBurstCreditOnEveryNightlyFormat) {
  /* trs = frame_time * reactive / packets_per_frame for the formats the nightly
   * rx_timing suite covers, widest to narrowest frame period. */
  struct {
    const char* name;
    long double trs;
  } formats[] = {
      {"1080p25", 9331.70L},    {"1080p30", 7776.42L},  {"1080p50", 4665.85L},
      {"1080p60", kTrs1080p60}, {"1080p100", 2332.93L}, {"1080p120", 1944.11L},
      {"2160p60", 972.17L},
  };

  for (const auto& format : formats) {
    EXPECT_EQ(measureShiftTicks(format.trs, kWarmPkts), expectedShiftTicks(format.trs))
        << format.name;
    /* The amount above is a choice; that it clears the measured early release is
     * the reason for the choice, so assert that separately. Below trs 1111ns five
     * intervals round to no ticks at all and the one-tick floor answers instead --
     * 2160p60 is the only format here, where a tick buys 11.4 intervals. */
    EXPECT_GE(measureShiftNs(format.trs, kWarmPkts),
              static_cast<uint64_t>(kBurstCreditTrs * format.trs))
        << format.name;
  }
}

TEST_F(St20TxRlRtpShiftTest, TheLeadOverTheEpochCapsTheShift) {
  /* The shift is spent out of rtp_offset, the RTP timestamp's lead over the
   * epoch, so it can never exceed that lead; beyond it the receiver trades the
   * negative latency for "rtp_offset exceed min" on every frame. A lead of only a
   * handful of packet intervals is reachable two ways: tv_init_pacing() forces
   * warm_pkts to 8 for height <= 576 however few packets tr_offset holds, and
   * EXACT_USER_PACING launches wherever the app asked inside the frame period. */
  struct {
    const char* name;
    long double trs;
    long double lead_ns;
    bool capped;
  } rows[] = {
      /* 480x270p30 under RL: tr_offset 1244444ns holds 10.03 intervals of
       * 124031ns and vrx spends 8 of them, leaving 23 ticks against five
       * un-capped intervals of 56. The tightest lead any session reaches, and
       * reachable at all only because tv_init_pacing() forces warm_pkts to 8
       * for height <= 576 however little tr_offset holds; its value does not
       * otherwise reach the helper, which only tests it for zero. */
      {"480x270p30", 124031.0L, 1244444.0L - 8 * 124031.0L, true},
      {"1080p60, two-interval lead", kTrs1080p60, 2.0L * kTrs1080p60, true},
      {"1080p60, ordinary lead", kTrs1080p60, kWideLeadNs, false},
  };

  for (const auto& row : rows) {
    uint32_t lead = toTicks(row.lead_ns);
    uint32_t shift = measureShiftTicksAtLead(row.trs, kWarmPkts, row.lead_ns);
    EXPECT_LE(shift, lead) << row.name << ", lead " << lead << " ticks";
    if (row.capped) {
      EXPECT_LT(shift, expectedShiftTicks(row.trs))
          << row.name << ": the lead was supposed to cap the shift";
    } else {
      EXPECT_EQ(shift, expectedShiftTicks(row.trs)) << row.name;
    }
  }
}

TEST_F(St20TxRlRtpShiftTest, ALaunchInstantOnOrBeforeItsEpochIsNotShiftedAtAll) {
  /* A vrx exceeding the packets tr_offset holds puts the launch instant on or
   * ahead of its epoch, and so does an app asking to launch exactly on one. There
   * is no lead left to spend and shifting would only push rtp_offset further
   * below its min. */
  EXPECT_EQ(measureShiftTicksAtLead(kTrs1080p60, kWarmPkts, 0.0L), 0u);
  EXPECT_EQ(measureShiftTicksAtLead(kTrs1080p60, kWarmPkts, -4.0L * kTrs1080p60), 0u);
}

TEST_F(St20TxRlRtpShiftTest, ShiftIsTheSameWholeTickCountAtEverySubTickCursorPosition) {
  /* The receiver checks the RTP timestamp delta between consecutive frames
   * against an exact tick count, so a shift that varied with where the launch
   * instant fell inside a tick would emit one too-large delta followed by one
   * too-small one. This is what the tick-domain subtraction buys, and an
   * equivalent ns amount before the conversion does not: five 1080p60 intervals
   * are 1.75 ticks, so an ns-domain shift collapses them to one tick on the
   * quarter of these positions where that rounds down instead of up. */
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);
  ut_txv_set_frame_time(ctx_, kFramePeriodNs);
  /* A whole frame period of lead, so the sweep never runs into the cap. */
  ut_txv_set_cur_epochs(ctx_, kCursorEpoch - 1);

  uint32_t expected = 0;
  for (uint64_t residue = 0; residue < kTickResiduePeriodNs; residue++) {
    ut_txv_set_ptp_time_cursor(ctx_, kCursorTai + residue);
    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    uint32_t shift = shiftTicks(kCursorTai + residue, ut_txv_rtp_time_stamp(ctx_));

    if (!residue) {
      expected = shift;
      ASSERT_EQ(expected, expectedShiftTicks(kTrs1080p60));
      continue;
    }
    if (shift != expected) {
      ADD_FAILURE() << "cursor " << residue << "ns past a tick shifted the RTP "
                    << "timestamp by " << shift << " ticks, not " << expected;
      break;
    }
  }
}

TEST_F(St20TxRlRtpShiftTest, ALeadCappedShiftNeverReachesPastTheEpochsOwnTick) {
  /* The epoch's own sub-tick position is arbitrary, whatever the frame rate:
   * tai_from_frame_count() truncates cur_epochs * frame_time to whole nanoseconds
   * and a 90kHz tick is 100000/9 of them, so the remainder walks the whole tick as
   * a session runs. Where the lead is what caps the shift, measuring that lead as
   * an ns difference and converting it once rounds the timestamp a whole tick
   * *past* the epoch on every epoch whose remainder is over half a tick -- 40% of
   * them at the lead below, where the receiver then reads an rtp_offset under its
   * floor. Sweeping that position is the only way to see it: the cursor sweep above
   * holds the lead above the cap, where the shift is a constant. */
  ASSERT_LT(toTicks(kSubCapLeadNs), expectedShiftTicks(kTrs1080p60))
      << "the lead no longer caps the shift, so this sweep proves nothing";
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);
  /* One epoch elapsed makes frame_time the epoch itself, which is how an arbitrary
   * sub-tick epoch position is driven from out here. */
  ut_txv_set_cur_epochs(ctx_, 1);

  for (uint64_t residue = 0; residue < kTickResiduePeriodNs; residue++) {
    const uint64_t epoch = kCursorTai + residue;
    const uint64_t cursor = epoch + kSubCapLeadNs;
    ut_txv_set_frame_time(ctx_, epoch);
    ut_txv_set_ptp_time_cursor(ctx_, cursor);
    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);

    uint32_t shift = shiftTicks(cursor, ut_txv_rtp_time_stamp(ctx_));
    uint32_t lead = tickOf(cursor) - tickOf(epoch);
    if (shift > lead) {
      ADD_FAILURE() << "an epoch " << residue << "ns past a tick shifted the RTP "
                    << "timestamp by " << shift << " ticks out of a lead of " << lead;
      break;
    }
  }
}

TEST_F(St20TxRlRtpShiftTest, ANegativeTimestampDeltaSpendsItsOwnLeadNotTheCursors) {
  /* rtp_timestamp_delta_us moves the RTP timestamp off the launch instant without
   * moving the schedule, so the lead the shift is spent out of belongs to the
   * delta'd instant and not to the cursor. Reading the cap off the cursor instead
   * shifts the timestamp below the epoch -- "rtp_offset exceed min" on every frame
   * of a session that had been compliant precisely because the app chose that
   * delta. A delta large enough to precede the epoch by itself is the app's own
   * doing, hence the floor below is the lower of the two; the shift's business is
   * only never to add to it. Where the crossing falls depends on the delta, the
   * lead and trs together, so it is swept. */
  const uint64_t lead_ns = 25000; /* 2.25 ticks, so any negative delta binds the cap */
  const uint32_t epoch_tick = tickOf(kCursorTai);
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);
  ut_txv_set_frame_time(ctx_, kFramePeriodNs);
  ut_txv_set_cur_epochs(ctx_, kCursorEpoch);
  ut_txv_set_ptp_time_cursor(ctx_, kCursorTai + lead_ns);

  for (int32_t delta_us = 0; delta_us >= -50; delta_us--) {
    const uint32_t delta_tick = tickOf(static_cast<uint64_t>(
        static_cast<int64_t>(kCursorTai + lead_ns) + delta_us * 1000));
    const uint32_t floor_tick =
        static_cast<int32_t>(delta_tick - epoch_tick) < 0 ? delta_tick : epoch_tick;
    ut_txv_set_rtp_timestamp_delta_us(ctx_, delta_us);
    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);

    int32_t below = static_cast<int32_t>(floor_tick - ut_txv_rtp_time_stamp(ctx_));
    if (below > 0) {
      ADD_FAILURE() << "a delta of " << delta_us << "us shifted the RTP timestamp "
                    << below << " ticks below the earlier of its epoch and itself";
      break;
    }
  }
}

TEST_F(St20TxRlRtpShiftTest, FrameTaskletReportsAnUnshiftedInstantWithAShiftedTimestamp) {
  /* frame->timestamp and frame->rtp_timestamp deliberately stop being each
   * other's st10_tai_to_media_clk() image once a pad train precedes the frame:
   * the reported instant stays the launch instant, because that is the schedule
   * the library resolved, while only the value that goes in the RTP header is
   * compensated. Collapsing the two either way is a regression. */
  ut_txv_set_frame_time(ctx_, kFramePeriodNs);
  ut_txv_set_max_onward_epochs(ctx_, 3);
  ut_txv_set_cur_epochs(ctx_, kCursorEpoch - 1);
  ut_txv_set_mock_ptp_time(ctx_, kCursorTai);
  ut_txv_set_mock_tsc_time(ctx_, kFramePeriodNs / 2);
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);
  ut_txv_set_tr_offset(ctx_, kWideLeadNs);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  ASSERT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1)
      << "the frame has to have completed for its reported fields to be the "
         "ones the app sees";
  uint64_t reported_tai = ut_txv_notify_frame_done_timestamp(ctx_);
  uint32_t reported_rtp_ts = ut_txv_notify_frame_done_rtp_timestamp(ctx_);
  /* vrx is zero here, so the launch instant is the epoch plus tr_offset -- which
   * is where the first packet was targeted. */
  EXPECT_EQ(packet_ptp, kWideLeadCursorTai);
  EXPECT_EQ(reported_tai, packet_ptp);
  EXPECT_EQ(shiftTicks(reported_tai, reported_rtp_ts), expectedShiftTicks(kTrs1080p60));
}

TEST_F(St20TxRlRtpShiftTest, ExactUserPacingIsShiftedOnItsUnsnappedCursor) {
  /* EXACT_USER_PACING is exempt from tv_sync_pacing()'s media-clock snap, so its
   * launch instant keeps an arbitrary sub-tick position -- and it still queues
   * the frame behind the pad train, so it still needs the compensation. Its lead
   * over now has to clear the whole pad train, or tv_pacing_required_tai() rejects
   * the timestamp and the frame falls back to ordinary epoch pacing. */
  /* An epoch plus a bit over two ticks: off a 90kHz tick, so the cursor really
   * does keep a sub-tick position, and a whole frame period past now. */
  const uint64_t required_tai = kCursorTai + kFramePeriodNs + 29098;
  ut_txv_set_frame_time(ctx_, kFramePeriodNs);
  ut_txv_set_max_onward_epochs(ctx_, 3);
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, kCursorTai);
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);

  ASSERT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, required_tai),
            required_tai)
      << "the lead no longer clears warm_pkts * trs, so this covers plain pacing";
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0);
  ASSERT_EQ(static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_)), required_tai)
      << "the snap was applied, so this no longer covers the unsnapped path";
  ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);

  EXPECT_EQ(shiftTicks(required_tai, ut_txv_rtp_time_stamp(ctx_)),
            expectedShiftTicks(kTrs1080p60));
}

TEST_F(St20TxRlRtpShiftTest, ExactUserPacingOnItsEpochHasNoLeadToSpend) {
  /* EXACT_USER_PACING sets ptp_time_cursor to the app's instant verbatim, and
   * calc_frame_count_since_epoch() then picks the epoch *nearest* that instant,
   * so an app launching exactly on an epoch has no lead at all -- unlike the
   * ordinary path, where tr_offset less vrx * trs always buys some. tr_offset is
   * therefore not the bound: read from it, the shift here would be two ticks and
   * every frame would report an rtp_offset below its -1 floor. */
  const uint64_t required_tai = kCursorTai + kFramePeriodNs; /* exactly an epoch */
  ut_txv_set_frame_time(ctx_, kFramePeriodNs);
  ut_txv_set_max_onward_epochs(ctx_, 3);
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, kCursorTai);
  ut_txv_set_trs(ctx_, kTrs1080p60);
  ut_txv_set_warm_pkts(ctx_, kWarmPkts);
  ut_txv_set_tr_offset(ctx_, kWideLeadNs);

  ASSERT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, required_tai),
            required_tai);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0);
  ASSERT_EQ(static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_)), required_tai);
  ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);

  EXPECT_EQ(shiftTicks(required_tai, ut_txv_rtp_time_stamp(ctx_)), 0u);
}

TEST_F(St20TxRlRtpShiftTest, UserTimestampIsNotShifted) {
  /* ST20_TX_FLAG_USER_TIMESTAMP hands the RTP timestamp to the app verbatim;
   * compensating a pad train the app never asked about would corrupt it. The app's
   * instant is deliberately nowhere near the launch instant, so only a timestamp
   * that really came from the app can land on its tick. */
  const uint64_t app_tai = kCursorTai + 7 * kFramePeriodNs;
  ut_txv_set_user_timestamp(ctx_, true);

  EXPECT_EQ(measureShiftTicks(kTrs1080p60, kWarmPkts, app_tai),
            shiftTicks(kWideLeadCursorTai, tickOf(app_tai)));
}

TEST_F(St20TxRlRtpShiftTest, EpochBasedTimestampIsNotShifted) {
  /* ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH times the frame from the bare epoch, which
   * already sits a whole tr_offset before any packet leaves. Shifting it would
   * also break its own contract, which is that the value is the epoch. */
  ut_txv_set_rtp_timestamp_epoch(ctx_, true);

  /* A wide lead and a full pad train -- everything the shift needs but permission,
   * so the timestamp has to still be the bare epoch's own tick. */
  EXPECT_EQ(measureShiftTicksAtLead(kTrs1080p60, kWarmPkts, kWideLeadNs),
            shiftTicks(kWideLeadCursorTai, tickOf(kCursorTai)));

  /* A positive rtp_timestamp_delta_us is what keeps this exemption load-bearing
   * rather than redundant: it gives the epoch-based instant a lead of its own, and
   * without the exemption the cap would let the shift spend it. */
  const int32_t delta_us = 250;
  ut_txv_set_rtp_timestamp_delta_us(ctx_, delta_us);
  EXPECT_EQ(measureShiftTicksAtLead(kTrs1080p60, kWarmPkts, kWideLeadNs),
            shiftTicks(kWideLeadCursorTai, tickOf(kCursorTai + delta_us * 1000)));
}
