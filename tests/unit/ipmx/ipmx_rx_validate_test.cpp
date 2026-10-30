/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX create-time validation of st20 RX ops, through rv_ops_check(), the
 * validation st20_rx_create() runs before it allocates anything.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxRx*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "ipmx/ipmx_test_ops.h"
#include "session/st20_harness.h"
#include "session/stderr_capture.h"

namespace {

using ut_ipmx::IpmxFormat;
using ut_ipmx::kIpmxFormats;
using ut_ipmx::RxOps;

class IpmxRxValidateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
  }
};

TEST_F(IpmxRxValidateTest, FlagOffLeavesNonIpmxOpsAccepted) {
  struct st20_rx_ops ops = RxOps(2);
  ops.flags = 0;
  ops.udp_port[0] = 1001;
  ops.udp_port[1] = 80;
  ops.interlaced = true;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);

  ops.type = ST20_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

TEST_F(IpmxRxValidateTest, EvenPortAbove5000Accepted) {
  struct st20_rx_ops ops = RxOps();
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

TEST_F(IpmxRxValidateTest, DefaultPortAccepted) {
  struct st20_rx_ops ops = RxOps();
  ops.udp_port[0] = 0; /* the library picks 10000 + 2 * idx */
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

TEST_F(IpmxRxValidateTest, OddPortRejected) {
  struct st20_rx_ops ops = RxOps();
  ops.udp_port[0] = 20001;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxRxValidateTest, PortAtOrBelow1024Rejected) {
  struct st20_rx_ops ops = RxOps();
  ops.udp_port[0] = 1000;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxRxValidateTest, BadRedundantPortRejectedWithValidPrimary) {
  struct st20_rx_ops ops = RxOps(2);
  ops.udp_port[MTL_SESSION_PORT_R] = 1022;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -EINVAL);
}

TEST_F(IpmxRxValidateTest, PortFrom1025To5000AcceptedWithWarning) {
  struct st20_rx_ops ops = RxOps();
  ops.udp_port[0] = 1026;
  int ret = -1;
  std::string log = ut_session::capture_stderr([&] { ret = ut20_rx_ops_check(&ops); });
  EXPECT_EQ(ret, 0);
  EXPECT_NE(log.find("udp port 1026"), std::string::npos) << "no port warning: " << log;
}

TEST_F(IpmxRxValidateTest, TwoPortsFrom1025To5000WarnOncePerPort) {
  struct st20_rx_ops ops = RxOps(2);
  ops.udp_port[MTL_SESSION_PORT_P] = 1030;
  ops.udp_port[MTL_SESSION_PORT_R] = 4998;
  int ret = -1;
  std::string log = ut_session::capture_stderr([&] { ret = ut20_rx_ops_check(&ops); });
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(ut_ipmx::CountOf(log, ut_ipmx::kPortWarning), 2) << log;
}

/* the NACK flow of the RX RTCP uses media port + 1, the port of the Sender Reports */
TEST_F(IpmxRxValidateTest, EnableRtcpRejected) {
  struct st20_rx_ops ops = RxOps();
  ops.flags |= ST20_RX_FLAG_ENABLE_RTCP;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -ENOTSUP);
}

/* data path only stays allowed, the session just gets no Sender Report */
TEST_F(IpmxRxValidateTest, DataPathOnlyAccepted) {
  struct st20_rx_ops ops = RxOps();
  ops.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

TEST_F(IpmxRxValidateTest, RtpLevelRejected) {
  struct st20_rx_ops ops = RxOps();
  ops.type = ST20_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxRxValidateTest, InterlacedRejected) {
  struct st20_rx_ops ops = RxOps();
  ops.interlaced = true;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxRxValidateTest, NonRfc4175FormatsRejected) {
  struct st20_rx_ops ops = RxOps();
  ops.fmt = ST20_FMT_V210;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -ENOTSUP);
  ops.fmt = ST20_FMT_YUV_422_PLANAR10LE;
  EXPECT_EQ(ut20_rx_ops_check(&ops), -ENOTSUP);
}

TEST_F(IpmxRxValidateTest, FlagOffNonRfc4175FormatAccepted) {
  struct st20_rx_ops ops = RxOps();
  ops.flags = 0;
  ops.fmt = ST20_FMT_V210;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

TEST_F(IpmxRxValidateTest, NonD6ProgressiveRateAccepted) {
  struct st20_rx_ops ops = RxOps();
  ops.fps = ST_FPS_P29_97;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

/* An accepted update re-inits the flow, which needs a NIC; only rejections run here. */
class IpmxRxUpdateSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut20_ctx_add_flags(ctx_, ST20_RX_FLAG_IPMX);
    port_ = ut20_dst_port(ctx_, MTL_SESSION_PORT_P);
  }
  void TearDown() override {
    ut20_ctx_destroy(ctx_);
  }
  ut20_test_ctx* ctx_ = nullptr;
  uint16_t port_ = 0;
};

TEST_F(IpmxRxUpdateSourceTest, OddPortRejectedAndSessionKept) {
  bool flow_kept = false;
  EXPECT_EQ(ut20_update_src_rejected(ctx_, 20001, &flow_kept), -EINVAL);
  EXPECT_TRUE(flow_kept);
  EXPECT_EQ(ut20_dst_port(ctx_, MTL_SESSION_PORT_P), port_);
}

TEST_F(IpmxRxUpdateSourceTest, PortAtOrBelow1024RejectedAndSessionKept) {
  bool flow_kept = false;
  EXPECT_EQ(ut20_update_src_rejected(ctx_, 1024, &flow_kept), -EINVAL);
  EXPECT_TRUE(flow_kept);
  EXPECT_EQ(ut20_dst_port(ctx_, MTL_SESSION_PORT_P), port_);
}

class IpmxRxFormatTest : public ::testing::TestWithParam<IpmxFormat> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
  }
};

TEST_P(IpmxRxFormatTest, SupportedFormatAccepted) {
  const IpmxFormat& f = GetParam();
  struct st20_rx_ops ops = RxOps();
  ops.width = f.width;
  ops.height = f.height;
  ops.fps = f.fps;
  ops.fmt = f.fmt;
  ops.type = f.type;
  ops.packing = f.packing;
  if (f.type == ST20_TYPE_SLICE_LEVEL) ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  EXPECT_EQ(ut20_rx_ops_check(&ops), 0);
}

INSTANTIATE_TEST_SUITE_P(Matrix, IpmxRxFormatTest, ::testing::ValuesIn(kIpmxFormats),
                         ut_ipmx::FormatName);

}  // namespace
