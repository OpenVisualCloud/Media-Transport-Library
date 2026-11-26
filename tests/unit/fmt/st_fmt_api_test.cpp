/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * Pure-CPU format, frame, alignment, bandwidth, and frame-rate mapping unit
 * tests.
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <st20_api.h>
#include <st_pipeline_api.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

TEST(Main, bandwidth) {
  uint64_t bandwidth_1080p_mps = st20_1080p59_yuv422_10bit_bandwidth_mps();
  uint64_t bandwidth_1080p = 0;
  int ret = st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                                   false, &bandwidth_1080p);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(bandwidth_1080p / 1000 / 1000, bandwidth_1080p_mps);

  uint64_t bandwidth_720p = 0;
  ret = st20_get_bandwidth_bps(1280, 720, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94, false,
                               &bandwidth_720p);
  EXPECT_GE(ret, 0);
  EXPECT_GT(bandwidth_1080p, bandwidth_720p);
}

TEST(Main, st20_frame_size) {
  uint32_t w = 1920;
  uint32_t h = 1080;
  size_t size = st20_frame_size(ST20_FMT_YUV_422_10BIT, w, h);
  size_t expect_size = w * h * 5 / 2;
  EXPECT_EQ(size, expect_size);
}

TEST(Main, fmt_equal_transport) {
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                           ST20_FMT_YUV_422_10BIT));
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_8BIT));
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT));

  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR10LE,
                                            ST20_FMT_YUV_422_10BIT));
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_V210, ST20_FMT_YUV_422_10BIT));
  EXPECT_FALSE(
      st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR8, ST20_FMT_YUV_422_8BIT));
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_12BIT));
}

TEST(Main, fmt_frame_transport) {
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_422_10BIT),
            ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_422_8BIT), ST_FRAME_FMT_UYVY);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_RGB_8BIT), ST_FRAME_FMT_RGB8);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_444_16BIT), ST_FRAME_FMT_MAX);
}

TEST(Main, fmt_to_transport) {
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10),
            ST20_FMT_YUV_422_10BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_UYVY), ST20_FMT_YUV_422_8BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_RGB8), ST20_FMT_RGB_8BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422PLANAR10LE), ST20_FMT_MAX);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_V210), ST20_FMT_MAX);
}

TEST(Main, frame_api) {
  uint32_t w = 1920;
  uint32_t h = 1080;
  size_t szero = 0;

  for (int i = ST_FRAME_FMT_YUV_START; i < ST_FRAME_FMT_YUV_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    EXPECT_GT(st_frame_size(fmt, w, h, false), szero);
    EXPECT_GT(st_frame_fmt_planes(fmt), 0);
    EXPECT_GT(st_frame_least_linesize(fmt, w, 0), szero);
  }
  for (int i = ST_FRAME_FMT_RGB_START; i < ST_FRAME_FMT_RGB_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    EXPECT_GT(st_frame_size(fmt, w, h, false), szero);
    EXPECT_GT(st_frame_fmt_planes(fmt), 0);
    EXPECT_GT(st_frame_least_linesize(fmt, w, 0), szero);
  }
  for (int i = ST_FRAME_FMT_CODESTREAM_START; i < ST_FRAME_FMT_CODESTREAM_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    EXPECT_EQ(st_frame_size(fmt, w, h, false), szero);
    EXPECT_EQ(st_frame_fmt_planes(fmt), 1);
    EXPECT_EQ(st_frame_least_linesize(fmt, w, 0), szero);
  }

  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_YUV_END, w, h, false), szero);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_RGB_END, w, h, false), szero);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_CODESTREAM_END, w, h, false), szero);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_MAX, w, h, false), szero);
}

