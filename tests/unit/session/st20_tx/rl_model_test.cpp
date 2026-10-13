/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the RL pre-arm burst plan of st_tx_video_rl.h against a frozen copy of the
 * arithmetic video_trs_rl_pre_arm() did inline before it moved there, a guard that the
 * move changed nothing: over a grid of shaper drains, pad sizes, trim pad counts and
 * launch gaps every burst and drain end must be identical. A change to the plan on
 * purpose updates the frozen copy with it. Also pins st_tx_video_rl_tokens() against a
 * copy of ice's RL profile encode.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlModelTest.*'
 */

#include <gtest/gtest.h>

#include <vector>

extern "C" {
#include "st2110/st_tx_video_rl.h"
}

namespace {

struct FrozenBurst {
  unsigned int n, coarse, fine;
  bool last;
  long double drained; /* after the burst */
};

int64_t FrozenTrimPads(uint16_t pad_size) {
  return RTE_MIN(pad_size / 64 + 64 / 8, 32);
}

/* one pass of the pre-arm loop past its idle clamp, as it was inline */
FrozenBurst FrozenNextBurst(long double drain, uint16_t pad_size, int64_t trim_pads,
                            long double launch, long double drained) {
  int64_t needed = (int64_t)ceill((launch - drained) / drain);
  bool last = needed <= trim_pads;
  uint32_t cut = 0;
  unsigned int n = last ? needed : RTE_MIN(needed - trim_pads, 32);
  unsigned int coarse = 0, fine = 0;

  if (last) {
    cut = (uint32_t)((n * drain - (launch - drained)) * pad_size / drain);
    coarse = RTE_MIN(cut / 64, n);
    fine = RTE_MIN((cut - coarse * 64) / 8, n - coarse);
    cut = coarse * 64 + fine * 8;
  }
  drained += n * drain - cut * drain / pad_size;
  return {n, coarse, fine, last, drained};
}

/* 1080p59, 2160p59 and 1080p25 drains of a trained shaper, and two round-off cases */
const std::vector<long double> kDrains = {16683333.333L * 1080 / 1125 / 4320 * 164 / 165,
                                          926.8L, 8835.0L, 1000.0L, 1234.56789L};
const std::vector<uint16_t> kPadSizes = {1262, 1198, 128, 1442, 8962};
/* tsc values a drain end is measured from */
const std::vector<long double> kBases = {0, 1e9L + 0.37L, 3.6e12L + 0.123L,
                                         1.8e15L + 0.5L};
/* gaps past a whole number of pads; 1 is the next whole number, where ceill turns */
const std::vector<long double> kPadFractions = {1e-6L, 0.25L, 0.5L, 0.999999L, 1};

std::vector<int64_t> TrimPadCounts(uint16_t pad_size) {
  return {FrozenTrimPads(pad_size), 1, 8, 27, 32};
}

testing::AssertionResult SameBurst(const FrozenBurst& want,
                                   const struct st_tx_video_rl_burst& b,
                                   long double drained) {
  if (b.n == want.n && b.coarse == want.coarse && b.fine == want.fine &&
      b.last == want.last && drained == want.drained)
    return testing::AssertionSuccess();
  return testing::AssertionFailure()
         << "n " << b.n << " vs " << want.n << ", coarse " << b.coarse << " vs "
         << want.coarse << ", fine " << b.fine << " vs " << want.fine << ", last "
         << b.last << " vs " << want.last << ", drained " << (double)drained << " vs "
         << (double)want.drained;
}

}  // namespace

TEST(St20TxRlModelTest, TrimPadsMatchTheInlineCount) {
  for (uint32_t pad_size = 0; pad_size <= UINT16_MAX; pad_size++)
    ASSERT_EQ(st_tx_video_rl_trim_pads(pad_size), FrozenTrimPads(pad_size)) << pad_size;
}

TEST(St20TxRlModelTest, EveryBurstMatchesTheInlinePlan) {
  for (long double drain : kDrains) {
    for (uint16_t pad_size : kPadSizes) {
      for (int64_t trim_pads : TrimPadCounts(pad_size)) {
        for (long double base : kBases) {
          for (int pads = 0; pads <= 140; pads++) {
            for (long double fraction : kPadFractions) {
              long double launch = base + (pads + fraction) * drain;
              struct st_tx_video_rl_burst b =
                  st_tx_video_rl_next_burst(drain, pad_size, trim_pads, launch, base);
              FrozenBurst want =
                  FrozenNextBurst(drain, pad_size, trim_pads, launch, base);
              long double drained = base + st_tx_video_rl_burst_ns(&b, drain, pad_size);

              ASSERT_TRUE(SameBurst(want, b, drained))
                  << "drain " << (double)drain << " pad_size " << pad_size
                  << " trim_pads " << trim_pads << " base " << (double)base << " pads "
                  << pads << " fraction " << (double)fraction;
            }
          }
        }
      }
    }
  }
}

