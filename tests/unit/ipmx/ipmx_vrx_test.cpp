/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * IPMX CMAX / VRX model of the RX timing parser (TR-10-1 section 8.1) on synthetic
 * arrival patterns, without a session.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxVrx*'
 */

#include "ipmx/ipmx_vrx.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>

#include "session/st20_harness.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr double kFrameTime60Ns = 1e9 / 60;
constexpr double kRactive = 1080.0 / 1125.0;
constexpr uint64_t kT0 = 1000000000000ull;

struct LimitCase {
  const char* name;
  uint32_t width;
  uint32_t height;
  enum st20_fmt fmt;
  int npackets;
  int32_t cmax;
  double vrx_drain_ns;
};

/* npackets = ceil(width x height x pg_size / pg_coverage / 1260), the BPM packet;
 * cmax = MAX(16, INT(npackets x 60 / 21600)); vrx drain = 0.96 x TFRAME / npackets */
const LimitCase kLimitCases[] = {
    {"Yuv422p10_1080p60", 1920, 1080, ST20_FMT_YUV_422_10BIT, 4115, 16, 3888.2139},
    {"Yuv422p10_2160p60", 3840, 2160, ST20_FMT_YUV_422_10BIT, 16458, 45, 972.1716},
    {"Rgb10_1080p60", 1920, 1080, ST20_FMT_RGB_10BIT, 6172, 17, 2592.3526},
    {"Rgb10_2160p60", 3840, 2160, ST20_FMT_RGB_10BIT, 24686, 68, 648.1406},
};

class IpmxVrxLimitTest : public ::testing::TestWithParam<LimitCase> {};

/* the limits rv_tp_init() sets up for the packets the TX session sends */
TEST_P(IpmxVrxLimitTest, LimitsMatchHandCalculation) {
  const LimitCase& c = GetParam();
  ASSERT_EQ(ut_txv_init(), 0);
  ut_txv_ctx* tx = ut_txv_create();
  ASSERT_NE(tx, nullptr);
  int npackets = ut_txv_frame_total_pkts(tx, c.width, c.height, c.fmt);
  ut_txv_destroy(tx);
  ASSERT_EQ(npackets, c.npackets);

  ASSERT_EQ(ut20_init(), 0);
  ut20_test_ctx* rx = ut20_ctx_create_geom(1, 2);
  ASSERT_NE(rx, nullptr);
  struct ipmx_vrx_limit limit;
  ASSERT_EQ(ut20_ipmx_tp_limit(rx, npackets, ST_FPS_P60, &limit), 0);
  ut20_ctx_destroy(rx);

  EXPECT_EQ(limit.cmax, c.cmax);
  EXPECT_EQ(limit.vrx_full, c.cmax * 2);
  EXPECT_NEAR(1 / limit.vrx_drain_per_ns, c.vrx_drain_ns, 0.001);
  EXPECT_NEAR(1 / limit.cinst_drain_per_ns, kFrameTime60Ns / (npackets * 1.1), 0.001);
}

INSTANTIATE_TEST_SUITE_P(Formats, IpmxVrxLimitTest, ::testing::ValuesIn(kLimitCases),
                         [](const ::testing::TestParamInfo<LimitCase>& info) {
                           return std::string(info.param.name);
                         });

/* 1080p60 YUV 4:2:2 10-bit: 4115 packets, CMAX 16, VRXFULL 32 */
class IpmxVrxTest : public ::testing::Test {
 protected:
  static constexpr int kPackets = 4115;
  static constexpr int32_t kCmax = 16;

  void SetUp() override {
    ipmx_vrx_init(&vrx_, kPackets, kFrameTime60Ns, kRactive, kCmax);
    NewFrame();
  }

  void NewFrame() {
    memset(&frame_, 0, sizeof(frame_));
  }

  void Pkt(int idx, double t_ns) {
    ipmx_vrx_on_packet(&vrx_, MTL_SESSION_PORT_P, &frame_, idx,
                       kT0 + static_cast<int64_t>(t_ns));
  }

  /* packets [from, to) one every period_ns, the first at start_ns */
  void Paced(int from, int to, double start_ns, double period_ns) {
    for (int i = from; i < to; i++) Pkt(i, start_ns + (i - from) * period_ns);
  }

  const char* Check() const {
    return ipmx_vrx_frame_check(&vrx_.limit, &frame_);
  }

  int32_t cmax() const {
    return vrx_.limit.cmax;
  }

  double trs() const {
    return 1 / vrx_.limit.vrx_drain_per_ns;
  }

  void Done(bool complete) {
    ipmx_vrx_frame_done(&vrx_, MTL_SESSION_PORT_P, &frame_, complete);
  }

  struct ipmx_vrx vrx_;
  struct ipmx_vrx_frame frame_;
};