TEST(Main, frame_name) {
  const char* fail = "unknown";

  for (int i = ST_FRAME_FMT_YUV_START; i < ST_FRAME_FMT_YUV_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    const char* name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }
  for (int i = ST_FRAME_FMT_RGB_START; i < ST_FRAME_FMT_RGB_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    const char* name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }
  for (int i = ST_FRAME_FMT_CODESTREAM_START; i < ST_FRAME_FMT_CODESTREAM_END; i++) {
    enum st_frame_fmt fmt = (enum st_frame_fmt)i;
    const char* name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }

  EXPECT_EQ(strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV_END)), 0);
  EXPECT_EQ(strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_RGB_END)), 0);
  EXPECT_EQ(strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_CODESTREAM_END)), 0);
  EXPECT_EQ(strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_MAX)), 0);
  EXPECT_EQ(st_frame_name_to_fmt(fail), ST_FRAME_FMT_MAX);
}

TEST(Main, size_page_align) {
  size_t pg_sz = 4096;

  EXPECT_EQ(mtl_size_page_align(pg_sz * 1, pg_sz), pg_sz * 1);
  EXPECT_EQ(mtl_size_page_align(pg_sz * 1 + 100, pg_sz), pg_sz * 2);
  EXPECT_EQ(mtl_size_page_align(pg_sz * 4, pg_sz), pg_sz * 4);
  EXPECT_EQ(mtl_size_page_align(pg_sz * 4 - 1, pg_sz), pg_sz * 4);
  EXPECT_EQ(mtl_size_page_align(pg_sz * 4 + 1, pg_sz), pg_sz * 5);
}

class fps_23_98 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_23_98, conv_fps_to_st_fps_23_98_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_23_98,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 22.00),
                                           std::make_tuple(ST_FPS_MAX, 22.97),
                                           std::make_tuple(ST_FPS_P23_98, 22.98),
                                           std::make_tuple(ST_FPS_P23_98, 23.98),
                                           std::make_tuple(ST_FPS_P23_98, 23.99),
                                           std::make_tuple(ST_FPS_P24, 24.00)));

class fps_24 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_24, conv_fps_to_st_fps_24_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_24,
                         ::testing::Values(std::make_tuple(ST_FPS_P23_98, 23.00),
                                           std::make_tuple(ST_FPS_P24, 24.00),
                                           std::make_tuple(ST_FPS_P24, 24.99),
                                           std::make_tuple(ST_FPS_P25, 25.00)));

class fps_25 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_25, conv_fps_to_st_fps_25_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_25,
                         ::testing::Values(std::make_tuple(ST_FPS_P25, 25.00),
                                           std::make_tuple(ST_FPS_P25, 26.00),
                                           std::make_tuple(ST_FPS_MAX, 27.00)));

class fps_29_97 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_29_97, conv_fps_to_st_fps_29_97_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_29_97,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 28.00),
                                           std::make_tuple(ST_FPS_MAX, 28.50),
                                           std::make_tuple(ST_FPS_P29_97, 29.97),
                                           std::make_tuple(ST_FPS_P29_97, 29.99),
                                           std::make_tuple(ST_FPS_P30, 30.00)));

class fps_30 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_30, conv_fps_to_st_fps_30_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_30,
                         ::testing::Values(std::make_tuple(ST_FPS_P30, 30.00),
                                           std::make_tuple(ST_FPS_P30, 31.00),
                                           std::make_tuple(ST_FPS_MAX, 31.01),
                                           std::make_tuple(ST_FPS_MAX, 32.00)));

class fps_50 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_50, conv_fps_to_st_fps_50_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Main, fps_50,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 48.00),
                                           std::make_tuple(ST_FPS_P50, 49.00),
                                           std::make_tuple(ST_FPS_P50, 49.50),
                                           std::make_tuple(ST_FPS_P50, 50.00),
                                           std::make_tuple(ST_FPS_P50, 50.50),
                                           std::make_tuple(ST_FPS_P50, 51.00),
                                           std::make_tuple(ST_FPS_MAX, 52.00)));

class fps_59_94 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

INSTANTIATE_TEST_SUITE_P(Main, fps_59_94,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 58.93),
                                           std::make_tuple(ST_FPS_P59_94, 58.94),
                                           std::make_tuple(ST_FPS_P59_94, 59.94),
                                           std::make_tuple(ST_FPS_P59_94, 59.99),
                                           std::make_tuple(ST_FPS_P60, 60.00)));

