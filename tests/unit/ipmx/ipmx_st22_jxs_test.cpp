/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX JPEG XS (TR-10-11) box values and picture header check of an st22 TX session.
 *
 * With ST22_TX_FLAG_IPMX the session declares the TR-08 section 8.1.1 IPMX-JPEG-XS
 * codestream in its RFC 9134 boxes: jxpl Ppih High444.12 (0x4a40) and the Plev picked
 * at create from width, height, frame rate and maximum codestream size (ISO/IEC 21122-2
 * Table A.8 levels, TR-08 allows 1k-1, 2k-1, 4k-2, 8k-2; Sublev3bpp up to 3 bpp,
 * Sublev4bpp up to 4 bpp), schar RGB 4:4:4 8-bit and colr BT.709 / identity / narrow
 * (ISO/IEC 21122-3 A.5.3, Tables A.9, A.20, A.21), brat in Mbit/s rounded up. Every
 * frame's codestream is then checked against those values (ISO/IEC 21122-1 Annex A
 * SOC, CAP, PIH, CDT; the TR-08 section 8.1.1 coding parameters; RFC 9134 section 3.3:
 * the box level is exact, its sublevel a limit) before it is sent; a frame that fails
 * is returned to the app and counted.
 *
 * Groups:
 *   IpmxJxsPlevTest     ipmx_jxs_plev() level and sublevel choice, rejects
 *   IpmxJxsOpsTest      the same rejects through tv_st22_ops_check() (st22_tx_create())
 *   IpmxJxsBoxesTest    the 60 box bytes tv_init_st22_boxes() writes, IPMX on and off
 *   IpmxJxsPihTest      ipmx_jxs_pih_check() on a real SVT-JPEG-XS header and broken ones
 *   IpmxJxsBuilderTest  tv_tasklet_st22() sends a good frame, drops and counts a bad one
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxJxs*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ipmx/ipmx_jxs.h"
#include "ipmx/ipmx_test_ops.h"
#include "session/st20_tx_harness.h"
#include "session/stderr_capture.h"

namespace {

constexpr size_t Bytes(uint32_t w, uint32_t h, uint32_t bpp) {
  return static_cast<size_t>(w) * h * bpp / 8;
}

/* ---- Plev: ISO/IEC 21122-2 Table A.8 limits, TR-08 section 8.1.1 choice ---- */

struct PlevCase {
  const char* name;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  size_t codestream;
  uint16_t plev;
};

class IpmxJxsPlevTest : public ::testing::TestWithParam<PlevCase> {};

/* every D6 size and rate gets the lowest level that fits; 2k-1 holds 1080p up to
 * 133.7 M pixels/s, 4k-2 2160p up to 534.8 M pixels/s; 3 bpp is Sublev3bpp (0x04),
 * up to 4 bpp Sublev4bpp (0x06) */
TEST_P(IpmxJxsPlevTest, LowestFittingLevelAndSublevel) {
  const PlevCase& c = GetParam();
  uint16_t plev = 0;
  ASSERT_EQ(ipmx_jxs_plev(c.width, c.height, c.fps, c.codestream, &plev), 0);
  EXPECT_EQ(plev, c.plev);
}

const PlevCase kPlevCases[] = {
    {"P1080p50_3bpp", 1920, 1080, ST_FPS_P50, Bytes(1920, 1080, 3), 0x1004},
    {"P1080p5994_3bpp", 1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 3), 0x1004},
    {"P1080p60_3bpp", 1920, 1080, ST_FPS_P60, Bytes(1920, 1080, 3), 0x1004},
    {"P1080p50_4bpp", 1920, 1080, ST_FPS_P50, Bytes(1920, 1080, 4), 0x1006},
    {"P1080p5994_4bpp", 1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 4), 0x1006},
    {"P1080p60_4bpp", 1920, 1080, ST_FPS_P60, Bytes(1920, 1080, 4), 0x1006},
    {"P2160p5994_3bpp", 3840, 2160, ST_FPS_P59_94, Bytes(3840, 2160, 3), 0x2404},
    {"P2160p60_3bpp", 3840, 2160, ST_FPS_P60, Bytes(3840, 2160, 3), 0x2404},
    {"P2160p5994_4bpp", 3840, 2160, ST_FPS_P59_94, Bytes(3840, 2160, 4), 0x2406},
    {"P2160p60_4bpp", 3840, 2160, ST_FPS_P60, Bytes(3840, 2160, 4), 0x2406},
    /* below 3 bpp is still Sublev3bpp, one byte above 3 bpp needs Sublev4bpp */
    {"P1080p60_2bpp", 1920, 1080, ST_FPS_P60, Bytes(1920, 1080, 2), 0x1004},
    {"P1080p60_3bppPlusOneByte", 1920, 1080, ST_FPS_P60, Bytes(1920, 1080, 3) + 1,
     0x1006},
    /* outside D6: 720p60 fits 1k-1 (1280 wide, 83.6 M pixels/s), 1080p120 is above
     * the 2k-1 rate, 2160p120 above the 4k-2 rate */
    {"P720p60_3bpp", 1280, 720, ST_FPS_P60, Bytes(1280, 720, 3), 0x0404},
    {"P1080p120_3bpp", 1920, 1080, ST_FPS_P120, Bytes(1920, 1080, 3), 0x2404},
    {"P2160p120_3bpp", 3840, 2160, ST_FPS_P120, Bytes(3840, 2160, 3), 0x3404},
};

