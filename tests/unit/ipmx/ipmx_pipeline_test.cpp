/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * ST20P_TX_FLAG_IPMX / ST20P_RX_FLAG_IPMX reach the transport ops the st20p
 * pipeline creates its session with, and those ops pass or fail the session
 * validation the same way direct st20 ops do. The st22p cases pin the same for
 * ST22P_TX_FLAG_IPMX / ST22P_RX_FLAG_IPMX and the st22 session flags.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxPipeline*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "ipmx/ipmx_test_ops.h"
#include "pipeline/st20p_harness.h"
#include "pipeline/st20p_tx_harness.h"
#include "pipeline/st22p_harness.h"
#include "pipeline/st22p_tx_harness.h"
#include "session/st20_harness.h"
#include "session/st20_tx_harness.h"
#include "session/stderr_capture.h"

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

TEST_F(IpmxPipelineTxTest, EnableRtcpRejected) {
  ut20p_tx_ops(ctx_)->flags |= ST20P_TX_FLAG_ENABLE_RTCP;
  struct st20_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP);
}

TEST_F(IpmxPipelineTxTest, NonRfc4175TransportFormatsRejected) {
  for (enum st20_fmt fmt : {ST20_FMT_V210, ST20_FMT_YUV_422_PLANAR10LE}) {
    ut20p_tx_ops(ctx_)->transport_fmt = fmt;
    struct st20_tx_ops transport;
    EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP) << fmt;
    EXPECT_EQ(transport.fmt, fmt);
  }
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

/* ST20P_RX_FLAG_ENABLE_RTCP maps to the session flag the IPMX RX rejects */
TEST_F(IpmxPipelineRxTest, EnableRtcpRejected) {
  ut20p_ops(ctx_)->flags |= ST20P_RX_FLAG_ENABLE_RTCP;
  struct st20_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP);
}

TEST_F(IpmxPipelineRxTest, NonRfc4175TransportFormatsRejected) {
  for (enum st20_fmt fmt : {ST20_FMT_V210, ST20_FMT_YUV_422_PLANAR10LE}) {
    ut20p_ops(ctx_)->transport_fmt = fmt;
    struct st20_rx_ops transport;
    EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP) << fmt;
    EXPECT_EQ(transport.fmt, fmt);
  }
}

/* st22p TX: the transport ops st22p_tx_create() passes to st22_tx_create() */
class IpmxSt22PipelineTxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut22p_tx_init(), 0);
    ctx_ = ut22p_tx_ctx_create(kFramebuffCnt);
    ASSERT_NE(ctx_, nullptr);
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "ipmx_st22p_tx";
    ops_.port.num_port = 1;
    memcpy(ops_.port.dip_addr[0], kGroup, sizeof(kGroup));
    ops_.port.udp_port[0] = ut_ipmx::kIpmxPort;
    ops_.port.payload_type = 112;
    ops_.width = 1920;
    ops_.height = 1080;
    ops_.fps = ST_FPS_P59_94;
    ops_.pack_type = ST22_PACK_CODESTREAM;
    ops_.codec = ST22_CODEC_JPEGXS;
    ops_.framebuff_cnt = kFramebuffCnt;
    ops_.flags = ST22P_TX_FLAG_IPMX;
  }
  void TearDown() override {
    ut22p_tx_ctx_destroy(ctx_);
  }
  int CaptureAndCheck(struct st22_tx_ops* transport) {
    EXPECT_EQ(ut22p_tx_capture_transport_ops(ctx_, &ops_, transport), -EIO);
    return ut_txv_st22_ops_check(transport);
  }
  ut22p_tx_ctx* ctx_ = nullptr;
  struct st22p_tx_ops ops_;
};

/* ST22P_TX_FLAG_IPMX turns on IPMX on the st22 session, the session rules apply */
TEST_F(IpmxSt22PipelineTxTest, FlagMapsToSessionFlag) {
  struct st22_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_TRUE(transport.flags & ST22_TX_FLAG_IPMX);
}

/* without the pipeline flag the session flag stays off, current MTL behaviour */
TEST_F(IpmxSt22PipelineTxTest, FlagOffMapsNoSessionFlag) {
  ops_.flags = 0;
  struct st22_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_FALSE(transport.flags & ST22_TX_FLAG_IPMX);
}