TEST_P(fps_59_94, conv_fps_to_st_fps_59_94_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_60 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

INSTANTIATE_TEST_SUITE_P(Main, fps_60,
                         ::testing::Values(std::make_tuple(ST_FPS_P60, 60.00),
                                           std::make_tuple(ST_FPS_P60, 61.00),
                                           std::make_tuple(ST_FPS_MAX, 61.01),
                                           std::make_tuple(ST_FPS_MAX, 62.00)));

TEST_P(fps_60, conv_fps_to_st_fps_60_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_100 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

INSTANTIATE_TEST_SUITE_P(Main, fps_100,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 98.99),
                                           std::make_tuple(ST_FPS_P100, 99.00),
                                           std::make_tuple(ST_FPS_P100, 100.00),
                                           std::make_tuple(ST_FPS_P100, 101.00),
                                           std::make_tuple(ST_FPS_MAX, 101.01)));

TEST_P(fps_100, conv_fps_to_st_fps_100_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_119_98 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

INSTANTIATE_TEST_SUITE_P(Main, fps_119_98,
                         ::testing::Values(std::make_tuple(ST_FPS_MAX, 118.87),
                                           std::make_tuple(ST_FPS_P119_88, 118.88),
                                           std::make_tuple(ST_FPS_P119_88, 119.88),
                                           std::make_tuple(ST_FPS_P119_88, 119.99),
                                           std::make_tuple(ST_FPS_P120, 120.00)));

TEST_P(fps_119_98, conv_fps_to_st_fps_119_98_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_120 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

INSTANTIATE_TEST_SUITE_P(Main, fps_120,
                         ::testing::Values(std::make_tuple(ST_FPS_P120, 120.00),
                                           std::make_tuple(ST_FPS_P120, 120.01),
                                           std::make_tuple(ST_FPS_P120, 121.00),
                                           std::make_tuple(ST_FPS_MAX, 121.01),
                                           std::make_tuple(ST_FPS_MAX, 122.00)));

TEST_P(fps_120, conv_fps_to_st_fps_120_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

namespace {

struct st_frame make_rfc4175_frame(
    std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
    enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10) {
  struct st_frame frame = {};
  frame.fmt = fmt;
  frame.width = w;
  frame.height = h;
  frame.addr[0] = buf.data();
  frame.linesize[0] = static_cast<size_t>(w) / 2 * 5;
  frame.buffer_size = buf.size();
  frame.data_size = buf.size();
  return frame;
}

}  // namespace

TEST(StFmtDrawLogo, Rfc4175SubRectPreservesBorder) {
  constexpr uint32_t w = 16;
  constexpr uint32_t h = 8;
  constexpr uint32_t logo_w = 6;
  constexpr uint32_t logo_h = 4;
  constexpr uint32_t x = 4;
  constexpr uint32_t y = 2;
  constexpr uint8_t kBorderByte = 0xA5;

  const size_t frame_stride = static_cast<size_t>(w) / 2 * 5;
  const size_t logo_stride = static_cast<size_t>(logo_w) / 2 * 5;
  std::vector<uint8_t> frame_buf(frame_stride * h, kBorderByte);
  std::vector<uint8_t> logo_buf(logo_stride * logo_h);
  for (size_t i = 0; i < logo_buf.size(); ++i) {
    uint8_t val = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
    logo_buf[i] = (val == kBorderByte) ? static_cast<uint8_t>(0x5Au) : val;
  }

  struct st_frame frame = make_rfc4175_frame(frame_buf, w, h);
  struct st_frame logo = make_rfc4175_frame(logo_buf, logo_w, logo_h);

  ASSERT_EQ(st_draw_logo(&frame, &logo, x, y), 0);

  const size_t x_byte_start = static_cast<size_t>(x) / 2 * 5;
  const size_t x_byte_end = static_cast<size_t>(x + logo_w) / 2 * 5;
  for (uint32_t row = 0; row < h; ++row) {
    for (size_t col = 0; col < frame_stride; ++col) {
      const bool inside =
          (row >= y && row < y + logo_h && col >= x_byte_start && col < x_byte_end);
      const uint8_t expected =
          inside ? logo_buf[(row - y) * logo_stride + (col - x_byte_start)] : kBorderByte;
      EXPECT_EQ(frame_buf[row * frame_stride + col], expected)
          << "row=" << row << " col=" << col;
    }
  }
}

TEST(StFmtDrawLogo, Rfc4175FullFrame) {
  constexpr uint32_t w = 16;
  constexpr uint32_t h = 8;
  const size_t frame_size = static_cast<size_t>(w) / 2 * 5 * h;

  std::vector<uint8_t> frame_buf(frame_size, 0xA5);
  std::vector<uint8_t> logo_buf(frame_size);
  for (size_t i = 0; i < logo_buf.size(); ++i) {
    logo_buf[i] = static_cast<uint8_t>((i * 19u + 7u) & 0xFFu);
  }

  struct st_frame frame = make_rfc4175_frame(frame_buf, w, h);
  struct st_frame logo = make_rfc4175_frame(logo_buf, w, h);

  ASSERT_EQ(st_draw_logo(&frame, &logo, 0, 0), 0);
  EXPECT_EQ(frame_buf, logo_buf);
}

TEST(StFmtDrawLogo, RejectsOutOfBoundsAndInvalidFormats) {
  constexpr uint32_t w = 16;
  constexpr uint32_t h = 8;
  constexpr uint32_t logo_w = 8;
  constexpr uint32_t logo_h = 4;

  const size_t frame_size = static_cast<size_t>(w) / 2 * 5 * h;
  const size_t logo_size = static_cast<size_t>(logo_w) / 2 * 5 * logo_h;
  std::vector<uint8_t> frame_buf(frame_size, 0xA5);
  const std::vector<uint8_t> untouched = frame_buf;
  std::vector<uint8_t> logo_buf(logo_size, 0x3C);

  struct st_frame frame = make_rfc4175_frame(frame_buf, w, h);
  struct st_frame logo = make_rfc4175_frame(logo_buf, logo_w, logo_h);

  /* Out-of-bounds X: x + logo_w (10 + 8) > w (16). */
  EXPECT_EQ(st_draw_logo(&frame, &logo, 10, 0), -EINVAL);
  EXPECT_EQ(frame_buf, untouched);

  /* Out-of-bounds Y: y + logo_h (6 + 4) > h (8). */
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 6), -EINVAL);
  EXPECT_EQ(frame_buf, untouched);

  /* Format mismatch between frame and logo. */
  logo.fmt = ST_FRAME_FMT_UYVY;
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 0), -EINVAL);
  EXPECT_EQ(frame_buf, untouched);

  /* Unsupported matching format. */
  frame.fmt = ST_FRAME_FMT_UYVY;
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 0), -EINVAL);
  EXPECT_EQ(frame_buf, untouched);
}

