/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * The IPMX Info Block of the Sender Report (TR-10-1 section 8.7, TR-10-2 section 10)
 * and the values an st20 TX session puts in it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxInfo*'
 */

#include "ipmx/ipmx_info.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ipmx/ipmx_sr_harness.h"
#include "ipmx/ipmx_tr10_2_example.h"

namespace {

constexpr uint8_t kSip[4] = {192, 168, 1, 10};

TEST(IpmxInfoBlockTest, Tr10_2ExampleGolden) {
  struct ipmx_video_info info = {};
  info.version = 1;
  snprintf(info.ts_refclk, sizeof(info.ts_refclk), "localmac=00-20-FC-32-2F-40");
  snprintf(info.mediaclk, sizeof(info.mediaclk), "sender");
  snprintf(info.sampling, sizeof(info.sampling), "YCbCr-4:2:2");
  info.depth = 10;
  info.gpm = true;
  info.par_w = 1;
  info.par_h = 1;
  snprintf(info.range, sizeof(info.range), "NARROW");
  snprintf(info.colorimetry, sizeof(info.colorimetry), "BT709");
  snprintf(info.tcs, sizeof(info.tcs), "SDR");
  info.width = 1920;
  info.height = 1080;
  info.rate_num = 60000;
  info.rate_den = 1001;
  info.pixclk = 148550104;
  info.htotal = 2200;
  info.vtotal = 1125;

  uint8_t block[IPMX_INFO_BLOCK_LEN + 4];
  memset(block, 0xee, sizeof(block));
  ASSERT_EQ(ipmx_info_block_build(&info, block), IPMX_INFO_BLOCK_LEN);
  for (int i = 0; i < IPMX_INFO_BLOCK_LEN; i++)
    EXPECT_EQ(block[i], kTr10_2Example[i]) << "octet " << i;
  EXPECT_EQ(block[IPMX_INFO_BLOCK_LEN], 0xee); /* nothing past the block */
}

TEST(IpmxInfoBlockTest, FlagBitsAndDepthMasks) {
  struct ipmx_video_info info = {};
  info.floating = true;
  info.depth = 0x7f;
  info.gpm = true;
  info.interlaced = true;
  info.segmented = true;

  uint8_t block[IPMX_INFO_BLOCK_LEN];
  ASSERT_EQ(ipmx_info_block_build(&info, block), IPMX_INFO_BLOCK_LEN);
  const uint8_t* mib = block + 84;
  EXPECT_EQ(mib[20], 0xff); /* F=1, depth 0x7f */
  EXPECT_EQ(mib[21], 0xe0); /* M=1 I=1 S=1, reserved 0 */
}

class IpmxInfoSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, 1, 20000);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  struct ipmx_video_info Info() {
    struct ipmx_video_info info;
    memset(&info, 0xee, sizeof(info));
    EXPECT_EQ(ut_sr_video_info(ctx_, &info), 0);
    return info;
  }
  ut_sr_ctx* ctx_ = nullptr;
};

TEST_F(IpmxInfoSessionTest, FixedFieldsOfTheMvp) {
  struct ipmx_video_info info = Info();
  EXPECT_EQ(info.version, 0);
  EXPECT_STREQ(info.mediaclk, "direct=0");
  EXPECT_STREQ(info.range, "NARROW");
  EXPECT_STREQ(info.colorimetry, "BT709");
  EXPECT_STREQ(info.tcs, "SDR");
  EXPECT_EQ(info.par_w, 1);
  EXPECT_EQ(info.par_h, 1);
  EXPECT_FALSE(info.floating);
  EXPECT_FALSE(info.interlaced);
  EXPECT_FALSE(info.segmented);
}

TEST_F(IpmxInfoSessionTest, StringsZeroPaddedToTheirField) {
  struct ipmx_video_info info = Info();
  for (size_t i = strlen(info.mediaclk); i < sizeof(info.mediaclk); i++)
    EXPECT_EQ(info.mediaclk[i], 0) << "mediaclk " << i;
  for (size_t i = strlen(info.ts_refclk); i < sizeof(info.ts_refclk); i++)
    EXPECT_EQ(info.ts_refclk[i], 0) << "ts-refclk " << i;
  for (size_t i = strlen(info.colorimetry); i < sizeof(info.colorimetry); i++)
    EXPECT_EQ(info.colorimetry[i], 0) << "colorimetry " << i;
}

