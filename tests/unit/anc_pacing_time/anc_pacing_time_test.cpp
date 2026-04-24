// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation
//
// Both ancillary and fast-metadata TX sessions compute the per-epoch
// pacing time as `epochs * frame_time`. Ancillary's helper applies
// nextafter() before casting to uint64_t; fast-metadata's helper
// returns a raw double, and the caller's implicit cast to uint64_t
// can collapse adjacent epochs near double-precision limits.
//
// These tests assert strict monotonicity on the uint64_t value each
// pipeline ultimately stores. Ancillary should always pass.
// Fast-metadata is expected to fail at large epoch counts —
// demonstrating the asymmetry as a real bug.

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
uint64_t test_anc_pacing_time(double frame_time, uint64_t epochs);
uint64_t test_fmd_pacing_time(double frame_time, uint64_t epochs);
uint32_t test_anc_pacing_time_stamp(double frame_time_sampling, uint64_t epochs);
uint32_t test_fmd_pacing_time_stamp(double frame_time_sampling, uint64_t epochs);
}

namespace {

// 60fps frame_time in nanoseconds.
constexpr double kFrameTime60 = 16666666.666666666;

TEST(AncPacingTime, ZeroEpochsIsZero) {
  EXPECT_EQ(0u, test_anc_pacing_time(kFrameTime60, 0));
}

TEST(AncPacingTime, MonotonicForSmallEpochs) {
  for (uint64_t e = 0; e < 1000; e++) {
    EXPECT_LT(test_anc_pacing_time(kFrameTime60, e),
              test_anc_pacing_time(kFrameTime60, e + 1))
        << "e=" << e;
  }
}

TEST(AncPacingTime, MonotonicNearDoublePrecisionLimit) {
  // Production comment on the parallel video helper documents this
  // boundary explicitly: doubles lose integer precision past 2^53.
  // Ancillary's nextafter() must keep adjacent epochs separable.
  uint64_t e = static_cast<uint64_t>((1ull << 53) / kFrameTime60);
  for (uint64_t k = 0; k < 16; k++) {
    EXPECT_LT(test_anc_pacing_time(kFrameTime60, e + k),
              test_anc_pacing_time(kFrameTime60, e + k + 1))
        << "e+k=" << (e + k);
  }
}

TEST(FmdPacingTime, MonotonicForSmallEpochs) {
  for (uint64_t e = 0; e < 1000; e++) {
    EXPECT_LT(test_fmd_pacing_time(kFrameTime60, e),
              test_fmd_pacing_time(kFrameTime60, e + 1))
        << "e=" << e;
  }
}

TEST(FmdPacingTime, MonotonicNearDoublePrecisionLimit) {
  // Same boundary as AncPacingTime.MonotonicNearDoublePrecisionLimit.
  // The fast-metadata helper does NOT call nextafter, so the implicit
  // double→uint64_t cast in the caller can collapse adjacent epochs;
  // this test is expected to FAIL until fmd applies nextafter the way
  // ancillary does.
  uint64_t e = static_cast<uint64_t>((1ull << 53) / kFrameTime60);
  for (uint64_t k = 0; k < 16; k++) {
    EXPECT_LT(test_fmd_pacing_time(kFrameTime60, e + k),
              test_fmd_pacing_time(kFrameTime60, e + k + 1))
        << "e+k=" << (e + k);
  }
}

// pacing_time_stamp returns uint32; both helpers compute
// (uint32_t)(epochs * frame_time_sampling). Lock the truncation
// behavior — a future "fix" that switched to a 64-bit return must
// not silently widen the wire format.

constexpr double kSampling90k =
    90000.0 / 1e9 * 16666666.666666666;  // ~1500 ticks/frame at 60fps

TEST(AncPacingTimeStamp, EqualsLow32BitsOfProduct) {
  for (uint64_t e = 0; e < 8; e++) {
    uint64_t expected64 = (uint64_t)(e * kSampling90k);
    EXPECT_EQ((uint32_t)expected64, test_anc_pacing_time_stamp(kSampling90k, e))
        << "e=" << e;
  }
}

TEST(AncPacingTimeStamp, WrapsAtUint32Boundary) {
  // Pick an epoch that drives the product just past 2^32 and verify
  // wraparound is exact. ancillary == fmd should produce identical
  // truncated values for the same inputs.
  uint64_t e = (1ull << 32) / (uint64_t)kSampling90k + 1;
  uint32_t a = test_anc_pacing_time_stamp(kSampling90k, e);
  uint32_t f = test_fmd_pacing_time_stamp(kSampling90k, e);
  EXPECT_EQ(a, f);
}

TEST(FmdPacingTimeStamp, AgreesWithAncillaryAcrossSamplingRates) {
  for (double sampling : {48000.0 / 1e9 * 1e6, 90000.0 / 1e9 * 1e6, 1.0}) {
    for (uint64_t e = 0; e < 4; e++) {
      EXPECT_EQ(test_anc_pacing_time_stamp(sampling, e),
                test_fmd_pacing_time_stamp(sampling, e))
          << "sampling=" << sampling << " e=" << e;
    }
  }
}

}  // namespace