TEST(StFmtNameToCodec, MapsAllKnownCodecs) {
  EXPECT_EQ(st_name_to_codec("jpegxs"), ST22_CODEC_JPEGXS);
  EXPECT_EQ(st_name_to_codec("h264_cbr"), ST22_CODEC_H264_CBR);
  EXPECT_EQ(st_name_to_codec("h264"), ST22_CODEC_H264);
  EXPECT_EQ(st_name_to_codec("h265_cbr"), ST22_CODEC_H265_CBR);
  EXPECT_EQ(st_name_to_codec("h265"), ST22_CODEC_H265);
}

TEST(StFmtNameToCodec, RejectsUnknownEmptyAndWrongCase) {
  EXPECT_EQ(st_name_to_codec(""), ST22_CODEC_MAX);
  EXPECT_EQ(st_name_to_codec("unknown"), ST22_CODEC_MAX);
  EXPECT_EQ(st_name_to_codec("JPEGXS"), ST22_CODEC_MAX);
  EXPECT_EQ(st_name_to_codec("H264"), ST22_CODEC_MAX);
  EXPECT_EQ(st_name_to_codec("H265_CBR"), ST22_CODEC_MAX);
  EXPECT_EQ(st_name_to_codec("jpegxs_extra"), ST22_CODEC_MAX);
}

