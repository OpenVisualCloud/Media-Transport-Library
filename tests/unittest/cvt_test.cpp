/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "log.h"
#include "tests.hpp"

TEST(Cvt, simd_level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  const char* name = mtl_get_simd_level_name(cpu_level);
  info("simd level by cpu: %d(%s)\n", cpu_level, name);
}

static void test_cvt_rfc4175_422be10_to_yuv422p10le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg, p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_scalar) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le(w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                            MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422be10_to_yuv422p10le_dma(mtl_udma_handle dma, int w, int h,
                                                        enum mtl_simd_level cvt_level,
                                                        enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) mtl_hp_free(st, pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd_dma(
      dma, pg, mtl_hp_virt2iova(st, pg), p10_u16, (p10_u16 + w * h),
      (p10_u16 + w * h * 3 / 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  mtl_hp_free(st, pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                              MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                                MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(
      dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                              MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                              MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p10le_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                                MTL_SIMD_LEVEL_AVX512_VBMI2);
  }

  mtl_udma_free(dma);
}

static void test_cvt_yuv422p10le_to_rfc4175_422be10(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_yuv422p10le_to_rfc4175_422be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg, p10_u16_2, (p10_u16_2 + w * h), (p10_u16_2 + w * h * 3 / 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_scalar) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10(w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512_vbmi) {
  test_cvt_yuv422p10le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                            MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_yuv422p10le_to_rfc4175_422be10_dma(mtl_udma_handle dma, int w, int h,
                                                        enum mtl_simd_level cvt_level,
                                                        enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)mtl_hp_zmalloc(st, planar_size, MTL_PORT_P);
  mtl_iova_t p10_u16_iova = mtl_hp_virt2iova(st, p10_u16);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
      dma, p10_u16, p10_u16_iova, (p10_u16 + w * h),
      (p10_u16_iova + (mtl_iova_t)w * h * 2), (p10_u16 + w * h * 3 / 2),
      (p10_u16_iova + (mtl_iova_t)w * h * 3), pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_yuv422p10le_simd(
      pg, p10_u16_2, (p10_u16_2 + w * h), (p10_u16_2 + w * h * 3 / 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  st_test_free(pg);
  mtl_hp_free(st, p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                              MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                                MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(
      dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                              MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                              MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_yuv422p10le_to_rfc4175_422be10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                                MTL_SIMD_LEVEL_AVX512_VBMI2);
  }

  mtl_udma_free(dma);
}

static void test_cvt_rfc4175_422le10_to_yuv422p10le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_2 =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_yuv422p10le(pg, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_422le10_to_yuv422p10le) {
  test_cvt_rfc4175_422le10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_yuv422p10le_scalar) {
  test_cvt_rfc4175_422le10_to_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_yuv422p10le_to_rfc4175_422le10(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

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

  st_test_free(pg);
  st_test_free(p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422le10) {
  test_cvt_yuv422p10le_to_rfc4175_422le10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422le10_scalar) {
  test_cvt_yuv422p10le_to_rfc4175_422le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_422be10_to_422le10(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422be10_to_422le10) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_422le10_scalar) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx2) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, MTL_SIMD_LEVEL_AVX2,
                                      MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10(w, h, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX2);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10(w, h, MTL_SIMD_LEVEL_AVX512,
                                        MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_422le10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                        MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422be10_to_422le10_dma(mtl_udma_handle dma, int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) mtl_hp_free(st, pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_422le10_simd_dma(dma, pg_be, mtl_hp_virt2iova(st, pg_be),
                                                 pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  mtl_hp_free(st, pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422be10_to_422le10_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 1920 * 4, 1080 * 4, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le10_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le10_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                            MTL_SIMD_LEVEL_AVX512_VBMI2);
  }

  mtl_udma_free(dma);
}

static void test_cvt_rfc4175_422le10_to_422be10(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_le_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  // st_test_cmp((uint8_t*)pg_le, (uint8_t*)pg_le_2, fb_pg2_size);
  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_le_2);
}

static void test_cvt_rfc4175_422le10_to_422be10_2(int w, int h,
                                                  enum mtl_simd_level cvt_level,
                                                  enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  // st_test_cmp((uint8_t*)pg_be, (uint8_t*)pg_be_2, fb_pg2_size);
  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422le10_to_422be10) {
  test_cvt_rfc4175_422le10_to_422be10_2(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                        MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_422be10_scalar) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422le10_to_422be10_avx2) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX2,
                                      MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422le10_to_422be10(w, h, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX2);
  }
}

TEST(Cvt, rfc4175_422le10_to_422be10_avx512) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422le10_to_422be10(w, h, MTL_SIMD_LEVEL_AVX512,
                                        MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422le10_to_422be10_vbmi) {
  test_cvt_rfc4175_422le10_to_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                      MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422le10_to_422be10(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                        MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422le10_to_422be10_dma(mtl_udma_handle dma, int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_le_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) mtl_hp_free(st, pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le10_to_422be10_simd_dma(dma, pg_le, mtl_hp_virt2iova(st, pg_le),
                                                 pg_be, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  st_test_free(pg_be);
  mtl_hp_free(st, pg_le);
  st_test_free(pg_le_2);
}

TEST(Cvt, rfc4175_422le10_to_422be10_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 1920 * 4, 1080 * 4, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422le10_to_422be10_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422le10_to_422be10_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422le10_to_422be10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422le10_to_422be10_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422le10_to_422be10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                            MTL_SIMD_LEVEL_AVX512_VBMI2);
  }

  mtl_udma_free(dma);
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

static void test_cvt_rfc4175_422be10_to_422le8(int w, int h,
                                               enum mtl_simd_level cvt_level,
                                               enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size_10 = (size_t)w * h * 5 / 2;
  size_t fb_pg2_size_8 = (size_t)w * h * 2;
  struct st20_rfc4175_422_10_pg2_be* pg_10 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size_10);
  struct st20_rfc4175_422_8_pg2_le* pg_8 =
      (struct st20_rfc4175_422_8_pg2_le*)st_test_zmalloc(fb_pg2_size_8);
  struct st20_rfc4175_422_8_pg2_le* pg_8_2 =
      (struct st20_rfc4175_422_8_pg2_le*)st_test_zmalloc(fb_pg2_size_8);

  if (!pg_10 || !pg_8 || !pg_8_2) {
    EXPECT_EQ(0, 1);
    if (pg_10) st_test_free(pg_10);
    if (pg_8) st_test_free(pg_8);
    if (pg_8_2) st_test_free(pg_8_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_8, fb_pg2_size_8, 0);
  test_cvt_extend_rfc4175_422le8_to_422be10(w, h, pg_8, pg_10);
  ret = st20_rfc4175_422be10_to_422le8_simd(pg_10, pg_8_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_8, pg_8_2, fb_pg2_size_8));

  st_test_free(pg_10);
  st_test_free(pg_8);
  st_test_free(pg_8_2);
}

TEST(Cvt, rfc4175_422be10_to_422le8) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_422le8_scalar) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8(w, h, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_422le8(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422be10_to_422le8_dma(mtl_udma_handle dma, int w, int h,
                                                   enum mtl_simd_level cvt_level,
                                                   enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size_10 = (size_t)w * h * 5 / 2;
  size_t fb_pg2_size_8 = (size_t)w * h * 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg_10 =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size_10, MTL_PORT_P);
  struct st20_rfc4175_422_8_pg2_le* pg_8 =
      (struct st20_rfc4175_422_8_pg2_le*)st_test_zmalloc(fb_pg2_size_8);
  struct st20_rfc4175_422_8_pg2_le* pg_8_2 =
      (struct st20_rfc4175_422_8_pg2_le*)st_test_zmalloc(fb_pg2_size_8);

  if (!pg_10 || !pg_8 || !pg_8_2) {
    EXPECT_EQ(0, 1);
    if (pg_10) mtl_hp_free(st, pg_10);
    if (pg_8) st_test_free(pg_8);
    if (pg_8_2) st_test_free(pg_8_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_8, fb_pg2_size_8, 0);
  test_cvt_extend_rfc4175_422le8_to_422be10(w, h, pg_8, pg_10);
  ret = st20_rfc4175_422be10_to_422le8_simd_dma(dma, pg_10, mtl_hp_virt2iova(st, pg_10),
                                                pg_8_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_8, pg_8_2, fb_pg2_size_8));

  mtl_hp_free(st, pg_10);
  st_test_free(pg_8);
  st_test_free(pg_8_2);
}

TEST(Cvt, rfc4175_422be10_to_422le8_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                         MTL_SIMD_LEVEL_MAX);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 1920 * 4, 1080 * 4, MTL_SIMD_LEVEL_MAX,
                                         MTL_SIMD_LEVEL_MAX);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le8_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                         MTL_SIMD_LEVEL_NONE);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                         MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                         MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                         MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                         MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                           MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_422le8_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                         MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                         MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                         MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_422le8_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                         MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_422le8_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                           MTL_SIMD_LEVEL_AVX512_VBMI2);
  }

  mtl_udma_free(dma);
}

static int test_cvt_extend_yuv422p8_to_rfc4175_422be10(
    int w, int h, uint8_t* y, uint8_t* b, uint8_t* r,
    struct st20_rfc4175_422_10_pg2_be* pg_10) {
  uint32_t cnt = w * h / 2;

  for (uint32_t i = 0; i < cnt; i++) {
    uint8_t b0 = *b++;
    uint8_t r0 = *r++;
    uint8_t y0 = *y++;
    uint8_t y1 = *y++;

    pg_10[i].Cb00 = b0;
    pg_10[i].Y00 = y0 >> 2;
    pg_10[i].Cb00_ = 0;
    pg_10[i].Y00_ = (y0 & 0x3) << 2;
    pg_10[i].Cr00 = r0 >> 4;
    pg_10[i].Y01 = y1 >> 6;
    pg_10[i].Cr00_ = (r0 & 0xF) << 2;
    pg_10[i].Y01_ = y1 << 2;
  }

  return 0;
}

static void test_cvt_rfc4175_422be10_to_yuv422p8(int w, int h,
                                                 enum mtl_simd_level cvt_level) {
  int ret;
  size_t fb_pg2_size_10 = (size_t)w * h * 5 / 2;
  size_t fb_yuv422p8_size = (size_t)w * h * 2;
  struct st20_rfc4175_422_10_pg2_be* pg_10 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size_10);
  uint8_t* p8 = (uint8_t*)st_test_zmalloc(fb_yuv422p8_size);
  uint8_t* p8_2 = (uint8_t*)st_test_zmalloc(fb_yuv422p8_size);

  if (!pg_10 || !p8 || !p8_2) {
    EXPECT_EQ(0, 1);
    if (pg_10) st_test_free(pg_10);
    if (p8) st_test_free(p8);
    if (p8_2) st_test_free(p8_2);
    return;
  }

  st_test_rand_data(p8, fb_yuv422p8_size, 0);
  test_cvt_extend_yuv422p8_to_rfc4175_422be10(w, h, p8, p8 + w * h, p8 + w * h * 3 / 2,
                                              pg_10);
  ret = st20_rfc4175_422be10_to_yuv422p8_simd(pg_10, p8_2, p8_2 + w * h,
                                              p8_2 + w * h * 3 / 2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p8, p8_2, fb_yuv422p8_size));

  st_test_free(pg_10);
  st_test_free(p8);
  st_test_free(p8_2);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8) {
  test_cvt_rfc4175_422be10_to_yuv422p8(1920, 1080, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8_scalar) {
  test_cvt_rfc4175_422be10_to_yuv422p8(1920, 1080, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8_avx2) {
  test_cvt_rfc4175_422be10_to_yuv422p8(1920, 1080, MTL_SIMD_LEVEL_AVX2);
  test_cvt_rfc4175_422be10_to_yuv422p8(722, 111, MTL_SIMD_LEVEL_AVX2);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p8(w, h, MTL_SIMD_LEVEL_AVX2);
  }
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8_avx512) {
  test_cvt_rfc4175_422be10_to_yuv422p8(1920, 1080, MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_yuv422p8(722, 111, MTL_SIMD_LEVEL_AVX512);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p8(w, h, MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_yuv422p8(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_yuv422p8(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_yuv422p8(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422be10_to_yuv420p8(int w, int h) {
  int ret;
  size_t fb_pg2_size_10 = (size_t)w * h * 5 / 2;
  size_t fb_yuv420p8_size = (size_t)w * h * 3 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_10 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size_10);
  uint8_t* p8 = (uint8_t*)st_test_zmalloc(fb_yuv420p8_size);
  uint8_t* p8_2 = (uint8_t*)st_test_zmalloc(fb_yuv420p8_size);

  if (!pg_10 || !p8 || !p8_2) {
    EXPECT_EQ(0, 1);
    if (pg_10) st_test_free(pg_10);
    if (p8) st_test_free(p8);
    if (p8_2) st_test_free(p8_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_10, fb_pg2_size_10, 0);
  ret = st20_rfc4175_422be10_to_yuv420p8_simd(pg_10, p8, p8 + w * h, p8 + w * h * 5 / 4,
                                              w, h, MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(0, ret);
  ret = st20_rfc4175_422be10_to_yuv420p8_simd(
      pg_10, p8_2, p8_2 + w * h, p8_2 + w * h * 5 / 4, w, h, MTL_SIMD_LEVEL_AVX512);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p8, p8_2, fb_yuv420p8_size));

  st_test_free(pg_10);
  st_test_free(p8);
  st_test_free(p8_2);
}

TEST(Cvt, rfc4175_422be10_to_yuv420p8) {
  test_cvt_rfc4175_422be10_to_yuv420p8(1920, 1080);
}

static void test_cvt_rfc4175_422le10_to_v210(int w, int h, enum mtl_simd_level cvt_level,
                                             enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  uint8_t* pg_v210 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);

  if (!pg_le || !pg_le_2 || !pg_v210) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    if (pg_v210) st_test_free(pg_v210);
    return;
  }

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

  st_test_free(pg_v210);
  st_test_free(pg_le);
  st_test_free(pg_le_2);
}

TEST(Cvt, rfc4175_422le10_to_v210) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le10_to_v210_scalar) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422le10_to_v210_avx512) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422le10_to_v210(722, 111, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422le10_to_v210(1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, rfc4175_422le10_to_v210_avx512_vbmi) {
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422le10_to_v210(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422le10_to_v210(1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
}

static void test_cvt_rfc4175_422be10_to_v210(int w, int h, enum mtl_simd_level cvt_level,
                                             enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  uint8_t* pg_v210 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);

  if (!pg_le || !pg_be || !pg_v210 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (pg_v210) st_test_free(pg_v210);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);
  ret = st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210, w, h, cvt_level);
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

  st_test_free(pg_v210);
  st_test_free(pg_be);
  st_test_free(pg_be_2);
  st_test_free(pg_le);
}

TEST(Cvt, rfc4175_422be10_to_v210) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_v210_scalar) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_v210_avx512) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422be10_to_v210(722, 111, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210(1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, rfc4175_422be10_to_v210_avx512_vbmi) {
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422be10_to_v210(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210(1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
}

static void test_cvt_rfc4175_422be10_to_v210_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  uint8_t* pg_v210 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);

  if (!pg_le || !pg_be || !pg_v210 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) mtl_hp_free(st, pg_be);
    if (pg_be_2) st_test_free(pg_be_2);
    if (pg_v210) st_test_free(pg_v210);
    if (pg_le) st_test_free(pg_le);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);
  ret = st20_rfc4175_422be10_to_v210_simd_dma(dma, pg_be, mtl_hp_virt2iova(st, pg_be),
                                              pg_v210, w, h, cvt_level);
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

  mtl_hp_free(st, pg_be);
  st_test_free(pg_be_2);
  st_test_free(pg_le);
  st_test_free(pg_v210);
}

TEST(Cvt, rfc4175_422be10_to_v210_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_v210_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_v210_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_v210_avx512_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_NONE);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be10_to_v210_dma(dma, 1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);

  mtl_udma_free(dma);
}

