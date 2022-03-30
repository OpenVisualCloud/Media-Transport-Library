/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "log.h"
#include "tests.h"

TEST(Cvt, simd_level) {
  enum st_simd_level cpu_level = st_get_simd_level();
  const char* name = st_get_simd_level_name(cpu_level);
  info("simd level by cpu: %d(%s)\n", cpu_level, name);
}

static void test_cvt_rfc4175_422be10_to_yuv422p10le(int w, int h,
                                                    enum st_simd_level cvt_level,
                                                    enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_2 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_2 != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg, p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422be10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  free(pg);
  free(pg_2);
  free(p10_u16);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_MAX,
                                          ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_scalar) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le(w, h, ST_SIMD_LEVEL_AVX512,
                                            ST_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le(w, h, ST_SIMD_LEVEL_AVX512_VBMI2,
                                            ST_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_yuv422p10le_to_rfc4175_422be10(int w, int h,
                                                    enum st_simd_level cvt_level,
                                                    enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);
  uint16_t* p10_u16_2 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16_2 != NULL);

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_yuv422p10le_to_rfc4175_422be10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg, p10_u16_2, (p10_u16_2 + w * h), (p10_u16_2 + w * h * 3 / 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  free(pg);
  free(p10_u16);
  free(p10_u16_2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, ST_SIMD_LEVEL_MAX,
                                          ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_scalar) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_AVX512,
                                          ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10(w, h, ST_SIMD_LEVEL_AVX512,
                                            ST_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512_vbmi) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                          ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10(w, h, ST_SIMD_LEVEL_AVX512_VBMI2,
                                            ST_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422le10_to_yuv422p10le(int w, int h,
                                                    enum st_simd_level cvt_level,
                                                    enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_2 =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_2 != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_yuv422p10le(pg, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  free(pg);
  free(pg_2);
  free(p10_u16);
}

TEST(Cvt, rfc4175_422le10_to_yuv422p10le) {
  test_cvt_rfc4175_422le10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_MAX,
                                          ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_yuv422p10le_scalar) {
  test_cvt_rfc4175_422le10_to_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_NONE);
}

static void test_cvt_yuv422p10le_to_rfc4175_422le10(int w, int h,
                                                    enum st_simd_level cvt_level,
                                                    enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);
  uint16_t* p10_u16_2 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16_2 != NULL);

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_yuv422p10le_to_rfc4175_422le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_yuv422p10le(pg, p10_u16_2, (p10_u16_2 + w * h),
                                            (p10_u16_2 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  free(pg);
  free(p10_u16);
  free(p10_u16_2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422le10) {
  test_cvt_yuv422p10le_to_rfc4175_422le10(1920, 1080, ST_SIMD_LEVEL_MAX,
                                          ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422le10_scalar) {
  test_cvt_yuv422p10le_to_rfc4175_422le10(1920, 1080, ST_SIMD_LEVEL_NONE,
                                          ST_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_422be10_to_422le10(int w, int h,
                                                enum st_simd_level cvt_level,
                                                enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be_2 != NULL);

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_422be10(pg_le, pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  free(pg_be);
  free(pg_le);
  free(pg_be_2);
}

TEST(Cvt, rfc4175_422be10_to_422le10) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_422le10_scalar) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                      ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_AVX512,
                                      ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10(w, h, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                      ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                      ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_NONE,
                                      ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                      ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10(w, h, ST_SIMD_LEVEL_AVX512_VBMI2,
                                        ST_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422le10_to_422be10(int w, int h,
                                                enum st_simd_level cvt_level,
                                                enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le_2 != NULL);

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_422be10(pg_le, pg_be, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_422le10(pg_be, pg_le_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  free(pg_be);
  free(pg_le);
  free(pg_le_2);
}

TEST(Cvt, rfc4175_422le10_to_422be10) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_422be10_scalar) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

static int test_cvt_extend_rfc4175_422le8_to_422be10(
    int w, int h, struct st20_rfc4175_422_8_pg2_le* pg_8,
    struct st20_rfc4175_422_10_pg2_be* pg_10) {
  uint32_t cnt = w * h / 2;

  for (uint32_t i = 0; i < cnt; i++) {
    pg_10[i].Cb00 = pg_8[i].Cb00;
    pg_10[i].Y00 = pg_8[i].Y00 >> 2;
    pg_10[i].Cb00_ = 0;
    pg_10[i].Y00_ = (pg_8[i].Y00 & 0x3) << 2;
    pg_10[i].Cr00 = pg_8[i].Cr00 >> 4;
    pg_10[i].Y01 = pg_8[i].Y01 >> 6;
    pg_10[i].Cr00_ = (pg_8[i].Cr00 & 0xF) << 2;
    pg_10[i].Y01_ = pg_8[i].Y01 << 2;
  }

  return 0;
}

static void test_cvt_rfc4175_422be10_to_422le8(int w, int h, enum st_simd_level cvt_level,
                                               enum st_simd_level back_level) {
  int ret;
  size_t fb_pg2_size_10 = w * h * 5 / 2;
  size_t fb_pg2_size_8 = w * h * 2;
  struct st20_rfc4175_422_10_pg2_be* pg_10 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size_10);
  ASSERT_TRUE(pg_10 != NULL);
  struct st20_rfc4175_422_8_pg2_le* pg_8 =
      (struct st20_rfc4175_422_8_pg2_le*)malloc(fb_pg2_size_8);
  ASSERT_TRUE(pg_8 != NULL);
  struct st20_rfc4175_422_8_pg2_le* pg_8_2 =
      (struct st20_rfc4175_422_8_pg2_le*)malloc(fb_pg2_size_8);
  ASSERT_TRUE(pg_8_2 != NULL);

  st_test_rand_data((uint8_t*)pg_8, fb_pg2_size_8, 0);
  test_cvt_extend_rfc4175_422le8_to_422be10(w, h, pg_8, pg_10);
  ret = st20_rfc4175_422be10_to_422le8_simd(pg_10, pg_8_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_8, pg_8_2, fb_pg2_size_8));

  free(pg_10);
  free(pg_8);
  free(pg_8_2);
}

TEST(Cvt, rfc4175_422be10_to_422le8) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_422le8_scalar) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                     ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_AVX512,
                                     ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8(w, h, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                     ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                     ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_NONE,
                                     ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                     ST_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8(w, h, ST_SIMD_LEVEL_AVX512_VBMI2,
                                       ST_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422le10_to_v210(int w, int h, enum st_simd_level cvt_level,
                                             enum st_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le_2 != NULL);
  uint8_t* pg_v210 = (uint8_t*)malloc(fb_pg2_size_v210);
  ASSERT_TRUE(pg_v210 != NULL);

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);
  ret = st20_rfc4175_422le10_to_v210_simd((uint8_t*)pg_le, pg_v210, w, h, cvt_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_v210_to_rfc4175_422le10(pg_v210, (uint8_t*)pg_le_2, w, h);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  if (fail_case)
    EXPECT_NE(0, memcmp(pg_le, pg_le_2, fb_pg2_size));
  else
    EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  free(pg_v210);
  free(pg_le);
  free(pg_le_2);
}

TEST(Cvt, rfc4175_422le10_to_v210) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_v210_scalar) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422le10_to_v210_avx512) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422le10_to_v210(722, 111, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1921, 1079, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
}

TEST(Cvt, rfc4175_422le10_to_v210_avx512_vbmi) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                   ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                   ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_NONE,
                                   ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512_VBMI2,
                                   ST_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422le10_to_v210(722, 111, ST_SIMD_LEVEL_AVX512_VBMI2,
                                   ST_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1921, 1079, ST_SIMD_LEVEL_AVX512_VBMI2,
                                   ST_SIMD_LEVEL_AVX512_VBMI2);
}

static void test_cvt_rfc4175_422be10_to_v210(int w, int h, enum st_simd_level cvt_level,
                                             enum st_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be_2 != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be_2 != NULL);
  uint8_t* pg_v210 = (uint8_t*)malloc(fb_pg2_size_v210);
  ASSERT_TRUE(pg_v210 != NULL);

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);
  ret = st20_rfc4175_422be10_to_v210_simd((uint8_t*)pg_be, pg_v210, w, h, cvt_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_v210_to_rfc4175_422le10(pg_v210, (uint8_t*)pg_le, w, h);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  st20_rfc4175_422le10_to_422be10(pg_le, pg_be_2, w, h);

  if (fail_case)
    EXPECT_NE(0, memcmp(pg_be, pg_be_2, fb_pg2_size));
  else
    EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  free(pg_v210);
  free(pg_be);
  free(pg_be_2);
  free(pg_le);
}

TEST(Cvt, rfc4175_422be10_to_v210_scalar) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_v210_avx512) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422be10_to_v210(722, 111, ST_SIMD_LEVEL_AVX512, ST_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1921, 1079, ST_SIMD_LEVEL_AVX512,
                                   ST_SIMD_LEVEL_AVX512);
}