INSTANTIATE_TEST_SUITE_P(Ipmx, IpmxJxsPlevTest, ::testing::ValuesIn(kPlevCases),
                         [](const ::testing::TestParamInfo<PlevCase>& info) {
                           return std::string(info.param.name);
                         });

/* D11: above 4 bpp is not an IPMX MVP stream */
TEST(IpmxJxsPlevRejectTest, AboveFourBppRejected) {
  uint16_t plev = 0;
  EXPECT_EQ(ipmx_jxs_plev(1920, 1080, ST_FPS_P60, Bytes(1920, 1080, 4) + 1, &plev),
            -ENOTSUP);
}

/* 8k-2, the highest TR-08 level, holds 2139 M pixels/s; 4320p120 is above it */
TEST(IpmxJxsPlevRejectTest, RateAboveEightK2Rejected) {
  uint16_t plev = 0;
  EXPECT_EQ(ipmx_jxs_plev(7680, 4320, ST_FPS_P120, Bytes(7680, 4320, 3), &plev),
            -ENOTSUP);
}

/* 8k-2 is at most 8192 pixels wide */
TEST(IpmxJxsPlevRejectTest, WidthAboveEightK2Rejected) {
  uint16_t plev = 0;
  EXPECT_EQ(ipmx_jxs_plev(8200, 1080, ST_FPS_P25, Bytes(8200, 1080, 3), &plev), -ENOTSUP);
}

/* ---- create-time reject through tv_st22_ops_check() ---- */

/* st22_tx_create() with ST22_TX_FLAG_IPMX fails before it allocates anything */
class IpmxJxsOpsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
  }
};

/* a session sized for more than 4 bpp fails with -ENOTSUP and says why */
TEST_F(IpmxJxsOpsTest, AboveFourBppRejected) {
  struct st22_tx_ops ops = ut_ipmx::St22TxOps();
  ops.framebuff_max_size = Bytes(1920, 1080, 5);
  int ret = 0;
  std::string log =
      ut_session::capture_stderr([&] { ret = ut_txv_st22_ops_check(&ops); });
  EXPECT_EQ(ret, -ENOTSUP);
  EXPECT_NE(log.find("4 bpp"), std::string::npos) << log;
}

/* a session no TR-08 level fits fails with -ENOTSUP */
TEST_F(IpmxJxsOpsTest, NoLevelFitsRejected) {
  struct st22_tx_ops ops = ut_ipmx::St22TxOps();
  ops.width = 7680;
  ops.height = 4320;
  ops.fps = ST_FPS_P120;
  ops.framebuff_max_size = Bytes(7680, 4320, 3);
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), -ENOTSUP);
}

/* the 4 bpp 1080p59.94 session of the TR-08 Appendix B RGB interoperability point is
 * accepted */