static void test_cvt_v210_to_rfc4175_422be10(int w, int h, enum mtl_simd_level cvt_level,
                                             enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  uint8_t* pg_v210 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);

  if (!pg_be || !pg_v210 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_v210) st_test_free(pg_v210);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);
  ret = st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210, w, h, cvt_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_v210_to_rfc4175_422be10_simd(pg_v210, pg_be_2, w, h, back_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  // st_test_cmp((uint8_t*)pg_be, (uint8_t*)pg_be_2, fb_pg2_size);
  if (fail_case)
    EXPECT_NE(0, memcmp(pg_be, pg_be_2, fb_pg2_size));
  else
    EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_v210);
  st_test_free(pg_be);
  st_test_free(pg_be_2);
}

TEST(Cvt, v210_to_rfc4175_422be10) {
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, v210_to_rfc4175_422be10_scalar) {
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, v210_to_rfc4175_422be10_avx512) {
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10(1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, v210_to_rfc4175_422be10_vbmi) {
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10(1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                   MTL_SIMD_LEVEL_AVX512_VBMI2);
}

static void test_cvt_v210_to_rfc4175_422be10_2(int w, int h,
                                               enum mtl_simd_level cvt_level,
                                               enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  uint8_t* pg_v210 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);
  uint8_t* pg_v210_2 = (uint8_t*)st_test_zmalloc(fb_pg2_size_v210);

  if (!pg_be || !pg_v210 || !pg_v210_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_v210) st_test_free(pg_v210);
    if (pg_v210_2) st_test_free(pg_v210_2);
    return;
  }

  st_test_rand_v210(pg_v210, fb_pg2_size_v210, 0);
  ret = st20_v210_to_rfc4175_422be10_simd(pg_v210, pg_be, w, h, cvt_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210_2, w, h, back_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  // st_test_cmp(pg_v210, pg_v210_2, fb_pg2_size_v210);
  if (fail_case)
    EXPECT_NE(0, memcmp(pg_v210, pg_v210_2, fb_pg2_size_v210));
  else
    EXPECT_EQ(0, memcmp(pg_v210, pg_v210_2, fb_pg2_size_v210));

  st_test_free(pg_v210);
  st_test_free(pg_be);
  st_test_free(pg_v210_2);
}

