/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PtpUserSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    /* EAL first: ut_ptp_create() seeds impl.tsc_hz from rte_get_tsc_hz(), which
     * is 0 before rte_eal_init(). mt_get_tsc() would then divide by zero and
     * every "local clock" reading below would be garbage rather than a real
     * TSC value -- the no_timesync preconditions would pass for the wrong
     * reason. */
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }

  ut_ptp_ctx* ctx_ = nullptr;
};

TEST_F(PtpUserSyncTest, HardwareTimestampHasNoCumulativeSyncBias) {
  constexpr uint64_t kInitialRawNs = 1000000000000ull;
  constexpr int64_t kFirstCorrectionNs = 100;
  constexpr int64_t kSecondCorrectionNs = 167;

  uint64_t user_ns = kInitialRawNs + kFirstCorrectionNs;
  ut_ptp_set_raw_time(kInitialRawNs);
  ut_ptp_set_user_time(ctx_, user_ns);
  ut_ptp_sync_from_user(ctx_);
  /* The correction must land in the NIC PHC, which is where the hardware RX
   * timestamps come from. Asserting only the identity below would still pass if
   * the PHC were never stepped at all -- the clock would then free-run away
   * from PTP time with nothing to catch it. */
  EXPECT_EQ(ut_ptp_get_raw_time(), user_ns);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, user_ns), user_ns);

  user_ns += kSecondCorrectionNs;
  ut_ptp_set_user_time(ctx_, user_ns);
  ut_ptp_sync_from_user(ctx_);
  EXPECT_EQ(ut_ptp_get_raw_time(), user_ns);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, user_ns), user_ns);
}

/* A VF never gets MT_IF_FEATURE_TIMESYNC (dev_start_timesync() only runs for
 * MT_PORT_PF), so a hw-timestamp session on a VF runs ptp_sync_from_user() with
 * no_timesync set. There the correction is a TSC-domain offset parked in
 * no_timesync_delta -- it must not be added to a NIC hardware RX timestamp,
 * which is already in the PTP domain. Applying it shifted every RX timestamp by
 * the whole TSC-to-realtime gap and made the ST 2110-21 timing parser score
 * every frame non-compliant. */
TEST_F(PtpUserSyncTest, NoTimesyncLeavesHardwareTimestampUntouched) {
  constexpr uint64_t kUserNs = 1700000000000000000ull; /* realtime, far from TSC */
  /* Kept under 2^53 so ptp_correct_ts()'s double multiply stays exact; the bug
   * this guards against shifts the result by ~1.7e18, not by rounding. */
  constexpr uint64_t kHardwareNs = 1234567890000000ull;

  ut_ptp_set_user_time(ctx_, kUserNs);
  ut_ptp_sync_from_user_no_timesync(ctx_);

  /* Precondition: a real correction was parked in the software accumulator.
   * Without this the identity below would still hold if delta ever collapsed
   * to zero (see the expect_result_avg clamp in ptp_sync_from_user), leaving
   * the test passing while pinning nothing. */
  EXPECT_NE(ut_ptp_no_timesync_delta(ctx_), 0);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, kHardwareNs), kHardwareNs);
}

/* Same property for the PTP-protocol path (ptp_adjust_delta), which is what runs
 * for a normal --ptp session. On a VF there is no MT_IF_FEATURE_TIMESYNC, so the
 * adjustment goes to the software no_timesync_delta accumulator: a TSC-domain
 * offset roughly the size of the whole TSC-to-realtime gap. A NIC hardware RX
 * timestamp is already in the PTP domain and must come back unchanged. */
TEST_F(PtpUserSyncTest, AdjustDeltaNoTimesyncLeavesHardwareTimestampUntouched) {
  /* Order of the TSC-to-realtime gap: what delta looks like on the first sync
   * when the "local clock" is a raw TSC reading. */
  constexpr int64_t kTscToPtpGapNs = 850000000000000000ll;
  /* Kept under 2^53 so ptp_correct_ts()'s double multiply stays exact. */
  constexpr uint64_t kHardwareNs = 1234567890000000ull;

  ut_ptp_adjust_delta(ctx_, kTscToPtpGapNs, false);

  /* Precondition: the correction really landed in the software accumulator,
   * otherwise the identity below would hold vacuously. */
  EXPECT_NE(ut_ptp_no_timesync_delta(ctx_), 0);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, kHardwareNs), kHardwareNs);
}