TEST_F(IpmxInfoSessionTest, TsRefclkIsLocalmacOfThePrimaryPort) {
  EXPECT_STREQ(Info().ts_refclk, "localmac=02-00-00-00-00-01");
  const uint8_t mac[6] = {0x00, 0x20, 0xfc, 0x32, 0x2f, 0x40};
  ut_sr_set_mac(ctx_, mac);
  EXPECT_STREQ(Info().ts_refclk, "localmac=00-20-FC-32-2F-40");
  const uint8_t hex[6] = {0xab, 0xcd, 0xef, 0x09, 0xa0, 0xff};
  ut_sr_set_mac(ctx_, hex);
  EXPECT_STREQ(Info().ts_refclk, "localmac=AB-CD-EF-09-A0-FF");
}

TEST_F(IpmxInfoSessionTest, PackingModeBitIsGeneralPacking) {
  ASSERT_EQ(ut_sr_set_video(ctx_, ST20_FMT_YUV_422_10BIT, 1920, 1080, ST20_PACKING_BPM,
                            ST_FPS_P59_94),
            0);
  EXPECT_FALSE(Info().gpm);
  ASSERT_EQ(ut_sr_set_video(ctx_, ST20_FMT_YUV_422_10BIT, 1920, 1080, ST20_PACKING_GPM,
                            ST_FPS_P59_94),
            0);
  EXPECT_TRUE(Info().gpm);
  ASSERT_EQ(ut_sr_set_video(ctx_, ST20_FMT_YUV_422_10BIT, 1920, 1080, ST20_PACKING_GPM_SL,
                            ST_FPS_P59_94),
            0);
  EXPECT_TRUE(Info().gpm);
}

TEST_F(IpmxInfoSessionTest, NonRfc4175FormatsRejected) {
  struct ipmx_video_info info;
  for (enum st20_fmt fmt : {ST20_FMT_V210, ST20_FMT_YUV_422_PLANAR10LE}) {
    ut_sr_set_video(ctx_, fmt, 1920, 1080, ST20_PACKING_BPM, ST_FPS_P59_94);
    EXPECT_EQ(ut_sr_video_info(ctx_, &info), -ENOTSUP) << fmt;
    EXPECT_EQ(ut_sr_attach(ctx_), -ENOTSUP) << fmt;
    EXPECT_FALSE(ut_sr_attached(ctx_));
  }
}

TEST_F(IpmxInfoSessionTest, BlockOfTheSessionMatchesItsInfo) {
  ut_sr_set_video(ctx_, ST20_FMT_RGB_10BIT, 3840, 2160, ST20_PACKING_GPM, ST_FPS_P60);
  struct ipmx_video_info info = Info();
  uint8_t block[IPMX_INFO_BLOCK_LEN];
  ASSERT_EQ(ipmx_info_block_build(&info, block), IPMX_INFO_BLOCK_LEN);
  const uint8_t mib[] = {
      0x00, 0x01, 0x00, 0x16, 'R', 'G', 'B', 0,    0,    0,    0,    0, 0,
      0,    0,    0,    0,    0,   0,   0,   0x0a, 0x80, 0x01, 0x01, /* depth 10, M=1, PAR
                                                                        1:1 */
  };
  EXPECT_EQ(0, memcmp(block + 84, mib, sizeof(mib)));
  const uint8_t tail[] = {
      0x0f, 0x00, 0x08, 0x70,                         /* 3840 x 2160 */
      0x00, 0x00, 0xf0, 0x01,                         /* 60 << 10 | 1 */
      0x00, 0x00, 0x00, 0x00, 0x1d, 0xa9, 0xc0, 0x00, /* 497664000 Hz */
      0x0f, 0x00, 0x08, 0x70,                         /* htotal, vtotal */
  };
  EXPECT_EQ(0, memcmp(block + IPMX_INFO_BLOCK_LEN - sizeof(tail), tail, sizeof(tail)));
}

struct SamplingCase {
  enum st20_fmt fmt;
  const char* sampling;
  uint8_t depth;
};

class IpmxInfoSamplingTest : public IpmxInfoSessionTest,
                             public ::testing::WithParamInterface<SamplingCase> {};

TEST_P(IpmxInfoSamplingTest, SamplingAndDepthOfTheFormat) {
  const SamplingCase& c = GetParam();
  ASSERT_EQ(ut_sr_set_video(ctx_, c.fmt, 1920, 1080, ST20_PACKING_BPM, ST_FPS_P59_94), 0);
  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.sampling, c.sampling);
  EXPECT_EQ(info.depth, c.depth);
}