TEST(Cvt, v210_to_rfc4175_422be10_2) {
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, v210_to_rfc4175_422be10_2_scalar) {
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, v210_to_rfc4175_422be10_2_avx512) {
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10_2(722, 111, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_2(1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                     MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, v210_to_rfc4175_422be10_2_vbmi) {
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_2(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10_2(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_2(1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                     MTL_SIMD_LEVEL_AVX512_VBMI2);
}

static void test_cvt_v210_to_rfc4175_422be10_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  bool fail_case = (w * h % 6); /* do not convert when pg_num is not multiple of 3 */
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  uint8_t* pg_v210 = (uint8_t*)mtl_hp_zmalloc(st, fb_pg2_size_v210, MTL_PORT_P);

  if (!pg_be || !pg_v210 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_v210) mtl_hp_free(st, pg_v210);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);
  ret = st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210, w, h, cvt_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_v210_to_rfc4175_422be10_simd_dma(dma, pg_v210, mtl_hp_virt2iova(st, pg_v210),
                                              pg_be_2, w, h, back_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  st_test_cmp((uint8_t*)pg_be, (uint8_t*)pg_be_2, fb_pg2_size);
  if (fail_case)
    EXPECT_NE(0, memcmp(pg_be, pg_be_2, fb_pg2_size));
  else
    EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  mtl_hp_free(st, pg_v210);
  st_test_free(pg_be);
  st_test_free(pg_be_2);
}

TEST(Cvt, v210_to_rfc4175_422be10_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, v210_to_rfc4175_422be10_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, v210_to_rfc4175_422be10_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1921, 1079, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);

  mtl_udma_free(dma);
}

TEST(Cvt, v210_to_rfc4175_422be10_vbmi_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_NONE);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_v210_to_rfc4175_422be10_dma(dma, 1921, 1079, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                       MTL_SIMD_LEVEL_AVX512_VBMI2);

  mtl_udma_free(dma);
}

static void test_cvt_rfc4175_422be10_to_y210(int w, int h, enum mtl_simd_level cvt_level,
                                             enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_be* pg_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t fb_pg_y210_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* pg_y210 = (uint16_t*)st_test_zmalloc(fb_pg_y210_size);

  if (!pg || !pg_2 || !pg_y210) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (pg_y210) st_test_free(pg_y210);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_y210_simd(pg, pg_y210, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_y210_to_rfc4175_422be10_simd(pg_y210, pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(pg_y210);
}

TEST(Cvt, rfc4175_422be10_to_y210) {
  test_cvt_rfc4175_422be10_to_y210(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be10_to_y210_scalar) {
  test_cvt_rfc4175_422be10_to_y210(1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be10_to_y210_avx512) {
  test_cvt_rfc4175_422be10_to_y210(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210(722, 111, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210(722, 111, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210(722, 111, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_y210(w, h, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512);
  }
}

static void test_cvt_rfc4175_422be10_to_y210_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  size_t fb_pg2_size_y210 = (size_t)w * h * 4;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  uint16_t* pg_y210 = (uint16_t*)st_test_zmalloc(fb_pg2_size_y210);

  if (!pg_be || !pg_y210 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) mtl_hp_free(st, pg_be);
    if (pg_be_2) st_test_free(pg_be_2);
    if (pg_y210) st_test_free(pg_y210);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_y210_simd_dma(dma, pg_be, mtl_hp_virt2iova(st, pg_be),
                                              pg_y210, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_y210_to_rfc4175_422be10(pg_y210, pg_be_2, w, h);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  mtl_hp_free(st, pg_be);
  st_test_free(pg_be_2);
  st_test_free(pg_y210);
}

TEST(Cvt, rfc4175_422be10_to_y210_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_y210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_y210_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_y210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be10_to_y210_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be10_to_y210_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be10_to_y210_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be10_to_y210_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                         MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

static void test_cvt_y210_to_rfc4175_422be10(int w, int h, enum mtl_simd_level cvt_level,
                                             enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t fb_pg_y210_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* pg_y210 = (uint16_t*)st_test_zmalloc(fb_pg_y210_size);
  uint16_t* pg_y210_2 = (uint16_t*)st_test_zmalloc(fb_pg_y210_size);

  if (!pg || !pg_y210_2 || !pg_y210) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_y210_2) st_test_free(pg_y210_2);
    if (pg_y210) st_test_free(pg_y210);
    return;
  }

  for (size_t i = 0; i < (fb_pg_y210_size / 2); i++) {
    pg_y210[i] = rand() & 0xFFC0; /* only 10 bit */
  }

  ret = st20_y210_to_rfc4175_422be10_simd(pg_y210, pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_y210_simd(pg, pg_y210_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_y210, pg_y210_2, fb_pg_y210_size));

  st_test_free(pg);
  st_test_free(pg_y210);
  st_test_free(pg_y210_2);
}

TEST(Cvt, y210_to_rfc4175_422be10) {
  test_cvt_y210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, y210_to_rfc4175_422be10_scalar) {
  test_cvt_y210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, y210_to_rfc4175_422be10_avx512) {
  test_cvt_y210_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                   MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_y210_to_rfc4175_422be10(w, h, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512);
  }
}

static void test_cvt_y210_to_rfc4175_422be10_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 5 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t fb_pg_y210_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* pg_y210 = (uint16_t*)mtl_hp_zmalloc(st, fb_pg_y210_size, MTL_PORT_P);
  mtl_iova_t pg_y210_iova = mtl_hp_virt2iova(st, pg_y210);
  uint16_t* pg_y210_2 = (uint16_t*)st_test_zmalloc(fb_pg_y210_size);

  if (!pg || !pg_y210_2 || !pg_y210) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_y210_2) st_test_free(pg_y210_2);
    if (pg_y210) st_test_free(pg_y210);
    return;
  }

  for (size_t i = 0; i < (fb_pg_y210_size / 2); i++) {
    pg_y210[i] = rand() & 0xFFC0; /* only 10 bit */
  }

  ret = st20_y210_to_rfc4175_422be10_simd_dma(dma, pg_y210, pg_y210_iova, pg, w, h,
                                              cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_y210_simd(pg, pg_y210_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_y210, pg_y210_2, fb_pg_y210_size));

  st_test_free(pg);
  mtl_hp_free(st, pg_y210);
  st_test_free(pg_y210_2);
}

TEST(Cvt, y210_to_rfc4175_422be10_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_y210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, y210_to_rfc4175_422be10_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_y210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, y210_to_rfc4175_422be10_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_y210_to_rfc4175_422be10_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_AVX512);
  test_cvt_y210_to_rfc4175_422be10_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                       MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_y210_to_rfc4175_422be10_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                         MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

static void test_rotate_rfc4175_422be10_422le10_yuv422p10le(
    int w, int h, enum mtl_simd_level cvt1_level, enum mtl_simd_level cvt2_level,
    enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p10_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p10_u16) st_test_free(p10_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le10_to_yuv422p10le(pg_le, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p10le_to_rfc4175_422be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 3 / 2), pg_be_2, w, h, cvt3_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rotate_rfc4175_422be10_422le10_yuv422p10le_avx512) {
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512);
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512);
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rotate_rfc4175_422be10_422le10_yuv422p10le_vbmi) {
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                                  MTL_SIMD_LEVEL_AVX512_VBMI2,
                                                  MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rotate_rfc4175_422be10_422le10_yuv422p10le_scalar) {
  test_rotate_rfc4175_422be10_422le10_yuv422p10le(
      1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_422be10_yuv422p10le_422le10(
    int w, int h, enum mtl_simd_level cvt1_level, enum mtl_simd_level cvt2_level,
    enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_422_10_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p10_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p10_u16) st_test_free(p10_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

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

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rotate_rfc4175_422be10_yuv422p10le_422le10_avx512) {
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512);
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_NONE);
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, rotate_rfc4175_422be10_yuv422p10le_422le10_vbmi) {
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                                  MTL_SIMD_LEVEL_AVX512_VBMI2,
                                                  MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_NONE);
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512_VBMI2);
}