TEST(Main, st20_pgroup_and_fmt_name) {
  for (int i = 0; i < ST20_FMT_MAX; ++i) {
    const auto fmt = static_cast<enum st20_fmt>(i);
    struct st20_pgroup pg = {};
    ASSERT_EQ(st20_get_pgroup(fmt, &pg), 0) << "fmt=" << i;
    EXPECT_EQ(pg.fmt, fmt);
    EXPECT_GT(pg.size, 0u);
    EXPECT_GT(pg.coverage, 0u);
    ASSERT_NE(pg.name, nullptr);

    const char* name = st20_fmt_name(fmt);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, pg.name);
    EXPECT_STRNE(name, "unknown");
    EXPECT_EQ(st20_name_to_fmt(name), fmt);
  }

  /* Verify canonical ST 2110-20 pgroup byte sizes and pixel coverages. */
  struct CanonicalPgroup {
    enum st20_fmt fmt;
    uint32_t size;
    uint32_t coverage;
  };
  const CanonicalPgroup canonical[] = {
      {ST20_FMT_YUV_422_10BIT, 5u, 2u},
      {ST20_FMT_YUV_422_8BIT, 4u, 2u},
      {ST20_FMT_RGB_8BIT, 3u, 1u},
      {ST20_FMT_YUV_444_10BIT, 15u, 4u},
  };
  for (const auto& entry : canonical) {
    struct st20_pgroup pg = {};
    ASSERT_EQ(st20_get_pgroup(entry.fmt, &pg), 0);
    EXPECT_EQ(pg.size, entry.size);
    EXPECT_EQ(pg.coverage, entry.coverage);
  }

  struct st20_pgroup invalid_pg = {};
  EXPECT_LT(st20_get_pgroup(ST20_FMT_MAX, &invalid_pg), 0);
  EXPECT_STREQ(st20_fmt_name(ST20_FMT_MAX), "unknown");
  EXPECT_EQ(st20_name_to_fmt("unknown"), ST20_FMT_MAX);
  EXPECT_EQ(st20_name_to_fmt("not_a_real_fmt"), ST20_FMT_MAX);
}

TEST(Main, frame_sanity_check) {
  using ScopedMallocFrame = std::unique_ptr<struct st_frame, decltype(&st_frame_free)>;

  ScopedMallocFrame frame(
      st_frame_create_by_malloc(ST_FRAME_FMT_YUV422PLANAR10LE, 64, 32, false),
      &st_frame_free);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(st_frame_sanity_check(frame.get()), 0);

  struct st_frame bad = *frame;
  bad.fmt = ST_FRAME_FMT_MAX;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);

  bad = *frame;
  bad.addr[0] = nullptr;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);

  bad = *frame;
  bad.addr[1] = nullptr;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);

  bad = *frame;
  bad.linesize[0] = st_frame_least_linesize(frame->fmt, frame->width, 0) - 1;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);

  /* Exceeds buffer_size. */
  bad = *frame;
  bad.data_size = bad.buffer_size + 1;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);

  /* Smaller than st_frame_size (least_sz). */
  bad = *frame;
  bad.data_size = bad.buffer_size - 1;
  EXPECT_LT(st_frame_sanity_check(&bad), 0);
}