TEST_F(IpmxVrxTest, NarrowGappedFramePasses) {
  Paced(0, kPackets, 0, trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_LE(frame_.cinst_max, 2);
  EXPECT_GE(frame_.vrx_min, cmax() - 1);
  EXPECT_LE(frame_.vrx_max, cmax() + 1);
}

TEST_F(IpmxVrxTest, BurstOfCmaxPasses) {
  Paced(0, cmax(), 0, 0);
  Paced(cmax(), kPackets, trs(), trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_EQ(frame_.cinst_max, cmax());
}

TEST_F(IpmxVrxTest, BurstPastCmaxFailsOnCinst) {
  Paced(0, cmax() + 1, 0, 0);
  Paced(cmax() + 1, kPackets, trs(), trs());
  EXPECT_STREQ(Check(), "cinst exceed cmax");
  EXPECT_EQ(frame_.cinst_max, cmax() + 1);
  EXPECT_LE(frame_.vrx_max, vrx_.limit.vrx_full);
}

TEST_F(IpmxVrxTest, CinstBucketCarriesAcrossFrames) {
  Paced(0, cmax(), 0, 0);
  EXPECT_EQ(Check(), nullptr) << Check();
  NewFrame();
  Paced(0, cmax(), 1, 0);
  EXPECT_STREQ(Check(), "cinst exceed cmax");
}

/* the clock steps back 1 s between frames; the bucket keeps its level and drains on */
TEST_F(IpmxVrxTest, CinstDrainsAfterClockStepsBack) {
  Paced(0, cmax() / 2, 0, 0);
  NewFrame();
  Paced(0, kPackets, -1e9, trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_EQ(frame_.cinst_max, cmax() / 2 + 1);
}

/* faster than the VRX drain, slower than the CINST drain */
TEST_F(IpmxVrxTest, OverFastSenderOverflowsVrx) {
  Paced(0, kPackets, 0, kFrameTime60Ns / (kPackets * 1.08));
  EXPECT_STREQ(Check(), "vrx exceed vrx_full");
  EXPECT_LE(frame_.cinst_max, 2);
  EXPECT_GT(frame_.vrx_max, vrx_.limit.vrx_full);
}

/* drain starts at the CMAX-th packet holding CMAX; the next one arriving after
 * CMAX + 1 drain periods finds the reader just caught up, one period later it is late */
TEST_F(IpmxVrxTest, ArrivalAsTheBufferEmptiesPasses) {
  Paced(0, cmax(), 0, 0);
  Pkt(cmax(), (cmax() + 1.5) * trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_EQ(frame_.vrx_min, 0);
}

TEST_F(IpmxVrxTest, ArrivalAfterTheBufferEmptiedUnderflows) {
  Paced(0, cmax(), 0, 0);
  Pkt(cmax(), (cmax() + 2.5) * trs());
  EXPECT_STREQ(Check(), "vrx underflow");
  EXPECT_EQ(frame_.vrx_min, -1);
}

TEST_F(IpmxVrxTest, NoDrainBeforeTheCmaxthPacket) {
  Paced(0, cmax() - 1, 0, 0);
  Paced(cmax() - 1, kPackets, kFrameTime60Ns, trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_EQ(frame_.vrx_min, cmax());
}

TEST_F(IpmxVrxTest, FrameJoinedMidwayCountsFromItsFirstPacket) {
  Paced(3000, kPackets, 0, trs());
  EXPECT_EQ(Check(), nullptr) << Check();
  EXPECT_LE(frame_.vrx_max, cmax() + 1);
}

TEST_F(IpmxVrxTest, FrameWithNoPacketTimedIsUnmeasured) {
  const struct ipmx_rx_tp_stat* stat = &vrx_.port[MTL_SESSION_PORT_P].stat;
  Done(true);
  EXPECT_EQ(stat->frames_unmeasured, 1u);
  EXPECT_EQ(stat->frames_compliant + stat->frames_failed, 0u);
}

TEST_F(IpmxVrxTest, IncompleteFrameIsUnmeasured) {
  const struct ipmx_rx_tp_stat* stat = &vrx_.port[MTL_SESSION_PORT_P].stat;
  Paced(0, cmax() + 1, 0, 0);
  ASSERT_NE(Check(), nullptr);
  Done(false);
  EXPECT_EQ(stat->frames_unmeasured, 1u);
  EXPECT_EQ(stat->frames_failed, 0u);
  EXPECT_EQ(stat->cinst_max, 0);
  EXPECT_STREQ(stat->failed_cause, "");
}

TEST_F(IpmxVrxTest, StatStartsUnmeasured) {
  const struct ipmx_rx_tp_stat* stat = &vrx_.port[MTL_SESSION_PORT_P].stat;
  EXPECT_EQ(stat->cmax, cmax());
  EXPECT_EQ(stat->vrx_full, cmax() * 2);
  EXPECT_EQ(stat->cinst_max, 0);
  EXPECT_EQ(stat->vrx_max, INT32_MIN);
  EXPECT_EQ(stat->vrx_min, INT32_MAX);
  EXPECT_EQ(stat->frames_compliant, 0u);
  EXPECT_EQ(stat->frames_failed, 0u);
  EXPECT_EQ(stat->frames_unmeasured, 0u);
  EXPECT_STREQ(stat->failed_cause, "");
}

TEST_F(IpmxVrxTest, StatFoldsFrames) {
  const struct ipmx_rx_tp_stat* stat = &vrx_.port[MTL_SESSION_PORT_P].stat;

  Paced(0, kPackets, 0, trs());
  Done(true);
  EXPECT_EQ(stat->frames_compliant, 1u);
  EXPECT_STREQ(stat->failed_cause, "");

  NewFrame();
  Paced(0, cmax() + 1, kFrameTime60Ns, 0);
  Done(true);
  EXPECT_EQ(stat->frames_compliant, 1u);
  EXPECT_EQ(stat->frames_failed, 1u);
  EXPECT_STREQ(stat->failed_cause, "cinst exceed cmax");
  EXPECT_EQ(stat->cinst_max, cmax() + 1);
  EXPECT_LE(stat->vrx_min, cmax());
  EXPECT_EQ(stat->vrx_max, cmax() + 1);
  EXPECT_EQ(vrx_.port[MTL_SESSION_PORT_R].stat.frames_failed, 0u);
}

}  // namespace