/* ST 2110-20 section 7.4.1 sampling names */
INSTANTIATE_TEST_SUITE_P(
    AllRfc4175Formats, IpmxInfoSamplingTest,
    ::testing::Values(SamplingCase{ST20_FMT_YUV_422_8BIT, "YCbCr-4:2:2", 8},
                      SamplingCase{ST20_FMT_YUV_422_10BIT, "YCbCr-4:2:2", 10},
                      SamplingCase{ST20_FMT_YUV_422_12BIT, "YCbCr-4:2:2", 12},
                      SamplingCase{ST20_FMT_YUV_422_16BIT, "YCbCr-4:2:2", 16},
                      SamplingCase{ST20_FMT_YUV_420_8BIT, "YCbCr-4:2:0", 8},
                      SamplingCase{ST20_FMT_YUV_420_10BIT, "YCbCr-4:2:0", 10},
                      SamplingCase{ST20_FMT_YUV_420_12BIT, "YCbCr-4:2:0", 12},
                      SamplingCase{ST20_FMT_YUV_420_16BIT, "YCbCr-4:2:0", 16},
                      SamplingCase{ST20_FMT_RGB_8BIT, "RGB", 8},
                      SamplingCase{ST20_FMT_RGB_10BIT, "RGB", 10},
                      SamplingCase{ST20_FMT_RGB_12BIT, "RGB", 12},
                      SamplingCase{ST20_FMT_RGB_16BIT, "RGB", 16},
                      SamplingCase{ST20_FMT_YUV_444_8BIT, "YCbCr-4:4:4", 8},
                      SamplingCase{ST20_FMT_YUV_444_10BIT, "YCbCr-4:4:4", 10},
                      SamplingCase{ST20_FMT_YUV_444_12BIT, "YCbCr-4:4:4", 12},
                      SamplingCase{ST20_FMT_YUV_444_16BIT, "YCbCr-4:4:4", 16}));

struct D6Case {
  const char* name;
  enum st20_fmt fmt;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  const char* sampling;
  uint8_t depth;
  uint32_t rate_num;
  uint32_t rate_den;
  uint64_t pixclk; /* TR-10-9 section 10: width * height * exactframerate */
};

class IpmxInfoD6Test : public IpmxInfoSessionTest,
                       public ::testing::WithParamInterface<D6Case> {};

TEST_P(IpmxInfoD6Test, RasterRateAndPixelClock) {
  const D6Case& c = GetParam();
  ASSERT_EQ(ut_sr_set_video(ctx_, c.fmt, c.width, c.height, ST20_PACKING_BPM, c.fps), 0);
  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.sampling, c.sampling);
  EXPECT_EQ(info.depth, c.depth);
  EXPECT_EQ(info.width, c.width);
  EXPECT_EQ(info.height, c.height);
  EXPECT_EQ(info.rate_num, c.rate_num);
  EXPECT_EQ(info.rate_den, c.rate_den);
  EXPECT_EQ(info.pixclk, c.pixclk);
  EXPECT_EQ(info.htotal, c.width);
  EXPECT_EQ(info.vtotal, c.height);
}

#define UT_D6(fmt_name, fmt, sampling, depth) \
  D6Case{fmt_name "_1080p50",                 \
         fmt,                                 \
         1920,                                \
         1080,                                \
         ST_FPS_P50,                          \
         sampling,                            \
         depth,                               \
         50,                                  \
         1,                                   \
         103680000},                          \
      D6Case{fmt_name "_1080p5994",           \
             fmt,                             \
             1920,                            \
             1080,                            \
             ST_FPS_P59_94,                   \
             sampling,                        \
             depth,                           \
             60000,                           \
             1001,                            \
             124291708},                      \
      D6Case{fmt_name "_1080p60",             \
             fmt,                             \
             1920,                            \
             1080,                            \
             ST_FPS_P60,                      \
             sampling,                        \
             depth,                           \
             60,                              \
             1,                               \
             124416000},                      \
      D6Case{fmt_name "_2160p5994",           \
             fmt,                             \
             3840,                            \
             2160,                            \
             ST_FPS_P59_94,                   \
             sampling,                        \
             depth,                           \
             60000,                           \
             1001,                            \
             497166833},                      \
      D6Case{fmt_name "_2160p60",             \
             fmt,                             \
             3840,                            \
             2160,                            \
             ST_FPS_P60,                      \
             sampling,                        \
             depth,                           \
             60,                              \
             1,                               \
             497664000}

INSTANTIATE_TEST_SUITE_P(Rates, IpmxInfoD6Test,
                         ::testing::Values(UT_D6("Yuv422p10", ST20_FMT_YUV_422_10BIT,
                                                 "YCbCr-4:2:2", 10),
                                           UT_D6("Rgb8", ST20_FMT_RGB_8BIT, "RGB", 8),
                                           UT_D6("Rgb10", ST20_FMT_RGB_10BIT, "RGB", 10)),
                         [](const ::testing::TestParamInfo<D6Case>& info) {
                           return std::string(info.param.name);
                         });

}  // namespace