TEST(Main, frame_downsample) {
  using ScopedMallocFrame = std::unique_ptr<struct st_frame, decltype(&st_frame_free)>;

  /* Width 32 gives linesize 80 >= 5 + 64 so AVX-512 VBMI2 64-byte loads stay in-bounds.
   */
  constexpr uint32_t kSrcW = 32;
  constexpr uint32_t kSrcH = 8;
  constexpr uint32_t kDstW = 16;
  constexpr uint32_t kDstH = 4;
  constexpr size_t kRfcPgBytes = 5;
  constexpr uint32_t kSrcPgsPerRow = kSrcW / 2;
  constexpr uint32_t kDstPgsPerRow = kDstW / 2;

  ScopedMallocFrame src(
      st_frame_create_by_malloc(ST_FRAME_FMT_YUV422RFC4175PG2BE10, kSrcW, kSrcH, false),
      &st_frame_free);
  ScopedMallocFrame dst(
      st_frame_create_by_malloc(ST_FRAME_FMT_YUV422RFC4175PG2BE10, kDstW, kDstH, false),
      &st_frame_free);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  ASSERT_EQ(src->linesize[0], kSrcPgsPerRow * kRfcPgBytes);
  ASSERT_EQ(dst->linesize[0], kDstPgsPerRow * kRfcPgBytes);

  auto* src_bytes = static_cast<uint8_t*>(src->addr[0]);
  auto* dst_bytes = static_cast<uint8_t*>(dst->addr[0]);
  for (uint32_t row = 0; row < kSrcH; ++row) {
    for (uint32_t pg = 0; pg < kSrcPgsPerRow; ++pg) {
      uint8_t* p = src_bytes + row * src->linesize[0] + pg * kRfcPgBytes;
      const uint32_t id = row * kSrcPgsPerRow + pg;
      p[0] = static_cast<uint8_t>(row);
      p[1] = static_cast<uint8_t>(pg);
      p[2] = static_cast<uint8_t>((id ^ 0x5Au) & 0xFFu);
      p[3] = static_cast<uint8_t>((row * 31u + pg * 7u) & 0xFFu);
      p[4] = static_cast<uint8_t>((0xFFu - id) & 0xFFu);
    }
  }

  for (int idx = 0; idx < 4; ++idx) {
    std::memset(dst_bytes, 0, dst->buffer_size);
    ASSERT_EQ(st_frame_downsample(src.get(), dst.get(), idx), 0) << "idx=" << idx;
    for (uint32_t row = 0; row < kDstH; ++row) {
      for (uint32_t pg = 0; pg < kDstPgsPerRow; ++pg) {
        const uint32_t src_row = row * 2 + static_cast<uint32_t>(idx / 2);
        const uint32_t src_pg = pg * 2 + static_cast<uint32_t>(idx % 2);
        const uint8_t* expect_p =
            src_bytes + src_row * src->linesize[0] + src_pg * kRfcPgBytes;
        const uint8_t* actual_p = dst_bytes + row * dst->linesize[0] + pg * kRfcPgBytes;
        for (size_t b = 0; b < kRfcPgBytes; ++b) {
          EXPECT_EQ(actual_p[b], expect_p[b])
              << "idx=" << idx << " row=" << row << " pg=" << pg << " b=" << b;
        }
      }
    }
  }

  /* Scalar downsample path on ST_FRAME_FMT_RGB8 (16x8 -> 8x4). */
  constexpr uint32_t kRgbSrcW = 16;
  constexpr uint32_t kRgbSrcH = 8;
  constexpr uint32_t kRgbDstW = 8;
  constexpr uint32_t kRgbDstH = 4;
  constexpr size_t kRgbBytes = 3;

  ScopedMallocFrame rgb_src(
      st_frame_create_by_malloc(ST_FRAME_FMT_RGB8, kRgbSrcW, kRgbSrcH, false),
      &st_frame_free);
  ScopedMallocFrame rgb_dst(
      st_frame_create_by_malloc(ST_FRAME_FMT_RGB8, kRgbDstW, kRgbDstH, false),
      &st_frame_free);
  ASSERT_NE(rgb_src, nullptr);
  ASSERT_NE(rgb_dst, nullptr);

  auto* rgb_src_bytes = static_cast<uint8_t*>(rgb_src->addr[0]);
  auto* rgb_dst_bytes = static_cast<uint8_t*>(rgb_dst->addr[0]);
  for (uint32_t row = 0; row < kRgbSrcH; ++row) {
    for (uint32_t col = 0; col < kRgbSrcW; ++col) {
      uint8_t* p = rgb_src_bytes + row * rgb_src->linesize[0] + col * kRgbBytes;
      p[0] = static_cast<uint8_t>(row);
      p[1] = static_cast<uint8_t>(col);
      p[2] = static_cast<uint8_t>(((row * kRgbSrcW + col) ^ 0xA5u) & 0xFFu);
    }
  }

  for (int idx : {0, 3}) {
    std::memset(rgb_dst_bytes, 0, rgb_dst->buffer_size);
    ASSERT_EQ(st_frame_downsample(rgb_src.get(), rgb_dst.get(), idx), 0) << "idx=" << idx;
    for (uint32_t row = 0; row < kRgbDstH; ++row) {
      for (uint32_t col = 0; col < kRgbDstW; ++col) {
        const uint32_t src_row = row * 2 + static_cast<uint32_t>(idx / 2);
        const uint32_t src_col = col * 2 + static_cast<uint32_t>(idx % 2);
        const uint8_t* expect_p =
            rgb_src_bytes + src_row * rgb_src->linesize[0] + src_col * kRgbBytes;
        const uint8_t* actual_p =
            rgb_dst_bytes + row * rgb_dst->linesize[0] + col * kRgbBytes;
        for (size_t b = 0; b < kRgbBytes; ++b) {
          EXPECT_EQ(actual_p[b], expect_p[b])
              << "rgb idx=" << idx << " row=" << row << " col=" << col << " b=" << b;
        }
      }
    }
  }

  /* Invalid sample box indices. */
  EXPECT_LT(st_frame_downsample(src.get(), dst.get(), -1), 0);
  EXPECT_LT(st_frame_downsample(src.get(), dst.get(), 4), 0);

  /* Format mismatch with matching 2x dimensions. */
  struct st_frame fmt_mismatch = *dst;
  fmt_mismatch.fmt = ST_FRAME_FMT_RGB8;
  EXPECT_LT(st_frame_downsample(src.get(), &fmt_mismatch, 0), 0);

  /* Non-transport format. */
  ScopedMallocFrame planar_src(
      st_frame_create_by_malloc(ST_FRAME_FMT_YUV422PLANAR10LE, 16, 8, false),
      &st_frame_free);
  ScopedMallocFrame planar_dst(
      st_frame_create_by_malloc(ST_FRAME_FMT_YUV422PLANAR10LE, 8, 4, false),
      &st_frame_free);
  ASSERT_NE(planar_src, nullptr);
  ASSERT_NE(planar_dst, nullptr);
  EXPECT_LT(st_frame_downsample(planar_src.get(), planar_dst.get(), 0), 0);

  /* Non-2x dimensions. */
  struct st_frame bad_dim = *dst;
  bad_dim.width = 12;
  EXPECT_LT(st_frame_downsample(src.get(), &bad_dim, 0), 0);
  bad_dim = *dst;
  bad_dim.height = 6;
  EXPECT_LT(st_frame_downsample(src.get(), &bad_dim, 0), 0);
}

