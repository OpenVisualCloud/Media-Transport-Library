/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * The IPMX Info Block version the application sets (TR-10-1 section 8.7) and the IPMX
 * info getter of an st20 TX session.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxBlockVersion*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr_harness.h"

namespace {

constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull;
constexpr uint32_t kRtpTs = 1000;
constexpr int kPortP = 0;
constexpr size_t kInfoBlockOffset = 28;
constexpr size_t kVersion = IPMX_INFO_BLOCK_VERSION_OFFSET;

class IpmxBlockVersionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, 1, 20000);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  uint64_t FramePtp() const {
    return kTaiNs + frames_ * 16683333ull;
  }
  uint32_t FrameRtpTs() const {
    return kRtpTs + frames_ * 1501;
  }
  /* the Info Block of the SR of the next frame */
  std::vector<uint8_t> NextInfoBlock() {
    ut_sr_frame(ctx_, kPortP, FramePtp(), FrameRtpTs(), false);
    frames_++;
    return SentInfoBlock();
  }
  std::vector<uint8_t> SentInfoBlock() {
    uint8_t pkt[UT_SR_PKT_MAX];
    uint16_t len = ut_sr_last_sr(ctx_, pkt);
    const uint8_t* block = pkt + UT_SR_UDP_HDR_LEN + kInfoBlockOffset;
    EXPECT_GE(len, UT_SR_UDP_HDR_LEN + kInfoBlockOffset + IPMX_INFO_BLOCK_LEN);
    return std::vector<uint8_t>(block, block + IPMX_INFO_BLOCK_LEN);
  }
  ut_sr_ctx* ctx_ = nullptr;
  int frames_ = 0;
};

TEST_F(IpmxBlockVersionTest, DefaultIsZero) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 0);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  EXPECT_EQ(info.version, 0);
}

TEST_F(IpmxBlockVersionTest, SetAppliesToTheNextSrsWithNothingElseChanged) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  std::vector<uint8_t> before = NextInfoBlock();
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 7), 0);
  std::vector<uint8_t> after = NextInfoBlock();
  EXPECT_EQ(after[kVersion], 7);
  after[kVersion] = before[kVersion];
  EXPECT_EQ(after, before);
  EXPECT_EQ(NextInfoBlock()[kVersion], 7);
}

TEST_F(IpmxBlockVersionTest, SrBuiltBeforeTheSetKeepsTheOldVersion) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ut_sr_dequeue(ctx_, kPortP, FramePtp(), FrameRtpTs(), false);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 9), 0);
  ASSERT_EQ(ut_sr_burst_pkt0(ctx_, kPortP, FramePtp(), FrameRtpTs()), 1);
  frames_++;
  EXPECT_EQ(SentInfoBlock()[kVersion], 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 9);
}

TEST_F(IpmxBlockVersionTest, FullRangeAndAppWrapToZero) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 255), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 255);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, (uint8_t)(255 + 1)), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 0);
}

TEST_F(IpmxBlockVersionTest, GetterVersionMatchesTheWire) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 42), 0);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  EXPECT_EQ(info.version, 42);
  EXPECT_EQ(NextInfoBlock()[kVersion], info.version);
}

TEST_F(IpmxBlockVersionTest, IpmxOffSessionRejected) {
  struct ipmx_video_info info;
  EXPECT_EQ(ut_sr_set_block_version(ctx_, 1), -EIO);
  EXPECT_EQ(ut_sr_get_info(ctx_, &info), -EIO);
}

TEST_F(IpmxBlockVersionTest, NullInfoRejected) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(ut_sr_get_info(ctx_, nullptr), -EINVAL);
}

struct GetterCase {
  const char* name;
  enum st20_fmt fmt;
  const char* sampling;
  uint8_t depth;
};

class IpmxBlockVersionGetterTest : public IpmxBlockVersionTest,
                                   public ::testing::WithParamInterface<GetterCase> {};

TEST_P(IpmxBlockVersionGetterTest, InfoIsWhatTheSrCarries) {
  const GetterCase& c = GetParam();
  ASSERT_EQ(ut_sr_set_video(ctx_, c.fmt, 1920, 1080, ST20_PACKING_BPM, ST_FPS_P59_94), 0);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 3), 0);

  struct ipmx_video_info info;
  memset(&info, 0xee, sizeof(info));
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  EXPECT_EQ(info.version, 3);
  EXPECT_STREQ(info.ts_refclk, "localmac=02-00-00-00-00-01");
  EXPECT_STREQ(info.mediaclk, "direct=0");
  EXPECT_STREQ(info.sampling, c.sampling);
  EXPECT_EQ(info.depth, c.depth);
  EXPECT_FALSE(info.gpm);
  EXPECT_EQ(info.width, 1920);
  EXPECT_EQ(info.height, 1080);
  EXPECT_EQ(info.rate_num, 60000u);
  EXPECT_EQ(info.rate_den, 1001u);
  EXPECT_EQ(info.pixclk, 124291708u);
  EXPECT_EQ(info.htotal, 1920);
  EXPECT_EQ(info.vtotal, 1080);

  uint8_t block[IPMX_INFO_BLOCK_LEN];
  ASSERT_EQ(ipmx_info_block_build(&info, block), IPMX_INFO_BLOCK_LEN);
  std::vector<uint8_t> wire = NextInfoBlock();
  EXPECT_EQ(0, memcmp(wire.data(), block, sizeof(block)));
}

INSTANTIATE_TEST_SUITE_P(
    Formats, IpmxBlockVersionGetterTest,
    ::testing::Values(GetterCase{"Rgb8", ST20_FMT_RGB_8BIT, "RGB", 8},
                      GetterCase{"Rgb10", ST20_FMT_RGB_10BIT, "RGB", 10},
                      GetterCase{"Yuv422p10", ST20_FMT_YUV_422_10BIT, "YCbCr-4:2:2", 10}),
    [](const ::testing::TestParamInfo<GetterCase>& info) {
      return std::string(info.param.name);
    });

}  // namespace