/* a whole pre-arm with no tsc moving: the bursts up to the last one and its drain end */
TEST(St20TxRlModelTest, AWholePlanMatchesTheInlineLoop) {
  for (long double drain : kDrains) {
    for (uint16_t pad_size : kPadSizes) {
      int64_t trim_pads = st_tx_video_rl_trim_pads(pad_size);
      for (long double base : kBases) {
        for (int pads = 0; pads <= 300; pads += 7) {
          for (long double fraction : kPadFractions) {
            long double launch = base + (pads + fraction) * drain;
            long double drained = base, frozen = base;
            bool last = false;

            for (int burst = 0; !last; burst++) {
              ASSERT_LT(burst, 32) << "the plan never ends";
              struct st_tx_video_rl_burst b =
                  st_tx_video_rl_next_burst(drain, pad_size, trim_pads, launch, drained);
              FrozenBurst want =
                  FrozenNextBurst(drain, pad_size, trim_pads, launch, frozen);
              drained += st_tx_video_rl_burst_ns(&b, drain, pad_size);
              frozen = want.drained;

              ASSERT_TRUE(SameBurst(want, b, drained))
                  << "drain " << (double)drain << " pad_size " << pad_size << " base "
                  << (double)base << " pads " << pads << " fraction " << (double)fraction
                  << " burst " << burst;
              last = b.last || launch <= drained;
            }
          }
        }
      }
    }
  }
}

/* long double rounding of a drain end, under 1 ps at a 1.8e15 ns TSC */
constexpr long double kRoundingNs = 1e-3L;

/* With pads enough for a whole cut, the plan's drain end, packet 0's launch, is at the
 * launch or later by up to one TRIM_FINE cut, never earlier. */
TEST(St20TxRlModelTest, APlanEndsWithinAFineTrimPastLaunch) {
  for (long double drain : kDrains) {
    for (uint16_t pad_size : kPadSizes) {
      int64_t trim_pads = st_tx_video_rl_trim_pads(pad_size);
      if (trim_pads < pad_size / 64 + 64 / 8) continue; /* capped: no whole-pad cut */
      const long double fine_ns = ST_TX_VIDEO_RL_TRIM_FINE * drain / pad_size;
      for (long double base : kBases) {
        for (int pads = (int)trim_pads; pads <= 300; pads++) {
          for (long double fraction : kPadFractions) {
            long double launch = base + (pads + fraction) * drain;
            long double drained = base;
            bool last = false;

            for (int burst = 0; !last; burst++) {
              ASSERT_LT(burst, 32) << "the plan never ends";
              struct st_tx_video_rl_burst b =
                  st_tx_video_rl_next_burst(drain, pad_size, trim_pads, launch, drained);
              drained += st_tx_video_rl_burst_ns(&b, drain, pad_size);
              last = b.last;
            }
            ASSERT_GE(drained, launch - kRoundingNs)
                << "drain " << (double)drain << " pad_size " << pad_size << " base "
                << (double)base << " pads " << pads << " fraction " << (double)fraction;
            ASSERT_LE(drained, launch + fine_ns + kRoundingNs)
                << "drain " << (double)drain << " pad_size " << pad_size << " base "
                << (double)base << " pads " << pads << " fraction " << (double)fraction;
          }
        }
      }
    }
  }
}

/* ice_sched_bw_to_rl_profile(): the tokens of a timeslot of 32 << encode psm_clk cycles,
 * and the encode it takes, for a rate in kbps */
static int64_t IceTokens(int64_t psm_hz, int64_t kbps, int encode) {
  int64_t bytes_per_sec = kbps * 1000 / 8;
  int64_t ts_rate = psm_hz / ((int64_t)1 << encode) / 32;
  if (ts_rate <= 0) return 0;
  return (bytes_per_sec * 10000 / ts_rate + 5000) / 10000;
}

static int IceEncode(int64_t psm_hz, int64_t kbps) {
  for (int i = 0; i < 64; i++)
    if (IceTokens(psm_hz, kbps, i) > 128) return i;
  return -1;
}

/* The shaper releases a packet on its timeslot grid. Whatever the NIC clock, a timeslot
 * passes the tokens ice programs at the rate the shaper drains, so the tokens bound the
 * timeslot of every rate ice takes, on every psm_clk it reads. A rate is no faster than
 * the bytes/s MTL programs, which iavf or the PMD truncates to kbps. */
TEST(St20TxRlModelTest, TokensCoverEveryIceTimeslot) {
  const int64_t kPsmHz[] = {367647059, 390625000, 416666667, 446428571};
  constexpr int64_t kMinKbps = 500, kMaxKbps = 100000000; /* ICE_SCHED_MIN/MAX_BW */

  for (int64_t psm : kPsmHz) {
    /* a faster rate takes a shorter timeslot; an encode has most tokens at its top */
    for (int encode = IceEncode(psm, kMaxKbps); encode <= IceEncode(psm, kMinKbps);
         encode++) {
      int64_t lo = kMinKbps, hi = kMaxKbps;
      while (lo < hi) {
        int64_t mid = lo + (hi - lo + 1) / 2;
        if (IceEncode(psm, mid) >= encode)
          lo = mid;
        else
          hi = mid - 1;
      }
      for (int64_t kbps : {lo, kMaxKbps}) {
        if (IceEncode(psm, kbps) != encode) continue;
        EXPECT_LE((double)IceTokens(psm, kbps, encode),
                  (double)st_tx_video_rl_tokens((uint64_t)kbps * 1000 / 8))
            << "psm " << psm << " encode " << encode << " kbps " << kbps;
      }
    }
  }
}