TEST(Main, rxp_txp_para_setters) {
  char port_p[] = "0000:af:01.0";
  char port_r[] = "0000:af:01.1";
  char ip_p[] = "239.168.85.20";
  char ip_r[] = "239.168.85.21";
  char ip_bad[] = "999.0.0.1";

  struct st_rx_port rxp = {};
  EXPECT_GT(st_rxp_para_port_set(&rxp, MTL_SESSION_PORT_P, port_p), 0);
  EXPECT_STREQ(rxp.port[MTL_SESSION_PORT_P], port_p);
  EXPECT_GT(st_rxp_para_port_set(&rxp, MTL_SESSION_PORT_R, port_r), 0);
  EXPECT_STREQ(rxp.port[MTL_SESSION_PORT_R], port_r);

  EXPECT_EQ(st_rxp_para_ip_set(&rxp, MTL_PORT_P, ip_p), 0);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_P][0], 239u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_P][1], 168u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_P][2], 85u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_P][3], 20u);

  EXPECT_EQ(st_rxp_para_ip_set(&rxp, MTL_PORT_R, ip_r), 0);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_R][0], 239u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_R][1], 168u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_R][2], 85u);
  EXPECT_EQ(rxp.ip_addr[MTL_SESSION_PORT_R][3], 21u);

  EXPECT_LT(st_rxp_para_ip_set(&rxp, MTL_PORT_P, ip_bad), 0);

  st_rxp_para_udp_port_set(&rxp, MTL_PORT_P, 20000);
  st_rxp_para_udp_port_set(&rxp, MTL_PORT_R, 20002);
  EXPECT_EQ(rxp.udp_port[MTL_SESSION_PORT_P], 20000u);
  EXPECT_EQ(rxp.udp_port[MTL_SESSION_PORT_R], 20002u);

  struct st_tx_port txp = {};
  EXPECT_GT(st_txp_para_port_set(&txp, MTL_SESSION_PORT_P, port_p), 0);
  EXPECT_STREQ(txp.port[MTL_SESSION_PORT_P], port_p);
  EXPECT_GT(st_txp_para_port_set(&txp, MTL_SESSION_PORT_R, port_r), 0);
  EXPECT_STREQ(txp.port[MTL_SESSION_PORT_R], port_r);

  EXPECT_EQ(st_txp_para_dip_set(&txp, MTL_PORT_P, ip_p), 0);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_P][0], 239u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_P][1], 168u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_P][2], 85u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_P][3], 20u);

  EXPECT_EQ(st_txp_para_dip_set(&txp, MTL_PORT_R, ip_r), 0);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_R][0], 239u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_R][1], 168u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_R][2], 85u);
  EXPECT_EQ(txp.dip_addr[MTL_SESSION_PORT_R][3], 21u);

  EXPECT_LT(st_txp_para_dip_set(&txp, MTL_PORT_P, ip_bad), 0);

  st_txp_para_udp_port_set(&txp, MTL_PORT_P, 30000);
  st_txp_para_udp_port_set(&txp, MTL_PORT_R, 30002);
  EXPECT_EQ(txp.udp_port[MTL_SESSION_PORT_P], 30000u);
  EXPECT_EQ(txp.udp_port[MTL_SESSION_PORT_R], 30002u);
}