TEST(Cvt, rotate_rfc4175_422be10_yuv422p10le_422le10_scalar) {
  test_rotate_rfc4175_422be10_yuv422p10le_422le10(
      1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_422be12_to_yuv422p12le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_be* pg =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_yuv422p12le_simd(
      pg, p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p12le_to_rfc4175_422be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le) {
  test_cvt_rfc4175_422be12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_scalar) {
  test_cvt_rfc4175_422be12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_avx512) {
  test_cvt_rfc4175_422be12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be12_to_yuv422p12le(w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_avx512_vbmi) {
  test_cvt_rfc4175_422be12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512_VBMI2);
  test_cvt_rfc4175_422be12_to_yuv422p12le(722, 111, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be12_to_yuv422p12le(w, h, MTL_SIMD_LEVEL_AVX512_VBMI2,
                                            MTL_SIMD_LEVEL_AVX512_VBMI2);
  }
}

static void test_cvt_rfc4175_422be12_to_yuv422p12le_dma(mtl_udma_handle dma, int w, int h,
                                                        enum mtl_simd_level cvt_level,
                                                        enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_12_pg2_be* pg =
      (struct st20_rfc4175_422_12_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_12_pg2_be* pg_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) mtl_hp_free(st, pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_yuv422p12le_simd_dma(
      dma, pg, mtl_hp_virt2iova(st, pg), p12_u16, (p12_u16 + w * h),
      (p12_u16 + w * h * 3 / 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p12le_to_rfc4175_422be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  mtl_hp_free(st, pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                              MTL_SIMD_LEVEL_MAX);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_NONE);

  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be12_to_yuv422p12le_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                              MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                              MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be12_to_yuv422p12le_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                                MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

static void test_cvt_yuv422p12le_to_rfc4175_422be12(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_be* pg =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p12_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p12_u16_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p12_u16_2) st_test_free(p12_u16_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p12_u16[i] = rand() & 0xfff; /* only 12 bit */
  }

  ret = st20_yuv422p12le_to_rfc4175_422be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be12_to_yuv422p12le_simd(
      pg, p12_u16_2, (p12_u16_2 + w * h), (p12_u16_2 + w * h * 3 / 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p12_u16, p12_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p12_u16);
  st_test_free(p12_u16_2);
}

TEST(Cvt, yuv422p12le_to_rfc4175_422be12) {
  test_cvt_yuv422p12le_to_rfc4175_422be12(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p12le_to_rfc4175_422be12_scalar) {
  test_cvt_yuv422p12le_to_rfc4175_422be12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_422le12_to_yuv422p12le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_le* pg =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_le* pg_2 =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_422le12_to_yuv422p12le(pg, p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p12le_to_rfc4175_422le12(p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 3 / 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_422le12_to_yuv422p12le) {
  test_cvt_rfc4175_422le12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le12_to_yuv422p12le_scalar) {
  test_cvt_rfc4175_422le12_to_yuv422p12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_yuv422p12le_to_rfc4175_422le12(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_le* pg =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p12_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p12_u16_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p12_u16_2) st_test_free(p12_u16_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p12_u16[i] = rand() & 0xfff; /* only 12 bit */
  }

  ret = st20_yuv422p12le_to_rfc4175_422le12(p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 3 / 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_yuv422p12le(pg, p12_u16_2, (p12_u16_2 + w * h),
                                            (p12_u16_2 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p12_u16, p12_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p12_u16);
  st_test_free(p12_u16_2);
}

TEST(Cvt, yuv422p12le_to_rfc4175_422le12) {
  test_cvt_yuv422p12le_to_rfc4175_422le12(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv422p12le_to_rfc4175_422le12_scalar) {
  test_cvt_yuv422p12le_to_rfc4175_422le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_422be12_to_422le12(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_422le12_simd(pg_be, pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422be12_to_422le12) {
  test_cvt_rfc4175_422be12_to_422le12(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422be12_to_422le12_scalar) {
  test_cvt_rfc4175_422be12_to_422le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_422be12_to_422le12_avx512) {
  test_cvt_rfc4175_422be12_to_422le12(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12(722, 111, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12(722, 111, MTL_SIMD_LEVEL_AVX512,
                                      MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be12_to_422le12(w, h, MTL_SIMD_LEVEL_AVX512,
                                        MTL_SIMD_LEVEL_AVX512);
  }
}

static void test_cvt_rfc4175_422be12_to_422le12_dma(mtl_udma_handle dma, int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 6 / 2;
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)mtl_hp_zmalloc(st, fb_pg2_size, MTL_PORT_P);
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) mtl_hp_free(st, pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_422le12_simd_dma(dma, pg_be, mtl_hp_virt2iova(st, pg_be),
                                                 pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  mtl_hp_free(st, pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422be12_to_422le12_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 1920 * 4, 1080 * 4, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be12_to_422le12_scalar_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
  mtl_udma_free(dma);
}

TEST(Cvt, rfc4175_422be12_to_422le12_avx512_dma) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  mtl_udma_handle dma = mtl_udma_create(handle, 128, MTL_PORT_P);
  if (!dma) return;

  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 722, 111, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_rfc4175_422be12_to_422le12_dma(dma, 722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_NONE);
  int w = 2; /* each pg has two pixels */
  for (int h = 640; h < (640 + 64); h++) {
    test_cvt_rfc4175_422be12_to_422le12_dma(dma, w, h, MTL_SIMD_LEVEL_AVX512,
                                            MTL_SIMD_LEVEL_AVX512);
  }

  mtl_udma_free(dma);
}

static void test_cvt_rfc4175_422le12_to_422be12(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_le* pg_le_2 =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_le_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be12_to_422le12_simd(pg_be, pg_le_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  // st_test_cmp((uint8_t*)pg_le, (uint8_t*)pg_le_2, fb_pg2_size);
  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_le_2);
}

static void test_cvt_rfc4175_422le12_to_422be12_2(int w, int h,
                                                  enum mtl_simd_level cvt_level,
                                                  enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  // st_test_cmp((uint8_t*)pg_be, (uint8_t*)pg_be_2, fb_pg2_size);
  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_422le12_to_422be12) {
  test_cvt_rfc4175_422le12_to_422be12_2(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                        MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_422le12_to_422be12_scalar) {
  test_cvt_rfc4175_422le12_to_422be12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_422be12_422le12_yuv422p12le(
    int w, int h, enum mtl_simd_level cvt1_level, enum mtl_simd_level cvt2_level,
    enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p12_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p12_u16) st_test_free(p12_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_422le12_simd(pg_be, pg_le, w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_yuv422p12le(pg_le, p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 3 / 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p12le_to_rfc4175_422be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), pg_be_2, w, h, cvt3_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rotate_rfc4175_422be12_422le12_yuv422p12le_scalar) {
  test_rotate_rfc4175_422be12_422le12_yuv422p12le(
      1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_422be12_yuv422p12le_422le12(
    int w, int h, enum mtl_simd_level cvt1_level, enum mtl_simd_level cvt2_level,
    enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 6 / 2;
  struct st20_rfc4175_422_12_pg2_be* pg_be =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_422_12_pg2_le* pg_le =
      (struct st20_rfc4175_422_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_422_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_422_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p12_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p12_u16) st_test_free(p12_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_422be12_to_yuv422p12le_simd(
      pg_be, p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 3 / 2), w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_yuv422p12le_to_rfc4175_422le12(p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 3 / 2), pg_le, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422le12_to_422be12(pg_le, pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rotate_rfc4175_422be12_yuv422p12le_422le12_scalar) {
  test_rotate_rfc4175_422be12_yuv422p12le_422le12(
      1920, 1080, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444be10_to_444p10le(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_be* pg =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_be* pg_2 =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg4_size, 0);

  ret = st20_rfc4175_444be10_to_444p10le_simd(pg, p10_u16, (p10_u16 + w * h),
                                              (p10_u16 + w * h * 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_444p10le_to_rfc4175_444be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg4_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_444be10_to_444p10le) {
  test_cvt_rfc4175_444be10_to_444p10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444be10_to_444p10le_scalar) {
  test_cvt_rfc4175_444be10_to_444p10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_444p10le_to_rfc4175_444be10(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_be* pg =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_444p10le_to_rfc4175_444be10_simd(p10_u16, (p10_u16 + w * h),
                                              (p10_u16 + w * h * 2), pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444be10_to_444p10le_simd(pg, p10_u16_2, (p10_u16_2 + w * h),
                                              (p10_u16_2 + w * h * 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, 444p10le_to_rfc4175_444be10) {
  test_cvt_444p10le_to_rfc4175_444be10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, 444p10le_to_rfc4175_444be10_scalar) {
  test_cvt_444p10le_to_rfc4175_444be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le10_to_yuv444p10le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_2 =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg4_size, 0);

  ret = st20_rfc4175_444le10_to_yuv444p10le(pg, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv444p10le_to_rfc4175_444le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg4_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_444le10_to_yuv444p10le) {
  test_cvt_rfc4175_444le10_to_yuv444p10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le10_to_yuv444p10le_scalar) {
  test_cvt_rfc4175_444le10_to_yuv444p10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le10_to_gbrp10le(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_2 =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg4_size, 0);

  ret = st20_rfc4175_444le10_to_gbrp10le(pg, p10_u16, (p10_u16 + w * h),
                                         (p10_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_gbrp10le_to_rfc4175_444le10(p10_u16, (p10_u16 + w * h),
                                         (p10_u16 + w * h * 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg4_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rfc4175_444le10_to_gbrp10le) {
  test_cvt_rfc4175_444le10_to_gbrp10le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le10_to_gbrp10le_scalar) {
  test_cvt_rfc4175_444le10_to_gbrp10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_yuv444p10le_to_rfc4175_444le10(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_yuv444p10le_to_rfc4175_444le10(p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_yuv444p10le(pg, p10_u16_2, (p10_u16_2 + w * h),
                                            (p10_u16_2 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, yuv444p10le_to_rfc4175_444le10) {
  test_cvt_yuv444p10le_to_rfc4175_444le10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv444p10le_to_rfc4175_444le10_scalar) {
  test_cvt_yuv444p10le_to_rfc4175_444le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_gbrp10le_to_rfc4175_444le10(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_2 || !p10_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_2) st_test_free(p10_u16_2);
    if (p10_u16) st_test_free(p10_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
  }

  ret = st20_gbrp10le_to_rfc4175_444le10(p10_u16, (p10_u16 + w * h),
                                         (p10_u16 + w * h * 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_gbrp10le(pg, p10_u16_2, (p10_u16_2 + w * h),
                                         (p10_u16_2 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p10_u16, p10_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p10_u16);
  st_test_free(p10_u16_2);
}

TEST(Cvt, gbrp10le_to_rfc4175_444le10) {
  test_cvt_gbrp10le_to_rfc4175_444le10(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, gbrp10le_to_rfc4175_444le10_scalar) {
  test_cvt_gbrp10le_to_rfc4175_444le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444be10_to_444le10(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_be* pg_be =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_le =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be_2 =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg4_size, 0);

  ret = st20_rfc4175_444be10_to_444le10_simd(pg_be, pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_444be10_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg4_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_444be10_to_444le10) {
  test_cvt_rfc4175_444be10_to_444le10(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444be10_to_444le10_scalar) {
  test_cvt_rfc4175_444be10_to_444le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le10_to_444be10(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg_le =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_le_2 =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);

  if (!pg_be || !pg_le || !pg_le_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg4_size, 0);

  ret = st20_rfc4175_444le10_to_444be10_simd(pg_le, pg_be, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444be10_to_444le10_simd(pg_be, pg_le_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg4_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_le_2);
}

static void test_cvt_rfc4175_444le10_to_444be10_2(int w, int h,
                                                  enum mtl_simd_level cvt_level,
                                                  enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg4_size = w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_le* pg_le =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be_2 =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg4_size, 0);

  ret = st20_rfc4175_444le10_to_444be10_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_444be10_simd(pg_le, pg_be_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg4_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_444le10_to_444be10) {
  test_cvt_rfc4175_444le10_to_444be10_2(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                        MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le10_to_444be10_scalar) {
  test_cvt_rfc4175_444le10_to_444be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_444be10_444le10_444p10le(int w, int h,
                                                         enum mtl_simd_level cvt1_level,
                                                         enum mtl_simd_level cvt2_level,
                                                         enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_be* pg_be =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_le =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be_2 =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);

  if (!pg_le || !pg_be || !p10_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p10_u16) st_test_free(p10_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg4_size, 0);

  ret = st20_rfc4175_444be10_to_444le10_simd(pg_be, pg_le, w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_yuv444p10le(pg_le, p10_u16, (p10_u16 + w * h),
                                            (p10_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_444p10le_to_rfc4175_444be10_simd(
      p10_u16, (p10_u16 + w * h), (p10_u16 + w * h * 2), pg_be_2, w, h, cvt3_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg4_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rotate_rfc4175_444be10_444le10_444p10le_scalar) {
  test_rotate_rfc4175_444be10_444le10_444p10le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                               MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_444be10_444p10le_444le10(int w, int h,
                                                         enum mtl_simd_level cvt1_level,
                                                         enum mtl_simd_level cvt2_level,
                                                         enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg4_size = (size_t)w * h * 15 / 4;
  struct st20_rfc4175_444_10_pg4_be* pg_be =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);
  struct st20_rfc4175_444_10_pg4_le* pg_le =
      (struct st20_rfc4175_444_10_pg4_le*)st_test_zmalloc(fb_pg4_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p10_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_444_10_pg4_be* pg_be_2 =
      (struct st20_rfc4175_444_10_pg4_be*)st_test_zmalloc(fb_pg4_size);

  if (!pg_le || !pg_be || !p10_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p10_u16) st_test_free(p10_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg4_size, 0);

  ret = st20_rfc4175_444be10_to_444p10le_simd(pg_be, p10_u16, (p10_u16 + w * h),
                                              (p10_u16 + w * h * 2), w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_444p10le_to_rfc4175_444le10(p10_u16, (p10_u16 + w * h),
                                         (p10_u16 + w * h * 2), pg_le, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le10_to_444be10(pg_le, pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg4_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p10_u16);
}

TEST(Cvt, rotate_rfc4175_444be10_444p10le_444le10_scalar) {
  test_rotate_rfc4175_444be10_444p10le_444le10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                               MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444be12_to_444p12le(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_be* pg =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_be* pg_2 =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_444be12_to_444p12le_simd(pg, p12_u16, (p12_u16 + w * h),
                                              (p12_u16 + w * h * 2), w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_444p12le_to_rfc4175_444be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 2), pg_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_444be12_to_444p12le) {
  test_cvt_rfc4175_444be12_to_444p12le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444be12_to_444p12le_scalar) {
  test_cvt_rfc4175_444be12_to_444p12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_444p12le_to_rfc4175_444be12(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_be* pg =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p12_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p12_u16_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p12_u16_2) st_test_free(p12_u16_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p12_u16[i] = rand() & 0xfff; /* only 12 bit */
  }

  ret = st20_444p12le_to_rfc4175_444be12_simd(p12_u16, (p12_u16 + w * h),
                                              (p12_u16 + w * h * 2), pg, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444be12_to_444p12le_simd(pg, p12_u16_2, (p12_u16_2 + w * h),
                                              (p12_u16_2 + w * h * 2), w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p12_u16, p12_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p12_u16);
  st_test_free(p12_u16_2);
}

TEST(Cvt, 444p12le_to_rfc4175_444be12) {
  test_cvt_444p12le_to_rfc4175_444be12(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, 444p12le_to_rfc4175_444be12_scalar) {
  test_cvt_444p12le_to_rfc4175_444be12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le12_to_yuv444p12le(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_2 =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_444le12_to_yuv444p12le(pg, p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_yuv444p12le_to_rfc4175_444le12(p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_444le12_to_yuv444p12le) {
  test_cvt_rfc4175_444le12_to_yuv444p12le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le12_to_yuv444p12le_scalar) {
  test_cvt_rfc4175_444le12_to_yuv444p12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le12_to_gbrp12le(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_2 =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !pg_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (pg_2) st_test_free(pg_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  st_test_rand_data((uint8_t*)pg, fb_pg2_size, 0);

  ret = st20_rfc4175_444le12_to_gbrp12le(pg, p12_u16, (p12_u16 + w * h),
                                         (p12_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_gbrp12le_to_rfc4175_444le12(p12_u16, (p12_u16 + w * h),
                                         (p12_u16 + w * h * 2), pg_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg, pg_2, fb_pg2_size));

  st_test_free(pg);
  st_test_free(pg_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rfc4175_444le12_to_gbrp12le) {
  test_cvt_rfc4175_444le12_to_gbrp12le(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le12_to_gbrp12le_scalar) {
  test_cvt_rfc4175_444le12_to_gbrp12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_yuv444p12le_to_rfc4175_444le12(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p12_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p12_u16_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p12_u16_2) st_test_free(p12_u16_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p12_u16[i] = rand() & 0xfff; /* only 12 bit */
  }

  ret = st20_yuv444p12le_to_rfc4175_444le12(p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_yuv444p12le(pg, p12_u16_2, (p12_u16_2 + w * h),
                                            (p12_u16_2 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p12_u16, p12_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p12_u16);
  st_test_free(p12_u16_2);
}

TEST(Cvt, yuv444p12le_to_rfc4175_444le12) {
  test_cvt_yuv444p12le_to_rfc4175_444le12(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                          MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, yuv444p12le_to_rfc4175_444le12_scalar) {
  test_cvt_yuv444p12le_to_rfc4175_444le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_gbrp12le_to_rfc4175_444le12(int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p12_u16_2 = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p12_u16_2 || !p12_u16) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p12_u16_2) st_test_free(p12_u16_2);
    if (p12_u16) st_test_free(p12_u16);
    return;
  }

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p12_u16[i] = rand() & 0xfff; /* only 12 bit */
  }

  ret = st20_gbrp12le_to_rfc4175_444le12(p12_u16, (p12_u16 + w * h),
                                         (p12_u16 + w * h * 2), pg, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_gbrp12le(pg, p12_u16_2, (p12_u16_2 + w * h),
                                         (p12_u16_2 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(p12_u16, p12_u16_2, planar_size));

  st_test_free(pg);
  st_test_free(p12_u16);
  st_test_free(p12_u16_2);
}

TEST(Cvt, gbrp12le_to_rfc4175_444le12) {
  test_cvt_gbrp12le_to_rfc4175_444le12(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                       MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, gbrp12le_to_rfc4175_444le12_scalar) {
  test_cvt_gbrp12le_to_rfc4175_444le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                       MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444be12_to_444le12(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_be* pg_be =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_le =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_444be12_to_444le12_simd(pg_be, pg_le, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_444be12_simd(pg_le, pg_be_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_444be12_to_444le12) {
  test_cvt_rfc4175_444be12_to_444le12(1920, 1080, MTL_SIMD_LEVEL_MAX, MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444be12_to_444le12_scalar) {
  test_cvt_rfc4175_444be12_to_444le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

static void test_cvt_rfc4175_444le12_to_444be12(int w, int h,
                                                enum mtl_simd_level cvt_level,
                                                enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg_le =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_le_2 =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_le_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_le_2) st_test_free(pg_le_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_444le12_to_444be12_simd(pg_le, pg_be, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444be12_to_444le12_simd(pg_be, pg_le_2, w, h, back_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_le, pg_le_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_le_2);
}

static void test_cvt_rfc4175_444le12_to_444be12_2(int w, int h,
                                                  enum mtl_simd_level cvt_level,
                                                  enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_le* pg_le =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_be || !pg_le || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_be) st_test_free(pg_be);
    if (pg_le) st_test_free(pg_le);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_le, fb_pg2_size, 0);

  ret = st20_rfc4175_444le12_to_444be12_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_444be12_simd(pg_le, pg_be_2, w, h, cvt_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
}

TEST(Cvt, rfc4175_444le12_to_444be12) {
  test_cvt_rfc4175_444le12_to_444be12_2(1920, 1080, MTL_SIMD_LEVEL_MAX,
                                        MTL_SIMD_LEVEL_MAX);
}

TEST(Cvt, rfc4175_444le12_to_444be12_scalar) {
  test_cvt_rfc4175_444le12_to_444be12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                      MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_444be12_444le12_444p12le(int w, int h,
                                                         enum mtl_simd_level cvt1_level,
                                                         enum mtl_simd_level cvt2_level,
                                                         enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_be* pg_be =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_le =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p12_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p12_u16) st_test_free(p12_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_444be12_to_444le12_simd(pg_be, pg_le, w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_yuv444p12le(pg_le, p12_u16, (p12_u16 + w * h),
                                            (p12_u16 + w * h * 2), w, h);
  EXPECT_EQ(0, ret);

  ret = st20_444p12le_to_rfc4175_444be12_simd(
      p12_u16, (p12_u16 + w * h), (p12_u16 + w * h * 2), pg_be_2, w, h, cvt3_level);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rotate_rfc4175_444be12_444le12_444p12le_scalar) {
  test_rotate_rfc4175_444be12_444le12_444p12le(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                               MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_rotate_rfc4175_444be12_444p12le_444le12(int w, int h,
                                                         enum mtl_simd_level cvt1_level,
                                                         enum mtl_simd_level cvt2_level,
                                                         enum mtl_simd_level cvt3_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 9 / 2;
  struct st20_rfc4175_444_12_pg2_be* pg_be =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);
  struct st20_rfc4175_444_12_pg2_le* pg_le =
      (struct st20_rfc4175_444_12_pg2_le*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 3 * sizeof(uint16_t);
  uint16_t* p12_u16 = (uint16_t*)st_test_zmalloc(planar_size);
  struct st20_rfc4175_444_12_pg2_be* pg_be_2 =
      (struct st20_rfc4175_444_12_pg2_be*)st_test_zmalloc(fb_pg2_size);

  if (!pg_le || !pg_be || !p12_u16 || !pg_be_2) {
    EXPECT_EQ(0, 1);
    if (pg_le) st_test_free(pg_le);
    if (pg_be) st_test_free(pg_be);
    if (p12_u16) st_test_free(p12_u16);
    if (pg_be_2) st_test_free(pg_be_2);
    return;
  }

  st_test_rand_data((uint8_t*)pg_be, fb_pg2_size, 0);

  ret = st20_rfc4175_444be12_to_444p12le_simd(pg_be, p12_u16, (p12_u16 + w * h),
                                              (p12_u16 + w * h * 2), w, h, cvt1_level);
  EXPECT_EQ(0, ret);

  ret = st20_444p12le_to_rfc4175_444le12(p12_u16, (p12_u16 + w * h),
                                         (p12_u16 + w * h * 2), pg_le, w, h);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_444le12_to_444be12(pg_le, pg_be_2, w, h);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(pg_be, pg_be_2, fb_pg2_size));

  st_test_free(pg_be);
  st_test_free(pg_le);
  st_test_free(pg_be_2);
  st_test_free(p12_u16);
}

TEST(Cvt, rotate_rfc4175_444be12_444p12le_444le12_scalar) {
  test_rotate_rfc4175_444be12_444p12le_444le12(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                               MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

static void test_am824_to_aes3(int blocks) {
  int ret;
  int subframes = blocks * 2 * 192;
  size_t blocks_size = subframes * 4;
  struct st31_aes3* b_aes3 = (struct st31_aes3*)st_test_zmalloc(blocks_size);
  struct st31_am824* b_am824 = (struct st31_am824*)st_test_zmalloc(blocks_size);
  struct st31_am824* b_am824_2 = (struct st31_am824*)st_test_zmalloc(blocks_size);
  if (!b_aes3 || !b_am824 || !b_am824_2) {
    EXPECT_EQ(0, 1);
    if (b_aes3) st_test_free(b_aes3);
    if (b_am824) st_test_free(b_am824);
    if (b_am824_2) st_test_free(b_am824_2);
    return;
  }

  st_test_rand_data((uint8_t*)b_am824, blocks_size, 0);
  /* set 'b' and 'f' for subframes */
  struct st31_am824* sf_am824 = b_am824;
  for (int i = 0; i < subframes; i++) {
    sf_am824->unused = 0;
    if (i % (192 * 2) == 0) {
      sf_am824->b = 1;
      sf_am824->f = 1;
    } else if (i % 2 == 0) {
      sf_am824->b = 0;
      sf_am824->f = 1;
    } else {
      sf_am824->b = 0;
      sf_am824->f = 0;
    }
    sf_am824++;
  }

  ret = st31_am824_to_aes3(b_am824, b_aes3, subframes);
  EXPECT_EQ(0, ret);

  ret = st31_aes3_to_am824(b_aes3, b_am824_2, subframes);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(b_am824, b_am824_2, blocks_size));

  st_test_free(b_aes3);
  st_test_free(b_am824);
  st_test_free(b_am824_2);
}

TEST(Cvt, st31_am824_to_aes3) {
  test_am824_to_aes3(1);
  test_am824_to_aes3(10);
  test_am824_to_aes3(100);
}

static void test_aes3_to_am824(int blocks) {
  int ret;
  int subframes = blocks * 2 * 192;
  size_t blocks_size = subframes * 4;
  struct st31_aes3* b_aes3 = (struct st31_aes3*)st_test_zmalloc(blocks_size);
  struct st31_am824* b_am824 = (struct st31_am824*)st_test_zmalloc(blocks_size);
  struct st31_aes3* b_aes3_2 = (struct st31_aes3*)st_test_zmalloc(blocks_size);
  if (!b_aes3 || !b_am824 || !b_aes3_2) {
    EXPECT_EQ(0, 1);
    if (b_aes3) st_test_free(b_aes3);
    if (b_am824) st_test_free(b_am824);
    if (b_aes3_2) st_test_free(b_aes3_2);
    return;
  }

  st_test_rand_data((uint8_t*)b_am824, blocks_size, 0);
  /* set 'b' and 'f' for subframes */
  struct st31_aes3* sf_aes3 = b_aes3;
  for (int i = 0; i < subframes; i++) {
    if (i % (192 * 2) == 0) {
      sf_aes3->preamble = 0x2;
    } else if (i % 2 == 0) {
      sf_aes3->preamble = 0x0;
    } else {
      sf_aes3->preamble = 0x1;
    }
    sf_aes3++;
  }

  ret = st31_aes3_to_am824(b_aes3, b_am824, subframes);
  EXPECT_EQ(0, ret);

  ret = st31_am824_to_aes3(b_am824, b_aes3_2, subframes);
  EXPECT_EQ(0, ret);

  EXPECT_EQ(0, memcmp(b_aes3, b_aes3_2, blocks_size));

  st_test_free(b_aes3);
  st_test_free(b_am824);
  st_test_free(b_aes3_2);
}

TEST(Cvt, st31_aes3_to_am824) {
  test_aes3_to_am824(1);
  test_aes3_to_am824(10);
  test_aes3_to_am824(100);
}

static void frame_malloc(struct st_frame* frame, uint8_t rand, bool align) {
  int planes = st_frame_fmt_planes(frame->fmt);
  size_t fb_size = 0;
  for (int plane = 0; plane < planes; plane++) {
    size_t least_line_size = st_frame_least_linesize(frame->fmt, frame->width, plane);
    frame->linesize[plane] = align ? MTL_ALIGN(least_line_size, 512) : least_line_size;
    fb_size += frame->linesize[plane] * frame->height;
  }
  uint8_t* fb = (uint8_t*)st_test_zmalloc(fb_size);
  if (!fb) return;
  if (rand) { /* fill the framebuffer */
    st_test_rand_data(fb, fb_size, rand);
    if (frame->fmt == ST_FRAME_FMT_YUV422PLANAR10LE) {
      /* only LSB 10 valid */
      uint16_t* p10_u16 = (uint16_t*)fb;
      for (size_t j = 0; j < (fb_size / 2); j++) {
        p10_u16[j] &= 0x3ff; /* only 10 bit */
      }
    } else if (frame->fmt == ST_FRAME_FMT_Y210) {
      /* only MSB 10 valid */
      uint16_t* y210_u16 = (uint16_t*)fb;
      for (size_t j = 0; j < (fb_size / 2); j++) {
        y210_u16[j] &= 0xffc0; /* only 10 bit */
      }
    } else if (frame->fmt == ST_FRAME_FMT_V210) {
      uint32_t* v210_word = (uint32_t*)fb;
      for (size_t j = 0; j < (fb_size / 4); j++) {
        v210_word[j] &= 0x3fffffff; /* only 30 bit */
      }
    }
  }
  frame->addr[0] = fb;
  for (int plane = 1; plane < planes; plane++) {
    frame->addr[plane] =
        (uint8_t*)frame->addr[plane - 1] + st_frame_plane_size(frame, plane - 1);
  }
  frame->data_size = frame->buffer_size = fb_size;
}

static void frame_free(struct st_frame* frame) {
  if (frame->addr[0]) st_test_free(frame->addr[0]);
  int planes = st_frame_fmt_planes(frame->fmt);
  for (int plane = 0; plane < planes; plane++) {
    frame->addr[plane] = NULL;
  }
}

static int frame_compare_each_line(struct st_frame* old_frame,
                                   struct st_frame* new_frame) {
  int ret = 0;
  int planes = st_frame_fmt_planes(old_frame->fmt);
  uint32_t h = st_frame_data_height(old_frame);

  for (int plane = 0; plane < planes; plane++) {
    for (uint32_t line = 0; line < h; line++) {
      uint8_t* old_addr =
          (uint8_t*)old_frame->addr[plane] + old_frame->linesize[plane] * line;
      uint8_t* new_addr =
          (uint8_t*)new_frame->addr[plane] + new_frame->linesize[plane] * line;
      if (memcmp(old_addr, new_addr,
                 st_frame_least_linesize(old_frame->fmt, old_frame->width, plane)))
        ret += -EIO;
    }
  }

  return ret;
}

static void test_st_frame_convert(struct st_frame* src, struct st_frame* dst,
                                  struct st_frame* new_src, bool expect_fail) {
  int ret;
  ret = st_frame_convert(src, dst);
  if (expect_fail)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st_frame_convert(dst, new_src);
  if (expect_fail)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  if (!expect_fail) {
    ret = frame_compare_each_line(src, new_src);
    EXPECT_EQ(0, ret);
  }
}

TEST(Cvt, st_frame_convert_fail_resolution) {
  struct st_frame src, dst, new_src;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&new_src, 0, sizeof(new_src));

  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_Y210;

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = 1080;
  dst.height = 1088;
  test_st_frame_convert(&src, &dst, &new_src, true);

  src.width = new_src.width = 1920;
  dst.width = 1280;
  src.height = new_src.height = dst.height = 1080;
  test_st_frame_convert(&src, &dst, &new_src, true);

  src.width = new_src.width = 1920;
  src.height = new_src.height = 1080;
  dst.width = 3840;
  dst.height = 2160;
  test_st_frame_convert(&src, &dst, &new_src, true);
}

TEST(Cvt, st_frame_convert_fail_fmt) {
  struct st_frame src, dst, new_src;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&new_src, 0, sizeof(new_src));

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;

  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_YUV444PLANAR10LE;
  test_st_frame_convert(&src, &dst, &new_src, true);

  src.fmt = new_src.fmt = ST_FRAME_FMT_Y210;
  dst.fmt = ST_FRAME_FMT_V210;
  test_st_frame_convert(&src, &dst, &new_src, true);

  src.fmt = new_src.fmt = ST_FRAME_FMT_GBRPLANAR10LE;
  dst.fmt = ST_FRAME_FMT_YUV420CUSTOM8;
  test_st_frame_convert(&src, &dst, &new_src, true);
}

TEST(Cvt, st_frame_convert_rotate_no_padding) {
  struct st_frame src, dst, new_src;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&new_src, 0, sizeof(new_src));

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_Y210;
  frame_malloc(&src, 1, false);
  frame_malloc(&dst, 0, false);
  frame_malloc(&new_src, 0, false);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 3840;
  src.height = new_src.height = dst.height = 2160;
  src.fmt = new_src.fmt = ST_FRAME_FMT_V210;
  dst.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  frame_malloc(&src, 2, false);
  frame_malloc(&dst, 0, false);
  frame_malloc(&new_src, 0, false);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  frame_malloc(&src, 3, false);
  frame_malloc(&dst, 0, false);
  frame_malloc(&new_src, 0, false);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);
}

TEST(Cvt, st_frame_convert_rotate_padding) {
  struct st_frame src, dst, new_src;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&new_src, 0, sizeof(new_src));

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  frame_malloc(&src, 1, true);
  frame_malloc(&dst, 0, true);
  frame_malloc(&new_src, 0, true);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 3840;
  src.height = new_src.height = dst.height = 2160;
  src.fmt = new_src.fmt = ST_FRAME_FMT_Y210;
  dst.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  frame_malloc(&src, 2, true);
  frame_malloc(&dst, 0, true);
  frame_malloc(&new_src, 0, true);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_Y210;
  frame_malloc(&src, 3, true);
  frame_malloc(&dst, 0, true);
  frame_malloc(&new_src, 0, true);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);
}

TEST(Cvt, st_frame_convert_rotate_mix_padding) {
  struct st_frame src, dst, new_src;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&new_src, 0, sizeof(new_src));

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  frame_malloc(&src, 1, false);
  frame_malloc(&dst, 0, true);
  frame_malloc(&new_src, 0, true);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 3840;
  src.height = new_src.height = dst.height = 2160;
  src.fmt = new_src.fmt = ST_FRAME_FMT_Y210;
  dst.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  frame_malloc(&src, 2, true);
  frame_malloc(&dst, 0, false);
  frame_malloc(&new_src, 0, true);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);

  src.width = new_src.width = dst.width = 1920;
  src.height = new_src.height = dst.height = 1080;
  src.fmt = new_src.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  dst.fmt = ST_FRAME_FMT_Y210;
  frame_malloc(&src, 3, true);
  frame_malloc(&dst, 0, true);
  frame_malloc(&new_src, 0, false);
  test_st_frame_convert(&src, &dst, &new_src, false);
  frame_free(&src);
  frame_free(&dst);
  frame_free(&new_src);
}

static void test_field_to_frame(mtl_handle mt, uint32_t width, uint32_t height,
                                enum st_frame_fmt fmt) {
  struct st_frame* frame = st_frame_create(mt, fmt, width, height, false);
  struct st_frame* first = st_frame_create(mt, fmt, width, height, true);
  struct st_frame* second = st_frame_create(mt, fmt, width, height, true);
  struct st_frame* back = st_frame_create(mt, fmt, width, height, false);
  int ret;

  if (!frame || !first || !second || !back) {
    EXPECT_EQ(0, 1);
    if (frame) st_frame_free(frame);
    if (first) st_frame_free(first);
    if (second) st_frame_free(second);
    if (back) st_frame_free(back);
    return;
  }
  second->second_field = true;
  st_test_rand_data((uint8_t*)frame->addr[0], frame->buffer_size, 0);

  ret = st_field_split(frame, first, second);
  EXPECT_GE(ret, 0);
  ret = st_field_merge(first, second, back);
  EXPECT_GE(ret, 0);

  /* check the result */
  EXPECT_EQ(0, memcmp(frame->addr[0], back->addr[0], frame->buffer_size));

  st_frame_free(frame);
  st_frame_free(first);
  st_frame_free(second);
  st_frame_free(back);
}

TEST(Cvt, field_to_frame) {
  struct st_tests_context* ctx = st_test_ctx();
  test_field_to_frame(ctx->handle, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  test_field_to_frame(ctx->handle, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE);
}

static void test_cvt_yuv422p16le_to_rfc4175_422be10(int w, int h,
                                                    enum mtl_simd_level cvt_level,
                                                    enum mtl_simd_level back_level) {
  int ret;
  size_t fb_pg2_size = (size_t)w * h * 5 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)st_test_zmalloc(fb_pg2_size);
  size_t planar_size = (size_t)w * h * 2 * sizeof(uint16_t);
  uint16_t* p10_u16_in = (uint16_t*)st_test_zmalloc(planar_size);
  uint16_t* p10_u16_out = (uint16_t*)st_test_zmalloc(planar_size);

  if (!pg || !p10_u16_out || !p10_u16_in) {
    EXPECT_EQ(0, 1);
    if (pg) st_test_free(pg);
    if (p10_u16_out) st_test_free(p10_u16_out);
    if (p10_u16_in) st_test_free(p10_u16_in);
    return;
  }

  uint16_t padding = 0b111111;
  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16_in[i] = (rand() & 0x3ff) << 6; /* 10-bit payload*/
    p10_u16_in[i] |= padding;              /* add 6-bits of padding for testing */
  }

  ret = st20_yuv422p16le_to_rfc4175_422be10_simd(p10_u16_in, (p10_u16_in + w * h),
                                                 (p10_u16_in + w * h * 3 / 2), pg, w, h,
                                                 cvt_level);
  EXPECT_EQ(0, ret);

  ret = st20_rfc4175_422be10_to_yuv422p16le_simd(pg, p10_u16_out, (p10_u16_out + w * h),
                                                 (p10_u16_out + w * h * 3 / 2), w, h,
                                                 back_level);
  EXPECT_EQ(0, ret);

  for (size_t i = 0; i < (planar_size / 2); i++) {
    p10_u16_in[i] &= ~padding; /* clear padding, expected be zero */
  }

  EXPECT_EQ(0, memcmp(p10_u16_in, p10_u16_out, planar_size));

  st_test_free(pg);
  st_test_free(p10_u16_in);
  st_test_free(p10_u16_out);
}

TEST(Cvt, yuv422p16le_to_rfc4175_422be10_scalar) {
  test_cvt_yuv422p16le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_NONE,
                                          MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, yuv422p16le_to_rfc4175_422be10_avx512) {
  test_cvt_yuv422p16le_to_rfc4175_422be10(1920, 1080, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
  test_cvt_yuv422p16le_to_rfc4175_422be10(722, 111, MTL_SIMD_LEVEL_AVX512,
                                          MTL_SIMD_LEVEL_AVX512);
}