TEST_F(IpmxJxsOpsTest, FourBppAccepted) {
  struct st22_tx_ops ops = ut_ipmx::St22TxOps();
  ops.framebuff_max_size = Bytes(1920, 1080, 4);
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);
}

/* without the flag the size is not checked, current MTL behaviour */
TEST_F(IpmxJxsOpsTest, FlagOffAboveFourBppAccepted) {
  struct st22_tx_ops ops = ut_ipmx::St22TxOps();
  ops.flags = 0;
  ops.framebuff_max_size = Bytes(1920, 1080, 8);
  EXPECT_EQ(ut_txv_st22_ops_check(&ops), 0);
}

/* ---- the box bytes ---- */

using Boxes = std::vector<uint8_t>;

class IpmxJxsBoxesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  Boxes Build(uint32_t w, uint32_t h, enum st_fps fps, size_t cs, bool ipmx) {
    uint8_t b[UT_TXV_ST22_BOXES_LEN];
    memset(b, 0xee, sizeof(b));
    EXPECT_EQ(ut_txv_st22_boxes(ctx_, w, h, fps, cs, ipmx, b), 0);
    return Boxes(b, b + sizeof(b));
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* 1080p59.94 at 3 bpp: brat ceil(8 * (60 + 777600) * 60000 / 1001 / 1e6) = 373
 * (0x175), frat 60/2 progressive, schar 0x8072, tcod 0, jxpl 0x4a40 / 0x1004, colr
 * METH 5, CP 1, TC 1, MC 0, V 0 */
TEST_F(IpmxJxsBoxesTest, Golden1080p5994ThreeBpp) {
  const Boxes expect = {
      0x00, 0x00, 0x00, 0x2a, 'j',  'p',  'v',  's',                          /* jpvs */
      0x00, 0x00, 0x00, 0x16, 'j',  'p',  'v',  'i',                          /* jpvi */
      0x00, 0x00, 0x01, 0x75, 0x02, 0x00, 0x00, 0x3c, 0x80, 0x72,             /* brat */
      0x00, 0x00, 0x00, 0x00,                                                 /* tcod */
      0x00, 0x00, 0x00, 0x0c, 'j',  'x',  'p',  'l',  0x4a, 0x40, 0x10, 0x04, /* jxpl */
      0x00, 0x00, 0x00, 0x12, 'c',  'o',  'l',  'r',                          /* colr */
      0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(Build(1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 3), true), expect);
}

/* 2160p59.94 at 4 bpp: brat ceil(8 * (60 + 4147200) * 60000 / 1001 / 1e6) = 1989
 * (0x7c5), jxpl 0x4a40 / 0x2406 */
TEST_F(IpmxJxsBoxesTest, Golden2160p5994FourBpp) {
  const Boxes expect = {
      0x00, 0x00, 0x00, 0x2a, 'j',  'p',  'v',  's',                          /* jpvs */
      0x00, 0x00, 0x00, 0x16, 'j',  'p',  'v',  'i',                          /* jpvi */
      0x00, 0x00, 0x07, 0xc5, 0x02, 0x00, 0x00, 0x3c, 0x80, 0x72,             /* brat */
      0x00, 0x00, 0x00, 0x00,                                                 /* tcod */
      0x00, 0x00, 0x00, 0x0c, 'j',  'x',  'p',  'l',  0x4a, 0x40, 0x24, 0x06, /* jxpl */
      0x00, 0x00, 0x00, 0x12, 'c',  'o',  'l',  'r',                          /* colr */
      0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(Build(3840, 2160, ST_FPS_P59_94, Bytes(3840, 2160, 4), true), expect);
}

/* TR-08 section 8.1.2.1: brat is the maximum rate, so the 4 bpp 1080p59.94 rate
 * 497.2 Mbit/s rounds up to 498 */
TEST_F(IpmxJxsBoxesTest, BratRoundsUp) {
  Boxes b = Build(1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 4), true);
  uint32_t brat = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
  EXPECT_EQ(brat, 498u);
}

/* without the flag the bytes stay those MTL has always sent, as captured from a
 * 1080p59.94 4 bpp session (brat in MiB/s rounded down, Main 422.10 / 4k-1, YCbCr 4:2:2
 * 10-bit, colr MC 1 and V 0x80) */
TEST_F(IpmxJxsBoxesTest, FlagOffBytesUnchanged) {
  const Boxes expect = {
      0x00, 0x00, 0x00, 0x2a, 'j',  'p',  'v',  's',  0x00, 0x00, 0x00, 0x16,
      'j',  'p',  'v',  'i',  0x00, 0x00, 0x01, 0xda, 0x02, 0x00, 0x00, 0x3c,
      0x80, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 'j',  'x',
      'p',  'l',  0x35, 0x40, 0x20, 0x80, 0x00, 0x00, 0x00, 0x12, 'c',  'o',
      'l',  'r',  0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x80,
  };
  EXPECT_EQ(Build(1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 4), false), expect);
}

