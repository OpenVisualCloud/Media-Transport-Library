// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
uint64_t test_tai(double frame_time, uint64_t frame_count);
uint64_t test_trs_start(double frame_time, double tr_offset, double trs, uint32_t vrx,
                        uint64_t frame_count);
uint64_t test_tv_rl_bps(uint16_t pkt_size, int total_pkts, int mul, int den,
                        int interlaced, int height);
}

namespace {

// 60fps frame_time in nanoseconds.
constexpr double kFrameTime60 = 16666666.666666666;

TEST(TaiFromFrameCount, IsMonotonicallyIncreasingForSmallCounts) {
  for (uint64_t n = 0; n < 1000; n++) {
    EXPECT_LT(test_tai(kFrameTime60, n), test_tai(kFrameTime60, n + 1)) << "n=" << n;
  }
}

TEST(TaiFromFrameCount, AdjacentFrameCountsRemainStrictlyIncreasingNearDoubleLimit) {
  // The doc-comment on tai_from_frame_count explains: doubles lose
  // integer precision beyond 2^53 (~9e15). Without nextafter, naive
  // (uint64_t)(n * frame_time) would collapse two adjacent frame
  // counts to the same TAI in this range. nextafter must round up
  // far enough to keep adjacent calls strictly increasing.
  //
  // Pick an n where n * frame_time straddles 2^53.
  uint64_t n = static_cast<uint64_t>((1ull << 53) / kFrameTime60);
  for (uint64_t k = 0; k < 16; k++) {
    EXPECT_LT(test_tai(kFrameTime60, n + k), test_tai(kFrameTime60, n + k + 1))
        << "n+k=" << (n + k);
  }
}

// transmission_start_time(pacing, n) = tai_from_frame_count(n) + tr_offset - vrx*trs.
// It is what feeds the per-frame TX scheduler; verify the algebraic
// identity holds for representative inputs.

TEST(TransmissionStartTime, EqualsTaiPlusOffsetMinusVrxTrs) {
  const double tr_offset = 250000.0;  // ns
  const double trs = 1000.0;          // ns per packet
  const uint32_t vrx = 200;
  for (uint64_t n = 0; n < 8; n++) {
    uint64_t tai = test_tai(kFrameTime60, n);
    uint64_t expected = tai + (uint64_t)tr_offset - (uint64_t)(vrx * trs);
    EXPECT_EQ(expected, test_trs_start(kFrameTime60, tr_offset, trs, vrx, n))
        << "n=" << n;
  }
}

TEST(TransmissionStartTime, ZeroVrxOrZeroTrsCollapsesToTaiPlusOffset) {
  const double tr_offset = 250000.0;
  for (uint64_t n = 0; n < 4; n++) {
    uint64_t tai_plus = test_tai(kFrameTime60, n) + (uint64_t)tr_offset;
    EXPECT_EQ(tai_plus, test_trs_start(kFrameTime60, tr_offset, 0.0, 200, n));
    EXPECT_EQ(tai_plus, test_trs_start(kFrameTime60, tr_offset, 1000.0, 0, n));
  }
}

// tv_rl_bps computes the rate-limit feed for the HW pacer:
//   bps = pkt_size * total_pkts * mul / den / reactive
// where reactive == 1.0 except SD interlaced (480i: 487/525,
// 576i: 576/625). Tests are written as properties (linearity,
// ratio-equivalence, branch-selection) rather than numeric oracles
// to avoid pinning the implementation to a specific arithmetic order.

// Anchor inputs reused across property tests; each test perturbs ONE
// dimension at a time so a regression points at a specific factor.
constexpr uint16_t kPkt = 1280;
constexpr int kTotal = 4320;
constexpr int kMul = 60;
constexpr int kDen = 1;

TEST(TvRlBps, ScalesLinearlyWithPacketSize) {
  uint64_t base = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 0, 1080);
  uint64_t doubled = test_tv_rl_bps(2 * kPkt, kTotal, kMul, kDen, 0, 1080);
  uint64_t tripled = test_tv_rl_bps(3 * kPkt, kTotal, kMul, kDen, 0, 1080);
  EXPECT_EQ(2 * base, doubled);
  EXPECT_EQ(3 * base, tripled);
}

