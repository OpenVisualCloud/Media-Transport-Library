/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX create-time validation of st20 TX ops, through tv_ops_check(), the
 * validation st20_tx_create() runs before it allocates anything.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxTx*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "ipmx/ipmx_test_ops.h"
#include "session/st20_tx_harness.h"
#include "session/stderr_capture.h"

namespace {

using ut_ipmx::IpmxFormat;
using ut_ipmx::kIpmxFormats;
using ut_ipmx::TxOps;

class IpmxTxValidateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
  }
};

TEST_F(IpmxTxValidateTest, FlagOffLeavesNonIpmxOpsAccepted) {
  struct st20_tx_ops ops = TxOps(2);
  ops.flags = 0;
  ops.udp_port[0] = 1001;
  ops.udp_port[1] = 80;
  ops.pacing = ST21_PACING_LINEAR;
  ops.interlaced = true;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);

  ops.type = ST20_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, EvenPortAbove5000Accepted) {
  struct st20_tx_ops ops = TxOps();
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, DefaultPortAccepted) {
  struct st20_tx_ops ops = TxOps();
  ops.udp_port[0] = 0; /* the library picks 10000 + 2 * idx */
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, OddPortRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.udp_port[0] = 20001;
  EXPECT_EQ(ut_txv_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxTxValidateTest, PortAtOrBelow1024Rejected) {
  struct st20_tx_ops ops = TxOps();
  ops.udp_port[0] = 1024;
  EXPECT_EQ(ut_txv_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxTxValidateTest, BadRedundantPortRejectedWithValidPrimary) {
  struct st20_tx_ops ops = TxOps(2);
  ops.udp_port[MTL_SESSION_PORT_R] = 20003;
  EXPECT_EQ(ut_txv_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxTxValidateTest, PortFrom1025To5000AcceptedWithWarning) {
  struct st20_tx_ops ops = TxOps();
  ops.udp_port[0] = 5000;
  int ret = -1;
  std::string log = ut_session::capture_stderr([&] { ret = ut_txv_ops_check(&ops); });
  EXPECT_EQ(ret, 0);
  EXPECT_NE(log.find("udp port 5000"), std::string::npos) << "no port warning: " << log;
}

TEST_F(IpmxTxValidateTest, TwoPortsFrom1025To5000WarnOncePerPort) {
  struct st20_tx_ops ops = TxOps(2);
  ops.udp_port[MTL_SESSION_PORT_P] = 4000;
  ops.udp_port[MTL_SESSION_PORT_R] = 4002;
  int ret = -1;
  std::string log = ut_session::capture_stderr([&] { ret = ut_txv_ops_check(&ops); });
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(ut_ipmx::CountOf(log, ut_ipmx::kPortWarning), 2) << log;
}

TEST_F(IpmxTxValidateTest, LinearPacingRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.pacing = ST21_PACING_LINEAR;
  EXPECT_EQ(ut_txv_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxTxValidateTest, WidePacingAccepted) {
  struct st20_tx_ops ops = TxOps();
  ops.pacing = ST21_PACING_WIDE;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, RtpLevelRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.type = ST20_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut_txv_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxTxValidateTest, InterlacedRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.interlaced = true;
  EXPECT_EQ(ut_txv_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxTxValidateTest, NonRfc4175FormatsRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.fmt = ST20_FMT_V210;
  EXPECT_EQ(ut_txv_ops_check(&ops), -ENOTSUP);
  ops.fmt = ST20_FMT_YUV_422_PLANAR10LE;
  EXPECT_EQ(ut_txv_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxTxValidateTest, FlagOffNonRfc4175FormatAccepted) {
  struct st20_tx_ops ops = TxOps();
  ops.flags = 0;
  ops.fmt = ST20_FMT_V210;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, EnableRtcpRejected) {
  struct st20_tx_ops ops = TxOps();
  ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  EXPECT_EQ(ut_txv_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxTxValidateTest, FlagOffEnableRtcpAccepted) {
  struct st20_tx_ops ops = TxOps();
  ops.flags = ST20_TX_FLAG_ENABLE_RTCP;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

TEST_F(IpmxTxValidateTest, NonD6ProgressiveRateAccepted) {
  struct st20_tx_ops ops = TxOps();
  ops.fps = ST_FPS_P25;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
  ops.fps = ST_FPS_P29_97;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

class IpmxTxUpdateDestinationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
    ASSERT_EQ(Update(ut_ipmx::kIpmxPort), 0);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  int Update(uint16_t udp_port) {
    uint16_t src_port = 0, dst_port = 0;
    return ut_txv_update_dst(ctx_, 0, ut_ipmx::kIpmxPort, udp_port, &src_port, &dst_port);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(IpmxTxUpdateDestinationTest, OddPortRejectedAndHeaderKept) {
  EXPECT_EQ(Update(20001), -EINVAL);
  EXPECT_EQ(ut_txv_hdr_dst_port(ctx_, MTL_SESSION_PORT_P), ut_ipmx::kIpmxPort);
}

TEST_F(IpmxTxUpdateDestinationTest, PortAtOrBelow1024RejectedAndHeaderKept) {
  EXPECT_EQ(Update(1000), -EINVAL);
  EXPECT_EQ(ut_txv_hdr_dst_port(ctx_, MTL_SESSION_PORT_P), ut_ipmx::kIpmxPort);
}

TEST_F(IpmxTxUpdateDestinationTest, PortFrom1025To5000AcceptedWithWarning) {
  int ret = -1;
  std::string log = ut_session::capture_stderr([&] { ret = Update(4000); });
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(ut_txv_hdr_dst_port(ctx_, MTL_SESSION_PORT_P), 4000);
  EXPECT_EQ(ut_ipmx::CountOf(log, ut_ipmx::kPortWarning), 1) << log;
}

TEST(IpmxTxUpdateDestinationFlagOffTest, OddPortAccepted) {
  ASSERT_EQ(ut_txv_init(), 0);
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(ctx, nullptr);
  uint16_t src_port = 0, dst_port = 0;
  EXPECT_EQ(ut_txv_update_dst(ctx, 0, ut_ipmx::kIpmxPort, 20001, &src_port, &dst_port),
            0);
  EXPECT_EQ(dst_port, 20001);
  ut_txv_destroy(ctx);
}

class IpmxTxFormatTest : public ::testing::TestWithParam<IpmxFormat> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
  }
};

TEST_P(IpmxTxFormatTest, SupportedFormatAccepted) {
  const IpmxFormat& f = GetParam();
  struct st20_tx_ops ops = TxOps();
  ops.width = f.width;
  ops.height = f.height;
  ops.fps = f.fps;
  ops.fmt = f.fmt;
  ops.type = f.type;
  ops.packing = f.packing;
  EXPECT_EQ(ut_txv_ops_check(&ops), 0);
}

INSTANTIATE_TEST_SUITE_P(Matrix, IpmxTxFormatTest, ::testing::ValuesIn(kIpmxFormats),
                         ut_ipmx::FormatName);

}  // namespace
