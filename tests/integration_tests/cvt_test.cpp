/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * DMA-backed color conversion integration tests. Pure-CPU conversion tests live
 * in tests/unit/fmt/cvt_test.cpp.
 */

#include "log.h"
#include "tests.hpp"

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

static void test_cvt_rfc4175_422be10_to_422le8_dma(mtl_udma_handle dma, int w, int h,
                                                   enum mtl_simd_level cvt_level,
                                                   enum mtl_simd_level /*back_level*/) {
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

static void test_cvt_rfc4175_422be10_to_v210_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level /*back_level*/) {
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

static void test_cvt_rfc4175_422be10_to_y210_dma(mtl_udma_handle dma, int w, int h,
                                                 enum mtl_simd_level cvt_level,
                                                 enum mtl_simd_level /*back_level*/) {
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
