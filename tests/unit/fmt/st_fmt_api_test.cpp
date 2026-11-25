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

#include <cstring>
#include <tuple>

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