TEST(TvRlBps, ScalesLinearlyWithTotalPkts) {
  uint64_t base = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 0, 1080);
  uint64_t doubled = test_tv_rl_bps(kPkt, 2 * kTotal, kMul, kDen, 0, 1080);
  EXPECT_EQ(2 * base, doubled);
}

TEST(TvRlBps, OnlyDependsOnFpsRatioNotAbsoluteMulDen) {
  // 60/1, 600/10, 6000/100 are the same fps; result must match.
  uint64_t a = test_tv_rl_bps(kPkt, kTotal, 60, 1, 0, 1080);
  uint64_t b = test_tv_rl_bps(kPkt, kTotal, 600, 10, 0, 1080);
  uint64_t c = test_tv_rl_bps(kPkt, kTotal, 6000, 100, 0, 1080);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
}

TEST(TvRlBps, MonotonicInFpsRatio) {
  // Higher fps → higher bps.
  uint64_t r25 = test_tv_rl_bps(kPkt, kTotal, 25, 1, 0, 1080);
  uint64_t r30 = test_tv_rl_bps(kPkt, kTotal, 30, 1, 0, 1080);
  uint64_t r60 = test_tv_rl_bps(kPkt, kTotal, 60, 1, 0, 1080);
  EXPECT_LT(r25, r30);
  EXPECT_LT(r30, r60);
}

// --- branch coverage of the SD-interlaced reactive correction --- //

TEST(TvRlBps, ReactiveOnlyAppliesToInterlacedSdHeights) {
  // Only the (interlaced && height<=576) branch may differ from the
  // progressive baseline. Sweep representative heights and the
  // interlaced flag; assert the branch fires exactly when expected.
  struct Case {
    int height;
    bool interlaced;
    bool expect_reactive;
  };
  const Case cases[] = {
      {1080, false, false}, {1080, true, false},  // HD: never
      {720, false, false},  {720, true, false},   // 720 > 576: never
      {576, false, false},  {576, true, true},    // PAL: only interlaced
      {480, false, false},  {480, true, true},    // NTSC: only interlaced
  };
  uint64_t baseline = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 0, 1080);
  for (const auto& c : cases) {
    uint64_t v = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, c.interlaced, c.height);
    if (c.expect_reactive) {
      EXPECT_GT(v, baseline) << "h=" << c.height << " i=" << c.interlaced;
    } else {
      EXPECT_EQ(v, baseline) << "h=" << c.height << " i=" << c.interlaced;
    }
  }
}

TEST(TvRlBps, PalReactiveIsHigherThanNtscReactive) {
  // PAL ratio 576/625 ≈ 0.9216, NTSC 487/525 ≈ 0.9276.
  // Smaller reactive → larger bps after dividing. Catches a swap of
  // the two ratio constants.
  uint64_t ntsc_480i = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 1, 480);
  uint64_t pal_576i = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 1, 576);
  EXPECT_GT(pal_576i, ntsc_480i);
}

TEST(TvRlBps, Height576UsesPalRatioNotNtscRatio) {
  // White-box: the if/else picks NTSC only for height==480; height==576
  // must take the PAL branch even though both are <=576. A mistaken
  // `<= 480` would silently apply NTSC to PAL streams.
  uint64_t pal_576i = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 1, 576);
  uint64_t ntsc_480i = test_tv_rl_bps(kPkt, kTotal, kMul, kDen, 1, 480);
  // The 576i answer must NOT coincide with what the 480i ratio would
  // give for the same inputs — otherwise the branch was mis-selected.
  // Use a synthetic comparison: rerun with height 480 and same
  // interlaced=true; if they ever match exactly, the branch collapsed.
  EXPECT_NE(pal_576i, ntsc_480i);
}

TEST(TvRlBps, LargePacketCountDoesNotOverflowUint64) {
  // 4K p60 worst-case: ~17280 pkts/frame * 1500B * 60fps. The
  // intermediate product fits in double mantissa, but the final cast
  // to uint64 must not wrap. Check via linearity: 2x pkts → 2x bps
  // even at 4K scale — wraparound would break the relation.
  uint64_t a = test_tv_rl_bps(1500, 17280, 60, 1, 0, 2160);
  uint64_t b = test_tv_rl_bps(1500, 2 * 17280, 60, 1, 0, 2160);
  EXPECT_EQ(2 * a, b);
  EXPECT_GT(a, 0u);
}

}  // namespace