/* And on a PF with timesync: rte_eth_timesync_adjust_time() steps the NIC PHC
 * that the hardware RX timestamps are derived from, so they already carry the
 * correction. Re-applying it in software would double-count without bound. */
TEST_F(PtpUserSyncTest, AdjustDeltaWithTimesyncLeavesHardwareTimestampUntouched) {
  constexpr uint64_t kInitialRawNs = 1000000000000ull;
  constexpr int64_t kDeltaNs = 4321ll;
  /* Kept under 2^53 so ptp_correct_ts()'s double multiply stays exact. */
  constexpr uint64_t kHardwareNs = 1234567890000000ull;

  ut_ptp_set_no_timesync(ctx_, false);
  ut_ptp_set_raw_time(kInitialRawNs);

  ut_ptp_adjust_delta(ctx_, kDeltaNs, false);

  /* The correction must be visible in the mocked PHC and nowhere else. */
  EXPECT_EQ(ut_ptp_get_raw_time(), kInitialRawNs + kDeltaNs);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 0);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, kHardwareNs), kHardwareNs);
}

/* The cases above all sit under 2^53 to keep ptp_correct_ts()'s double multiply
 * exact. Production hardware timestamps are ~1.7e18, so on their own they would
 * not notice a correction reinstated only at realistic magnitudes. Repeat the
 * no_timesync property there, with a tolerance for the ~256ns quantisation that
 * the double round-trip imposes at this scale. */
TEST_F(PtpUserSyncTest, NoTimesyncLeavesRealisticHardwareTimestampUntouched) {
  constexpr uint64_t kUserNs = 1700000000000000000ull;
  constexpr uint64_t kHardwareNs = 1700000000123456789ull;
  /* One ulp of a double at 1.7e18 is 256ns, so the identity can only be
   * asserted to within half that. The bug this guards against is ~1e18. */
  constexpr uint64_t kQuantisationNs = 256;

  ut_ptp_set_user_time(ctx_, kUserNs);
  ut_ptp_sync_from_user_no_timesync(ctx_);
  ASSERT_NE(ut_ptp_no_timesync_delta(ctx_), 0);

  const uint64_t got = ut_ptp_mbuf_time_stamp(ctx_, kHardwareNs);
  const uint64_t diff = got > kHardwareNs ? got - kHardwareNs : kHardwareNs - got;
  EXPECT_LE(diff, kQuantisationNs) << "got " << got << " want " << kHardwareNs;
}

/* mbuf_hw_time_stamp() still owes the caller ptp_correct_ts()'s frequency-drift
 * correction. Every other case here runs with coefficient 1.0 and last_sync_ts
 * 0, where that call is an identity and could be deleted unnoticed. */
TEST_F(PtpUserSyncTest, HardwareTimestampKeepsFrequencyDriftCorrection) {
  constexpr uint64_t kLastSyncNs = 1000000000000ull;
  /* 2^20 ns of advance and a 1 + 2^-20 coefficient (~0.95ppm): both are exactly
   * representable as doubles and their product is exactly kAdvanceNs + 1, so the
   * expectation needs no epsilon. A decimal coefficient like 1.000001 is NOT
   * representable and truncates the product back down to kAdvanceNs. */
  constexpr uint64_t kAdvanceNs = 1048576ull;
  constexpr double kCoefficient = 1.0 + 1.0 / 1048576.0;

  ut_ptp_set_last_sync_ts(ctx_, kLastSyncNs);
  ut_ptp_set_coefficient(ctx_, kCoefficient);

  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, kLastSyncNs + kAdvanceNs),
            kLastSyncNs + kAdvanceNs + 1);
}