TEST(Main, frame_accessors) {
  uint8_t plane0[16] = {};
  uint8_t plane1[16] = {};
  struct st20_rx_tp_meta tp_p = {};
  struct st20_rx_tp_meta tp_r = {};

  struct st_frame frame = {};
  frame.addr[0] = plane0;
  frame.addr[1] = plane1;
  frame.iova[0] = 0x100000ULL;
  frame.iova[1] = 0x200000ULL;
  frame.tp[MTL_SESSION_PORT_P] = &tp_p;
  frame.tp[MTL_SESSION_PORT_R] = &tp_r;

  EXPECT_EQ(st_frame_addr(&frame, 0), static_cast<void*>(plane0));
  EXPECT_EQ(st_frame_addr(&frame, 1), static_cast<void*>(plane1));
  EXPECT_EQ(st_frame_addr_cpuva(&frame, 0),
            static_cast<mtl_cpuva_t>(reinterpret_cast<uintptr_t>(plane0)));
  EXPECT_EQ(st_frame_addr_cpuva(&frame, 1),
            static_cast<mtl_cpuva_t>(reinterpret_cast<uintptr_t>(plane1)));
  EXPECT_EQ(st_frame_iova(&frame, 0), static_cast<mtl_iova_t>(0x100000ULL));
  EXPECT_EQ(st_frame_iova(&frame, 1), static_cast<mtl_iova_t>(0x200000ULL));
  EXPECT_EQ(st_frame_tp_meta(&frame, MTL_SESSION_PORT_P), &tp_p);
  EXPECT_EQ(st_frame_tp_meta(&frame, MTL_SESSION_PORT_R), &tp_r);
}