/* TR-10-11 section 7 / TR-10-2 section 7 port rule through the pipeline */
TEST_F(IpmxSt22PipelineTxTest, OddPortRejected) {
  ops_.port.udp_port[0] = 20001;
  struct st22_tx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -EINVAL);
}

/* TR-10-11 is JPEG XS: st22p_tx_create() rejects any other codec before it allocates
 * or looks up an encoder plugin */
TEST_F(IpmxSt22PipelineTxTest, NonJpegxsCodecRejected) {
  ops_.codec = ST22_CODEC_H264_CBR;
  st22p_tx_handle handle = nullptr;
  std::string log = ut_session::capture_stderr([&] { handle = ut22p_tx_create(&ops_); });
  EXPECT_EQ(handle, nullptr);
  EXPECT_NE(log.find("JPEG XS only"), std::string::npos) << log;
  EXPECT_EQ(log.find("get encoder"), std::string::npos) << log;
}

/* st22p RX: the transport ops st22p_rx_create() passes to st22_rx_create() */
class IpmxSt22PipelineRxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut22p_init(), 0);
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut22p_ctx_create(kFramebuffCnt);
    ASSERT_NE(ctx_, nullptr);
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "ipmx_st22p_rx";
    ops_.port.num_port = 1;
    memcpy(ops_.port.ip_addr[0], kGroup, sizeof(kGroup));
    ops_.port.udp_port[0] = ut_ipmx::kIpmxPort;
    ops_.port.payload_type = 112;
    ops_.width = 1920;
    ops_.height = 1080;
    ops_.fps = ST_FPS_P59_94;
    ops_.pack_type = ST22_PACK_CODESTREAM;
    ops_.codec = ST22_CODEC_JPEGXS;
    ops_.framebuff_cnt = kFramebuffCnt;
    ops_.flags = ST22P_RX_FLAG_IPMX;
  }
  void TearDown() override {
    ut22p_ctx_destroy(ctx_);
  }
  int CaptureAndCheck(struct st22_rx_ops* transport) {
    EXPECT_EQ(ut22p_rx_capture_transport_ops(ctx_, &ops_, transport), -EIO);
    return ut20_rx_st22_ops_check(transport);
  }
  ut22p_ctx* ctx_ = nullptr;
  struct st22p_rx_ops ops_;
};

/* ST22P_RX_FLAG_IPMX turns on IPMX on the st22 session, the session rules apply */
TEST_F(IpmxSt22PipelineRxTest, FlagMapsToSessionFlag) {
  struct st22_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_TRUE(transport.flags & ST22_RX_FLAG_IPMX);
}

/* without the pipeline flag the session flag stays off, current MTL behaviour */
TEST_F(IpmxSt22PipelineRxTest, FlagOffMapsNoSessionFlag) {
  ops_.flags = 0;
  struct st22_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), 0);
  EXPECT_FALSE(transport.flags & ST22_RX_FLAG_IPMX);
}

/* TR-10-11 is JPEG XS: st22p_rx_create() rejects any other codec before it allocates
 * or looks up a decoder plugin */
TEST_F(IpmxSt22PipelineRxTest, NonJpegxsCodecRejected) {
  ops_.codec = ST22_CODEC_H264_CBR;
  st22p_rx_handle handle = nullptr;
  std::string log = ut_session::capture_stderr([&] { handle = ut22p_rx_create(&ops_); });
  EXPECT_EQ(handle, nullptr);
  EXPECT_NE(log.find("JPEG XS only"), std::string::npos) << log;
  EXPECT_EQ(log.find("get decoder"), std::string::npos) << log;
}

/* RFC 9134 sections 3.4 and 4.1: ST22P_RX_FLAG_DISABLE_BOXES maps to the session flag
 * the IPMX RX rejects */
TEST_F(IpmxSt22PipelineRxTest, DisableBoxesRejected) {
  ops_.flags |= ST22P_RX_FLAG_DISABLE_BOXES;
  struct st22_rx_ops transport;
  EXPECT_EQ(CaptureAndCheck(&transport), -ENOTSUP);
}

}  // namespace