/* ---- the picture header check ---- */

/* The start of a High444.12 RGB 4:4:4 8-bit 1080p59.94 4 bpp codestream from
 * SVT-JPEG-XS: SOC, CAP, PIH (Lcod 1036800, Ppih 0x4a40, Plev 0x1006, 1920x1080, Nc 3,
 * Cpih 1, NL,x 5, NL,y 2, Qpih 1), CDT (B 8, sx = sy = 1, three times), WGT, SLH and the
 * first slice bytes. */
const uint8_t kSvtHeader[] = {
    0xff, 0x10, 0xff, 0x50, 0x00, 0x04, 0x00, 0x00, 0xff, 0x12, 0x00, 0x1a, 0x00, 0x0f,
    0xd2, 0x00, 0x4a, 0x40, 0x10, 0x06, 0x07, 0x80, 0x04, 0x38, 0x00, 0x00, 0x00, 0x04,
    0x03, 0x04, 0x08, 0x14, 0x84, 0x01, 0x52, 0x10, 0xff, 0x13, 0x00, 0x08, 0x08, 0x11,
    0x08, 0x11, 0x08, 0x11, 0xff, 0x14, 0x00, 0x3e, 0x04, 0x0c, 0x03, 0x0f, 0x03, 0x0e,
    0x03, 0x03, 0x02, 0x0b, 0x02, 0x0a, 0x03, 0x18, 0x02, 0x1a, 0x02, 0x1b, 0x02, 0x00,
    0x01, 0x04, 0x01, 0x05, 0x02, 0x12, 0x01, 0x15, 0x01, 0x14, 0x02, 0x13, 0x01, 0x17,
    0x01, 0x16, 0x01, 0x0d, 0x00, 0x10, 0x00, 0x11, 0x01, 0x02, 0x00, 0x09, 0x00, 0x06,
    0x01, 0x01, 0x00, 0x07, 0x00, 0x08, 0x01, 0x19, 0x00, 0x1c, 0x00, 0x1d, 0xff, 0x20,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x68, 0x05, 0x18, 0x00, 0x00, 0x00, 0x02, 0xaa,
    0x80, 0xaa, 0xa0, 0x04, 0xfa, 0x09, 0xf8, 0x00, 0xff, 0xef, 0xfd, 0xff, 0xbf, 0xf7,
    0xfe, 0xff, 0xdf, 0xfb, 0xff, 0x7f, 0xef, 0xfd, 0xff, 0xbf, 0xf7, 0xfe, 0xff, 0xdf,
    0xfb, 0xff, 0xdf, 0xfd, 0xff, 0xdf,
};
constexpr uint16_t kSvtPlev = 0x1006;
/* byte offsets in kSvtHeader */
constexpr size_t kCap = 2, kPih = 8, kLpih = 10, kPpih = 16, kPlev = 18, kWf = 20;
constexpr size_t kHf = 22, kNc = 28, kCpih = 33, kNl = 34, kQpih = 35;
constexpr size_t kCdt = 36, kLcdt = 38, kCdtB1 = 42, kCdtS2 = 45;
constexpr size_t kCdtEnd = 46;

