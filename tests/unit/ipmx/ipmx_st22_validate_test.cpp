/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX create-time validation of ST 2110-22 ops, through tv_st22_ops_check() /
 * rv_st22_ops_check(), the validation st22_tx_create() / st22_rx_create() run before
 * they allocate anything. TR-10-11 section 7 makes a JPEG XS sender and receiver follow
 * the TR-10-1 video rules, so the st22 rules mirror the st20 ones, plus the RFC 9134
 * box rule. Each rejected case also checks the err() log names the broken rule.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxSt22*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "ipmx/ipmx_test_ops.h"
#include "session/st20_harness.h"
#include "session/st20_tx_harness.h"
#include "session/stderr_capture.h"

namespace {

using ut_ipmx::St22RxOps;
using ut_ipmx::St22TxOps;

/* TX: st22_tx_create() with ST22_TX_FLAG_IPMX */
class IpmxSt22TxValidateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
  }
  /* the check result, with the log required to contain rule */
  void ExpectRejected(struct st22_tx_ops* ops, int expect, const char* rule) {
    int ret = 0;
    std::string log = ut_session::capture_stderr([&] { ret = ut_txv_st22_ops_check(ops); });
    EXPECT_EQ(ret, expect);
    EXPECT_NE(log.find(rule), std::string::npos) << "no '" << rule << "' in: " << log;
  }
};

/* without the flag none of the IPMX rules apply, current MTL behaviour is kept */
TEST_F(IpmxSt22TxValidateTest, FlagOffLeavesNonIpmxOpsAccepted) {
  struct st22_tx_ops ops = St22TxOps(2);
  ops.flags = ST22_TX_FLAG_ENABLE_RTCP | ST22_TX_FLAG_DISABLE_BOXES;
  ops.udp_port[0] = 1001;
  ops.udp_port[1] = 80;
  ops.pacing = ST21_PACING_LINEAR;
  ops.interlaced = true;
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);

  ops.type = ST22_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);
}

/* the accept path: a compliant 1080p codestream session on an even port above 5000 */
TEST_F(IpmxSt22TxValidateTest, CompliantOpsAccepted) {
  struct st22_tx_ops ops = St22TxOps(2);
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);
}

/* port 0 lets the library pick 10000 + 2 * idx, TR-10-2/TR-10-11 section 7 compliant */
TEST_F(IpmxSt22TxValidateTest, DefaultPortAccepted) {
  struct st22_tx_ops ops = St22TxOps();
  ops.udp_port[0] = 0;
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);
}

/* TR-10-11 section 7 / TR-10-2 section 7: the destination port shall be even */
TEST_F(IpmxSt22TxValidateTest, OddPortRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.udp_port[0] = 20001;
  ExpectRejected(&ops, -EINVAL, "is odd");
}

/* TR-10-11 section 7 / TR-10-2 section 7: the destination port shall be above 1024 */
TEST_F(IpmxSt22TxValidateTest, PortAtOrBelow1024Rejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.udp_port[0] = 1024;
  ExpectRejected(&ops, -EINVAL, "<= 1024");
}

/* the port rule holds on the ST 2022-7 R leg too */
TEST_F(IpmxSt22TxValidateTest, BadRedundantPortRejectedWithValidPrimary) {
  struct st22_tx_ops ops = St22TxOps(2);
  ops.udp_port[MTL_SESSION_PORT_R] = 20003;
  ExpectRejected(&ops, -EINVAL, "is odd");
}

/* TR-10-11 section 7 makes the sender comply with TR-10-1, whose section 8.1 lets IPMX
 * receivers not support linear senders (VRX does not apply, TR-10-11 section 11) */
TEST_F(IpmxSt22TxValidateTest, LinearPacingRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.pacing = ST21_PACING_LINEAR;
  ExpectRejected(&ops, -EINVAL, "ST21_PACING_LINEAR");
}

/* TR-10-1 section 8.8.2: the sender report schedule needs library built packets */
TEST_F(IpmxSt22TxValidateTest, RtpLevelRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.type = ST22_TYPE_RTP_LEVEL;
  ExpectRejected(&ops, -ENOTSUP, "ST22_TYPE_RTP_LEVEL");
}

/* interlaced is out of the IPMX MVP scope */
TEST_F(IpmxSt22TxValidateTest, InterlacedRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.interlaced = true;
  ExpectRejected(&ops, -ENOTSUP, "interlaced");
}

/* the NACK RTCP port is media port + 1, which TR-10-1 section 8.7 reserves for the
 * sender report */
TEST_F(IpmxSt22TxValidateTest, EnableRtcpRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.flags |= ST22_TX_FLAG_ENABLE_RTCP;
  ExpectRejected(&ops, -ENOTSUP, "ST22_TX_FLAG_ENABLE_RTCP");
}

