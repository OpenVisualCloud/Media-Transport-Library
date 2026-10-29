/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * What mt_ptp.c keeps of the current master's Announce messages.
 *
 * ptp_parse_announce() stores the grandmaster identity, the domain, the log announce
 * interval and the arrival time (mt_get_tsc()) of every Announce whose source port
 * identity is the master the built-in PTP selected, under the master_announce_seq
 * seqcount of struct mt_ptp_impl. The IPMX Sender Report reads them to build the
 * ts-refclk string (tests/unit/ipmx/ipmx_refclk_test.cpp). These cases feed Announce
 * messages through mt_ptp_parse() and read the stored values back, so they pin the
 * writer side of that contract: what is stored, and that Announces of any other clock
 * or port never overwrite the master's values.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='PtpAnnounce*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "ptp/ptp_harness.h"

namespace {

/* clock identities of the source port: the master the PTP stack selects, and another */
constexpr uint8_t kMaster[8] = {0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
constexpr uint8_t kOther[8] = {0x02, 0x66, 0x77, 0xff, 0xfe, 0x88, 0x99, 0xaa};
/* grandmasterIdentity carried in the Announce body, the TR-10-1 section 8.7 example */
constexpr uint8_t kGm[8] = {0xec, 0x46, 0x70, 0xff, 0xfe, 0x10, 0xff, 0xb0};
constexpr uint8_t kGm2[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

class PtpAnnounceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }
  /* the stored master Announce state, as the IPMX SR reads it */
  struct ut_ptp_master_announce Master() {
    struct ut_ptp_master_announce m;
    ut_ptp_master_announce(ctx_, &m);
    return m;
  }
  ut_ptp_ctx* ctx_ = nullptr;
};

/* A zero arrival time is how a reader sees "no master yet". */
TEST_F(PtpAnnounceTest, NoneBeforeTheFirstAnnounce) {
  EXPECT_EQ(Master().tsc_ns, 0u);
  EXPECT_EQ(Master().gm_id, 0u);
}

/* The first Announce selects the master and stores its grandmaster (octet 0 most
 * significant), domain, interval and a non-zero arrival time. */
TEST_F(PtpAnnounceTest, FirstAnnounceStoresItsGrandmaster) {
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 1, kGm, 127, -2), 0);
  struct ut_ptp_master_announce m = Master();
  EXPECT_EQ(m.gm_id, 0xEC4670FFFE10FFB0ull);
  EXPECT_EQ(m.domain, 127);
  EXPECT_EQ(m.log_interval, -2);
  EXPECT_NE(m.tsc_ns, 0u);
}

/* Every later Announce of the same master overwrites all four values, so a grandmaster
 * or domain change behind a boundary clock reaches the reader. */
TEST_F(PtpAnnounceTest, LaterAnnounceOfTheMasterUpdates) {
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 1, kGm, 127, 0), 0);
  uint64_t first_ns = Master().tsc_ns;
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 1, kGm2, 3, 1), 0);
  struct ut_ptp_master_announce m = Master();
  EXPECT_EQ(m.gm_id, 0x0001020304050607ull);
  EXPECT_EQ(m.domain, 3);
  EXPECT_EQ(m.log_interval, 1);
  EXPECT_GE(m.tsc_ns, first_ns);
}

/* The built-in PTP never re-selects its master, so another clock's Announce changes
 * nothing, not even the arrival time (it must not keep a lost master looking fresh). */
TEST_F(PtpAnnounceTest, AnnounceOfAnotherClockIgnored) {
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 1, kGm, 127, 0), 0);
  struct ut_ptp_master_announce before = Master();
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kOther, 1, kGm2, 3, 1), 0);
  struct ut_ptp_master_announce after = Master();
  EXPECT_EQ(after.gm_id, before.gm_id);
  EXPECT_EQ(after.domain, before.domain);
  EXPECT_EQ(after.log_interval, before.log_interval);
  EXPECT_EQ(after.tsc_ns, before.tsc_ns);
}

/* The match is on the full port identity: the master's clock on another port number is
 * a different source and is ignored too. */
TEST_F(PtpAnnounceTest, AnnounceOfAnotherPortOfTheMasterClockIgnored) {
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 1, kGm, 127, 0), 0);
  ASSERT_EQ(ut_ptp_rx_announce(ctx_, kMaster, 2, kGm2, 3, 1), 0);
  EXPECT_EQ(Master().gm_id, 0xEC4670FFFE10FFB0ull);
  EXPECT_EQ(Master().domain, 127);
}

}  // namespace