static void test_rotate_rfc4175_422be10_422le10_yuv422p10le(
    int w, int h, enum st_simd_level cvt1_level, enum st_simd_level cvt2_level,
    enum st_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be_2 != NULL);

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_422le10(pg_be, pg_le, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_yuv422p10le(pg_le, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422be10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  free(pg_be);
  free(pg_le);
  free(pg_be_2);
}

TEST(Cvt, rotate_rfc4175_422be10_422le10_yuv422p10le) {
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_MAX,
                                                  ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rotate_rfc4175_422be10_422le10_yuv422p10le_scalar) {
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(1920, 1080, ST_SIMD_LEVEL_NONE,
                                                  ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_422be10_yuv422p10le_422le10(
    int w, int h, enum st_simd_level cvt1_level, enum st_simd_level cvt2_level,
    enum st_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be != NULL);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_le != NULL);
  size_t planar_size = w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)malloc(planar_size);
  ASSERT_TRUE(p10_u16 != NULL);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size);
  ASSERT_TRUE(pg_be_2 != NULL);

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg_be, p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg_le, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_422be10(pg_le, pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  free(pg_be);
  free(pg_le);
  free(pg_be_2);
}

TEST(Cvt, rotate_rfc4175_422be10_yuv422p10le_422le10) {
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(1920, 1080, ST_SIMD_LEVEL_MAX,
                                                  ST_SIMD_LEVEL_MAX, ST_SIMD_LEVEL_MAX);
}

TEST(Cvt, rotate_rfc4175_422be10_yuv422p10le_422le10_scalar) {
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(1920, 1080, ST_SIMD_LEVEL_NONE,
                                                  ST_SIMD_LEVEL_NONE, ST_SIMD_LEVEL_NONE);
}