class IpmxJxsPihTest : public ::testing::Test {
 protected:
  std::vector<uint8_t> cs_{kSvtHeader, kSvtHeader + sizeof(kSvtHeader)};
  /* an exact size heap copy, so a read past size is a heap overflow */
  int Check(size_t size, uint16_t plev = kSvtPlev, uint32_t w = 1920, uint32_t h = 1080) {
    std::vector<uint8_t> exact(cs_.begin(), cs_.begin() + size);
    return ipmx_jxs_pih_check(exact.data(), size, w, h, plev);
  }
  int Check() {
    return Check(cs_.size());
  }
};

/* a real SVT-JPEG-XS High444.12 RGB 8-bit header passes */
TEST_F(IpmxJxsPihTest, RealSvtHeaderPasses) {
  EXPECT_EQ(Check(), 0);
}

/* a codestream must start with SOC (ISO/IEC 21122-1 A.4.1) */
TEST_F(IpmxJxsPihTest, NoSocFails) {
  cs_[1] = 0x11;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NO_SOC);
}

/* Main 444.12 (0x3a40) is not the TR-08 RGB profile */
TEST_F(IpmxJxsPihTest, OtherProfileFails) {
  cs_[kPpih] = 0x3a;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_PPIH);
}

/* the level is exact: 4k-2 in the codestream, 2k-1 in the box */
TEST_F(IpmxJxsPihTest, OtherLevelFails) {
  cs_[kPlev] = 0x24;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_LEVEL);
}

/* level 0, unrestricted, is not a TR-08 level */
TEST_F(IpmxJxsPihTest, LevelZeroFails) {
  cs_[kPlev] = 0x00;
  EXPECT_EQ(Check(cs_.size(), 0x0006), -IPMX_JXS_PIH_LEVEL);
}

/* RFC 9134 section 3.3: the box sublevel is a limit, a frame of at most 3 bpp with
 * Sublev3bpp fits a Sublev4bpp box */
TEST_F(IpmxJxsPihTest, TighterSublevelPasses) {
  cs_[kPlev + 1] = 0x04;
  EXPECT_EQ(Check(cs_.size(), 0x1006), 0);
}

/* a Sublev4bpp codestream exceeds a Sublev3bpp box */
TEST_F(IpmxJxsPihTest, LooserSublevelFails) {
  EXPECT_EQ(Check(cs_.size(), 0x1004), -IPMX_JXS_PIH_SUBLEVEL);
}

/* TR-08 section 8.1.1: above 3 bpp is Sublev4bpp, so a 4 bpp frame with Sublev3bpp fails
 * even in a Sublev4bpp box */
TEST_F(IpmxJxsPihTest, FourBppFrameWithSublev3bppFails) {
  cs_[kPlev + 1] = 0x04;
  cs_.resize(Bytes(1920, 1080, 4));
  EXPECT_EQ(Check(cs_.size(), 0x1006), -IPMX_JXS_PIH_SUBLEVEL_RATE);
}

/* the 4 bpp frame with Sublev4bpp passes */
TEST_F(IpmxJxsPihTest, FourBppFrameWithSublev4bppPasses) {
  cs_.resize(Bytes(1920, 1080, 4));
  EXPECT_EQ(Check(), 0);
}

/* exactly 3 bpp (777600 bytes at 1080p) is still Sublev3bpp */
TEST_F(IpmxJxsPihTest, ExactThreeBppFrameWithSublev3bppPasses) {
  cs_[kPlev + 1] = 0x04;
  cs_.resize(Bytes(1920, 1080, 3));
  EXPECT_EQ(Check(cs_.size(), 0x1004), 0);
}

/* TR-08 section 8.1.1 allows Sublev2bpp only for capability sets E and F, and needs
 * Sublev3bpp up to 3 bpp */
TEST_F(IpmxJxsPihTest, Sublev2bppFails) {
  cs_[kPlev + 1] = 0x03;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_SUBLEVEL);
}

/* only Sublev3bpp and Sublev4bpp are allowed; 0x05 is neither */
TEST_F(IpmxJxsPihTest, OtherSublevelCodeFails) {
  cs_[kPlev + 1] = 0x05;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_SUBLEVEL);
}

/* Wf 1280 in a 1920 wide session */
TEST_F(IpmxJxsPihTest, OtherWidthFails) {
  cs_[kWf] = 0x05;
  cs_[kWf + 1] = 0x00;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_WF);
}