/* RFC 9134 sections 3.4 and 4.1: a picture segment is the VS box, the CS box and the
 * codestream, and in codestream mode the packetization unit is the whole segment */
TEST_F(IpmxSt22TxValidateTest, DisableBoxesRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.flags |= ST22_TX_FLAG_DISABLE_BOXES;
  ExpectRejected(&ops, -ENOTSUP, "ST22_TX_FLAG_DISABLE_BOXES");
}

/* slice packetization stays rejected by the existing non-IPMX check with the flag on */
TEST_F(IpmxSt22TxValidateTest, SlicePackingStillRejected) {
  struct st22_tx_ops ops = St22TxOps();
  ops.pack_type = ST22_PACK_SLICE;
  ExpectRejected(&ops, -EINVAL, "invalid pack_type");
}

/* RX: st22_rx_create() with ST22_RX_FLAG_IPMX */
class IpmxSt22RxValidateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
  }
  void ExpectRejected(struct st22_rx_ops* ops, int expect, const char* rule) {
    int ret = 0;
    std::string log =
        ut_session::capture_stderr([&] { ret = ut20_rx_st22_ops_check(ops); });
    EXPECT_EQ(ret, expect);
    EXPECT_NE(log.find(rule), std::string::npos) << "no '" << rule << "' in: " << log;
  }
};

/* without the flag none of the IPMX rules apply, current MTL behaviour is kept */
TEST_F(IpmxSt22RxValidateTest, FlagOffLeavesNonIpmxOpsAccepted) {
  struct st22_rx_ops ops = St22RxOps(2);
  ops.flags = ST22_RX_FLAG_ENABLE_RTCP | ST22_RX_FLAG_DISABLE_BOXES;
  ops.udp_port[0] = 1001;
  ops.udp_port[1] = 80;
  ops.interlaced = true;
  EXPECT_EQ(ut20_rx_st22_ops_check(&ops), 0);

  ops.type = ST22_TYPE_RTP_LEVEL;
  EXPECT_EQ(ut20_rx_st22_ops_check(&ops), 0);
}

/* the accept path: a compliant 1080p codestream receiver on an even port above 5000 */
TEST_F(IpmxSt22RxValidateTest, CompliantOpsAccepted) {
  struct st22_rx_ops ops = St22RxOps(2);
  EXPECT_EQ(ut20_rx_st22_ops_check(&ops), 0);
}

/* TR-10-11 section 7 / TR-10-2 section 7: the port shall be even */
TEST_F(IpmxSt22RxValidateTest, OddPortRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.udp_port[0] = 20001;
  ExpectRejected(&ops, -EINVAL, "is odd");
}

/* TR-10-11 section 7 / TR-10-2 section 7: the port shall be above 1024 */
TEST_F(IpmxSt22RxValidateTest, PortAtOrBelow1024Rejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.udp_port[0] = 554;
  ExpectRejected(&ops, -EINVAL, "<= 1024");
}

/* RTP level RX is kept out for the planned st22 sender report receive */
TEST_F(IpmxSt22RxValidateTest, RtpLevelRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.type = ST22_TYPE_RTP_LEVEL;
  ExpectRejected(&ops, -ENOTSUP, "ST22_TYPE_RTP_LEVEL");
}

/* interlaced is out of the IPMX MVP scope */
TEST_F(IpmxSt22RxValidateTest, InterlacedRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.interlaced = true;
  ExpectRejected(&ops, -ENOTSUP, "interlaced");
}

/* the NACK RTCP port is media port + 1, which TR-10-1 section 8.7 reserves for the
 * sender report */
TEST_F(IpmxSt22RxValidateTest, EnableRtcpRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.flags |= ST22_RX_FLAG_ENABLE_RTCP;
  ExpectRejected(&ops, -ENOTSUP, "ST22_RX_FLAG_ENABLE_RTCP");
}

/* RFC 9134 sections 3.4 and 4.1: an IPMX sender always sends the boxes, without the parse
 * the RX would take them as codestream */
TEST_F(IpmxSt22RxValidateTest, DisableBoxesRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.flags |= ST22_RX_FLAG_DISABLE_BOXES;
  ExpectRejected(&ops, -ENOTSUP, "ST22_RX_FLAG_DISABLE_BOXES");
}

/* slice packetization stays rejected by the existing non-IPMX check with the flag on */
TEST_F(IpmxSt22RxValidateTest, SlicePackingStillRejected) {
  struct st22_rx_ops ops = St22RxOps();
  ops.pack_type = ST22_PACK_SLICE;
  ExpectRejected(&ops, -EINVAL, "invalid pack_type");
}

}  // namespace
