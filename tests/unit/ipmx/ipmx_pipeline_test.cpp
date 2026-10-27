/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * ST20P_TX_FLAG_IPMX / ST20P_RX_FLAG_IPMX reach the transport ops the st20p
 * pipeline creates its session with, and those ops pass or fail the session
 * validation the same way direct st20 ops do.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxPipeline*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

#include "ipmx/ipmx_test_ops.h"
#include "pipeline/st20p_harness.h"
#include "pipeline/st20p_tx_harness.h"
#include "session/st20_harness.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr int kFramebuffCnt = 2;
const uint8_t kGroup[MTL_IP_ADDR_LEN] = {239, 0, 0, 1};

class IpmxPipelineTxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20p_tx_init(), 0);
    ctx_ = ut20p_tx_ctx_create(kFramebuffCnt);
    ASSERT_NE(ctx_, nullptr);
    struct st20p_tx_ops* ops = ut20p_tx_ops(ctx_);
    ops->name = "ipmx_st20p_tx";
    ops->port.num_port = 1;
    memcpy(ops->port.dip_addr[0], kGroup, sizeof(kGroup));
    ops->port.udp_port[0] = ut_ipmx::kIpmxPort;
    ops->port.payload_type = 96;
    ops->width = 1920;
    ops->height = 1080;
    ops->fps = ST_FPS_P59_94;
    ops->transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops->transport_pacing = ST21_PACING_NARROW;
    ops->framebuff_cnt = kFramebuffCnt;
    ops->flags = ST20P_TX_FLAG_IPMX;
  }
  void TearDown() override {
    ut20p_tx_ctx_destroy(ctx_);
  }
  int CaptureAndCheck(struct st20_tx_ops* transport) {
    EXPECT_EQ(ut20p_tx_capture_transport_ops(ctx_, transport), -EIO);
    return ut_txv_ops_check(transport);
  }
  ut20p_tx_ctx* ctx_ = nullptr;
};

TEST_F(IpmxPipelineTxTest, FlagMapsToSessionFlag) {
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_TRUE(transport.flags & ST20_TX_FLAG_IPMX);
}

TEST_F(IpmxPipelineTxTest, FlagOffMapsNoSessionFlag) {
  ut20p_tx_ops(ctx_)->flags = 0;
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_FALSE(transport.flags & ST20_TX_FLAG_IPMX);
}

TEST_F(IpmxPipelineTxTest, LinearPacingRejected) {
  ut20p_tx_ops(ctx_)->transport_pacing = ST21_PACING_LINEAR;
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -EINVAL);
}

TEST_F(IpmxPipelineTxTest, OddPortRejected) {
  ut20p_tx_ops(ctx_)->port.udp_port[0] = 20001;
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -EINVAL);
}

TEST_F(IpmxPipelineTxTest, InterlacedRejected) {
  ut20p_tx_ops(ctx_)->interlaced = true;
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP);
}

class IpmxPipelineRxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0);
    ctx_ = ut20p_ctx_create(kFramebuffCnt);
    ASSERT_NE(ctx_, nullptr);
    struct st20p_rx_ops* ops = ut20p_ops(ctx_);
    ops->name = "ipmx_st20p_rx";
    ops->port.num_port = 1;
    memcpy(ops->port.ip_addr[0], kGroup, sizeof(kGroup));
    ops->port.udp_port[0] = ut_ipmx::kIpmxPort;
    ops->port.payload_type = 96;
    ops->width = 1920;
    ops->height = 1080;
    ops->fps = ST_FPS_P59_94;
    ops->transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops->framebuff_cnt = kFramebuffCnt;
    ops->flags = ST20P_RX_FLAG_IPMX;
  }
  void TearDown() override {
    ut20p_ctx_destroy(ctx_);
  }
  int CaptureAndCheck(struct st20_rx_ops* transport) {
    EXPECT_EQ(ut20p_capture_transport_ops(ctx_, transport), -EIO);
    return ut20_rx_ops_check(transport);
  }
  ut20p_ctx* ctx_ = nullptr;
};

TEST_F(IpmxPipelineRxTest, FlagMapsToSessionFlag) {
  struct st20_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_TRUE(transport.flags & ST20_RX_FLAG_IPMX);
}

TEST_F(IpmxPipelineRxTest, FlagOffMapsNoSessionFlag) {
  ut20p_ops(ctx_)->flags = 0;
  struct st20_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_FALSE(transport.flags & ST20_RX_FLAG_IPMX);
}

TEST_F(IpmxPipelineRxTest, PortAtOrBelow1024Rejected) {
  ut20p_ops(ctx_)->port.udp_port[0] = 554;
  struct st20_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -EINVAL);
}

TEST_F(IpmxPipelineRxTest, InterlacedRejected) {
  ut20p_ops(ctx_)->interlaced = true;
  struct st20_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP);
}

}  // namespace