/* Hf 720 in a 1080 high session */
TEST_F(IpmxJxsPihTest, OtherHeightFails) {
  cs_[kHf] = 0x02;
  cs_[kHf + 1] = 0xd0;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_HF);
}

/* TR-08 Table 1 RGB: Cpih 1; Cpih 0 is no colour transform */
TEST_F(IpmxJxsPihTest, NoColourTransformFails) {
  cs_[kCpih] &= 0xf0;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_CPIH);
}

/* RGB is 3 components */
TEST_F(IpmxJxsPihTest, FourComponentsFails) {
  cs_[kNc] = 4;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NC);
}

/* D11: 8-bit only, a 10-bit component fails */
TEST_F(IpmxJxsPihTest, TenBitComponentFails) {
  cs_[kCdtB1] = 10;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_B);
}

/* 4:4:4 only, a horizontally subsampled component fails */
TEST_F(IpmxJxsPihTest, SubsampledComponentFails) {
  cs_[kCdtS2] = 0x21;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_SAMPLING);
}

/* TR-08 section 8.1.1: NL,x 5; 4 horizontal decompositions fail */
TEST_F(IpmxJxsPihTest, OtherHorizontalDecompositionFails) {
  cs_[kNl] = 0x42;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NLX);
}

/* TR-08 section 8.1.1: NL,y 2; 1 vertical decomposition fails */
TEST_F(IpmxJxsPihTest, OtherVerticalDecompositionFails) {
  cs_[kNl] = 0x51;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NLY);
}

/* TR-08 section 8.1.1: Qpih 1 uniform; the deadzone quantizer (0) fails */
TEST_F(IpmxJxsPihTest, DeadzoneQuantizerFails) {
  cs_[kQpih] &= ~0x30;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_QPIH);
}

/* ISO/IEC 21122-1 A.4.3: CAP is the second marker segment; a COM there fails */
TEST_F(IpmxJxsPihTest, MissingCapFails) {
  cs_[kCap + 1] = 0x15;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NO_CAP);
}

/* ISO/IEC 21122-1 A.4.4: PIH directly follows CAP; a COM segment between fails */
TEST_F(IpmxJxsPihTest, SegmentBetweenCapAndPihFails) {
  const uint8_t com[] = {0xff, 0x15, 0x00, 0x02};
  cs_.insert(cs_.begin() + kPih, com, com + sizeof(com));
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NO_PIH);
}

/* a CDT where the PIH must be fails */
TEST_F(IpmxJxsPihTest, ComponentTableBeforePihFails) {
  std::vector<uint8_t> cdt(cs_.begin() + kCdt, cs_.begin() + kCdtEnd);
  cs_.insert(cs_.begin() + kPih, cdt.begin(), cdt.end());
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NO_PIH);
}

/* ISO/IEC 21122-1 A.4.4: exactly one PIH; a second one before the CDT fails */
TEST_F(IpmxJxsPihTest, SecondPihFails) {
  std::vector<uint8_t> pih(cs_.begin() + kPih, cs_.begin() + kCdt);
  cs_.insert(cs_.begin() + kCdt, pih.begin(), pih.end());
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_REPEATED);
}

/* ISO/IEC 21122-1 A.4.3: exactly one CAP; a second one before the CDT fails */
TEST_F(IpmxJxsPihTest, SecondCapFails) {
  std::vector<uint8_t> cap(cs_.begin() + kCap, cs_.begin() + kPih);
  cs_.insert(cs_.begin() + kCdt, cap.begin(), cap.end());
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_REPEATED);
}

/* ISO/IEC 21122-1 A.4.1: only one SOC; a second one before the CDT fails */
TEST_F(IpmxJxsPihTest, SecondSocFails) {
  const uint8_t soc[] = {0xff, 0x10};
  cs_.insert(cs_.begin() + kCdt, soc, soc + sizeof(soc));
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_REPEATED);
}

/* ISO/IEC 21122-1 Table A.7: Lpih is 26 */
TEST_F(IpmxJxsPihTest, OtherPihLengthFails) {
  cs_[kLpih + 1] = 0x1c;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_LPIH);
}

/* ISO/IEC 21122-1 Table A.15: Lcdt is 2 * Nc + 2 */
TEST_F(IpmxJxsPihTest, OtherComponentTableLengthFails) {
  cs_[kLcdt + 1] = 0x0a;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_LCDT);
}

/* a segment that does not start with a 0xff marker byte fails */
TEST_F(IpmxJxsPihTest, NonMarkerFails) {
  cs_[kCdt] = 0x00;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_BAD_SEGMENT);
}

/* a slice header before any CDT means there is no component table */
TEST_F(IpmxJxsPihTest, SliceBeforeComponentTableFails) {
  cs_[36] = 0xff;
  cs_[37] = 0x20;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_NO_CDT);
}

/* a segment length that runs past the codestream is refused, not followed */
TEST_F(IpmxJxsPihTest, CapLengthPastEndFails) {
  cs_[4] = 0xff;
  cs_[5] = 0xff;
  EXPECT_EQ(Check(), -IPMX_JXS_PIH_TRUNCATED);
}

/* every prefix that ends before the CDT does fails as truncated and reads
 * nothing past its size */
TEST_F(IpmxJxsPihTest, TruncatedHeaderFails) {
  for (size_t size = 0; size < kCdtEnd; size++)
    EXPECT_EQ(Check(size), -IPMX_JXS_PIH_TRUNCATED) << "size " << size;
}

/* the check stops at the CDT, the codestream may end right after it */
TEST_F(IpmxJxsPihTest, EndRightAfterComponentTablePasses) {
  EXPECT_EQ(Check(kCdtEnd), 0);
}

/* ---- the builder ---- */

/* tv_tasklet_st22() of an IPMX session, the frame start with its PIH check */
class IpmxJxsBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  struct ut_txv_st22_frame_result Run(const std::vector<uint8_t>& cs) {
    struct ut_txv_st22_frame_result r = {};
    EXPECT_EQ(
        ut_txv_run_st22_ipmx_frame(ctx_, 1920, 1080, kSvtPlev, cs.data(), cs.size(), &r),
        0);
    return r;
  }
  ut_txv_ctx* ctx_ = nullptr;
  std::vector<uint8_t> good_{kSvtHeader, kSvtHeader + sizeof(kSvtHeader)};
};

/* a frame that passes is taken for sending and not returned */
TEST_F(IpmxJxsBuilderTest, GoodFrameSent) {
  struct ut_txv_st22_frame_result r = Run(good_);
  EXPECT_TRUE(r.started);
  EXPECT_EQ(r.notify_frame_done_calls, 0);
  EXPECT_EQ(r.pih_fail, 0u);
}

/* a frame that fails is not sent, goes back through notify_frame_done and is
 * counted */
TEST_F(IpmxJxsBuilderTest, BadFrameDroppedNotifiedAndCounted) {
  std::vector<uint8_t> bad = good_;
  bad[kPpih] = 0x3a;
  struct ut_txv_st22_frame_result r = Run(bad);
  EXPECT_FALSE(r.started);
  EXPECT_EQ(r.notify_frame_done_calls, 1);
  EXPECT_EQ(r.pih_fail, 1u);
}

/* every dropped frame is counted, the err() is logged once per stat period so the
 * data plane is not flooded */
TEST_F(IpmxJxsBuilderTest, RepeatedBadFramesLogOnce) {
  std::vector<uint8_t> bad = good_;
  bad[kNc] = 4;
  struct ut_txv_st22_frame_result r = {};
  std::string log = ut_session::capture_stderr([&] {
    Run(bad);
    r = Run(bad);
  });
  EXPECT_EQ(r.pih_fail, 2u);
  EXPECT_EQ(ut_ipmx::CountOf(log, "JPEG XS header check fail"), 1) << log;
}

/* the session keeps running: a good frame after a bad one is sent */
TEST_F(IpmxJxsBuilderTest, GoodFrameAfterBadFrameSent) {
  std::vector<uint8_t> bad = good_;
  bad[kWf] = 0x05;
  Run(bad);
  struct ut_txv_st22_frame_result r = Run(good_);
  EXPECT_TRUE(r.started);
  EXPECT_EQ(r.pih_fail, 1u);
}

}  // namespace
