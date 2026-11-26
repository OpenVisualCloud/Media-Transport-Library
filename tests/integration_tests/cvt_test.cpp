/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * DMA-backed color conversion integration tests. Pure-CPU conversion tests live
 * in tests/unit/fmt/cvt_test.cpp.
 */

#include <cstring>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

namespace {

using CvtCaseFn = void (*)(mtl_handle st, mtl_udma_handle dma, int w, int h,
                           enum mtl_simd_level dma_level);

enum class CvtSweep {
  kAuto1080p,
  kAuto1080pAnd4K,
  kSimdStandard,
  kSimdV210,
};

struct CvtDimCase {
  int w;
  int h;
};

void run_cvt_dma_suite(CvtCaseFn fn, CvtSweep sweep,
                       enum mtl_simd_level simd_level = MTL_SIMD_LEVEL_MAX) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  const enum mtl_simd_level req =
      (simd_level == MTL_SIMD_LEVEL_MAX) ? MTL_SIMD_LEVEL_AVX512 : simd_level;
  if (mtl_get_simd_level() < req) {
    GTEST_SKIP() << "CPU SIMD level " << mtl_get_simd_level_name(mtl_get_simd_level())
                 << " < required " << mtl_get_simd_level_name(req)
                 << " for DMA color conversion";
  }

  mtl_handle st = ctx->handle;
  mtl_test::ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  switch (sweep) {
    case CvtSweep::kAuto1080p:
      fn(st, dma.get(), 1920, 1080, MTL_SIMD_LEVEL_MAX);
      break;
    case CvtSweep::kAuto1080pAnd4K:
      fn(st, dma.get(), 1920, 1080, MTL_SIMD_LEVEL_MAX);
      fn(st, dma.get(), 3840, 2160, MTL_SIMD_LEVEL_MAX);
      break;
    case CvtSweep::kSimdStandard: {
      /* All dimensions exceed the 256 KB DMA cache threshold. */
      const CvtDimCase cases[] = {
          {1920, 1080},
          {602, 211},
      };
      for (const auto& c : cases) {
        fn(st, dma.get(), c.w, c.h, simd_level);
      }
      break;
    }
    case CvtSweep::kSimdV210: {
      /* All positive dimensions exceed 256 KB; {602, 211} exercises invalid pg count. */
      const CvtDimCase cases[] = {
          {1920, 1080},
          {600, 211},
          {606, 211},
          {602, 211},
      };
      for (const auto& c : cases) {
        fn(st, dma.get(), c.w, c.h, simd_level);
      }
      break;
    }
  }
}

void fill_masked_u16(uint16_t* dst, size_t count, uint16_t mask) {
  st_test_rand_data(reinterpret_cast<uint8_t*>(dst), count * sizeof(uint16_t), 0);
  for (size_t i = 0; i < count; i++) {
    dst[i] &= mask;
  }
}

void cvt_rfc4175_422be10_to_yuv422p10le(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                        enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t pixels = static_cast<size_t>(w) * h;
  mtl_test::ScopedHpBuf pg(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg);
  ASSERT_NE(pg.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_2(pg_sz, 0);
  std::vector<uint16_t> p10(pixels * 2, 0);

  st_test_rand_data(pg.bytes(), pg_sz, 0);

  auto* pg_be = static_cast<struct st20_rfc4175_422_10_pg2_be*>(pg.data());
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be10_to_yuv422p10le_dma(
                      dma, pg_be, pg.iova(), p10.data(), p10.data() + pixels,
                      p10.data() + pixels * 3 / 2, w, h)
                : st20_rfc4175_422be10_to_yuv422p10le_simd_dma(
                      dma, pg_be, pg.iova(), p10.data(), p10.data() + pixels,
                      p10.data() + pixels * 3 / 2, w, h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_yuv422p10le_to_rfc4175_422be10_simd(
                   p10.data(), p10.data() + pixels, p10.data() + pixels * 3 / 2,
                   reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_2.data()), w,
                   h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg.bytes(), pg_2.data(), pg_sz));
}

void cvt_yuv422p10le_to_rfc4175_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                        enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t pixels = static_cast<size_t>(w) * h;
  const size_t planar_sz = pixels * 2 * sizeof(uint16_t);
  mtl_test::ScopedHpBuf p10(st, planar_sz, MTL_PORT_P, true);
  ASSERT_TRUE(p10);
  ASSERT_NE(p10.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg(pg_sz, 0);
  std::vector<uint16_t> p10_2(pixels * 2, 0);

  auto* p10_u16 = static_cast<uint16_t*>(p10.data());
  fill_masked_u16(p10_u16, pixels * 2, 0x3ff);

  auto* pg_be = reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg.data());
  const mtl_iova_t b_iova = p10.iova() + static_cast<mtl_iova_t>(pixels * 2);
  const mtl_iova_t r_iova = p10.iova() + static_cast<mtl_iova_t>(pixels * 3);
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_yuv422p10le_to_rfc4175_422be10_dma(
                      dma, p10_u16, p10.iova(), p10_u16 + pixels, b_iova,
                      p10_u16 + pixels * 3 / 2, r_iova, pg_be, w, h)
                : st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
                      dma, p10_u16, p10.iova(), p10_u16 + pixels, b_iova,
                      p10_u16 + pixels * 3 / 2, r_iova, pg_be, w, h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_rfc4175_422be10_to_yuv422p10le_simd(
                   pg_be, p10_2.data(), p10_2.data() + pixels,
                   p10_2.data() + pixels * 3 / 2, w, h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(p10_u16, p10_2.data(), planar_sz));
}

void cvt_rfc4175_422be10_to_422le10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                    enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  mtl_test::ScopedHpBuf pg_be(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_be);
  ASSERT_NE(pg_be.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_le(pg_sz, 0);
  std::vector<uint8_t> pg_be_2(pg_sz, 0);

  st_test_rand_data(pg_be.bytes(), pg_sz, 0);

  auto* src = static_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be.data());
  auto* dst = reinterpret_cast<struct st20_rfc4175_422_10_pg2_le*>(pg_le.data());
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be10_to_422le10_dma(dma, src, pg_be.iova(), dst, w, h)
                : st20_rfc4175_422be10_to_422le10_simd_dma(dma, src, pg_be.iova(), dst, w,
                                                           h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(
      0, st20_rfc4175_422le10_to_422be10_simd(
             dst, reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be_2.data()), w,
             h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz));
}

void cvt_rfc4175_422le10_to_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                    enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  mtl_test::ScopedHpBuf pg_le(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_le);
  ASSERT_NE(pg_le.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be(pg_sz, 0);
  std::vector<uint8_t> pg_le_2(pg_sz, 0);

  st_test_rand_data(pg_le.bytes(), pg_sz, 0);

  auto* src = static_cast<struct st20_rfc4175_422_10_pg2_le*>(pg_le.data());
  auto* dst = reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be.data());
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422le10_to_422be10_dma(dma, src, pg_le.iova(), dst, w, h)
                : st20_rfc4175_422le10_to_422be10_simd_dma(dma, src, pg_le.iova(), dst, w,
                                                           h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(
      0, st20_rfc4175_422be10_to_422le10_simd(
             dst, reinterpret_cast<struct st20_rfc4175_422_10_pg2_le*>(pg_le_2.data()), w,
             h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg_le.bytes(), pg_le_2.data(), pg_sz));
}

void extend_rfc4175_422le8_to_422be10(int w, int h,
                                      const struct st20_rfc4175_422_8_pg2_le* pg_8,
                                      struct st20_rfc4175_422_10_pg2_be* pg_10) {
  const uint32_t cnt = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) / 2;
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
}

void cvt_rfc4175_422be10_to_422le8(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                   enum mtl_simd_level dma_level) {
  const size_t sz_10 = static_cast<size_t>(w) * h * 5 / 2;
  const size_t sz_8 = static_cast<size_t>(w) * h * 2;
  mtl_test::ScopedHpBuf pg_10(st, sz_10, MTL_PORT_P, true);
  ASSERT_TRUE(pg_10);
  ASSERT_NE(pg_10.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_8(sz_8, 0);
  std::vector<uint8_t> pg_8_2(sz_8, 0);

  auto* src = static_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_10.data());
  auto* dst = reinterpret_cast<struct st20_rfc4175_422_8_pg2_le*>(pg_8_2.data());
  st_test_rand_data(pg_8.data(), sz_8, 0);
  extend_rfc4175_422le8_to_422be10(
      w, h, reinterpret_cast<const struct st20_rfc4175_422_8_pg2_le*>(pg_8.data()), src);

  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be10_to_422le8_dma(dma, src, pg_10.iova(), dst, w, h)
                : st20_rfc4175_422be10_to_422le8_simd_dma(dma, src, pg_10.iova(), dst, w,
                                                          h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, memcmp(pg_8.data(), pg_8_2.data(), sz_8));
}

void cvt_rfc4175_422be10_to_v210(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                 enum mtl_simd_level dma_level) {
  const bool fail_case = ((w * h) % 6) != 0; /* v210 requires pg count multiple of 3 */
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t v210_sz = static_cast<size_t>(w) * h * 8 / 3;
  mtl_test::ScopedHpBuf pg_be(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_be);
  ASSERT_NE(pg_be.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be_2(pg_sz, 0);
  std::vector<uint8_t> pg_le(pg_sz, 0);
  std::vector<uint8_t> pg_v210(v210_sz, 0);

  st_test_rand_data(pg_be.bytes(), pg_sz, 0);

  auto* src = static_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be.data());
  int ret =
      (dma_level == MTL_SIMD_LEVEL_MAX)
          ? st20_rfc4175_422be10_to_v210_dma(dma, src, pg_be.iova(), pg_v210.data(), w, h)
          : st20_rfc4175_422be10_to_v210_simd_dma(dma, src, pg_be.iova(), pg_v210.data(),
                                                  w, h, dma_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  ret = st20_v210_to_rfc4175_422le10(pg_v210.data(), pg_le.data(), w, h);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  st20_rfc4175_422le10_to_422be10_simd(
      reinterpret_cast<struct st20_rfc4175_422_10_pg2_le*>(pg_le.data()),
      reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be_2.data()), w, h,
      MTL_SIMD_LEVEL_NONE);

  if (fail_case)
    EXPECT_NE(0, memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz));
  else
    EXPECT_EQ(0, memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz));
}

void cvt_v210_to_rfc4175_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                 enum mtl_simd_level dma_level) {
  const bool fail_case = ((w * h) % 6) != 0; /* v210 requires pg count multiple of 3 */
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t v210_sz = static_cast<size_t>(w) * h * 8 / 3;
  mtl_test::ScopedHpBuf pg_v210(st, v210_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_v210);
  ASSERT_NE(pg_v210.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be(pg_sz, 0);
  std::vector<uint8_t> pg_be_2(pg_sz, 0);

  st_test_rand_data(pg_be.data(), pg_sz, 0);

  int ret = st20_rfc4175_422be10_to_v210_simd(
      reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be.data()), pg_v210.bytes(),
      w, h, MTL_SIMD_LEVEL_NONE);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  auto* dst = reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be_2.data());
  ret = (dma_level == MTL_SIMD_LEVEL_MAX)
            ? st20_v210_to_rfc4175_422be10_dma(dma, pg_v210.bytes(), pg_v210.iova(), dst,
                                               w, h)
            : st20_v210_to_rfc4175_422be10_simd_dma(dma, pg_v210.bytes(), pg_v210.iova(),
                                                    dst, w, h, dma_level);
  if (fail_case)
    EXPECT_NE(0, ret);
  else
    EXPECT_EQ(0, ret);

  if (fail_case) {
    EXPECT_NE(0, memcmp(pg_be.data(), pg_be_2.data(), pg_sz));
  } else {
    EXPECT_EQ(0, memcmp(pg_be.data(), pg_be_2.data(), pg_sz));
  }
}

void cvt_rfc4175_422be10_to_y210(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                 enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t y210_u16s = static_cast<size_t>(w) * h * 2;
  mtl_test::ScopedHpBuf pg_be(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_be);
  ASSERT_NE(pg_be.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be_2(pg_sz, 0);
  std::vector<uint16_t> pg_y210(y210_u16s, 0);

  st_test_rand_data(pg_be.bytes(), pg_sz, 0);

  auto* src = static_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be.data());
  int ret =
      (dma_level == MTL_SIMD_LEVEL_MAX)
          ? st20_rfc4175_422be10_to_y210_dma(dma, src, pg_be.iova(), pg_y210.data(), w, h)
          : st20_rfc4175_422be10_to_y210_simd_dma(dma, src, pg_be.iova(), pg_y210.data(),
                                                  w, h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_y210_to_rfc4175_422be10_simd(
                   pg_y210.data(),
                   reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg_be_2.data()),
                   w, h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz));
}

void cvt_y210_to_rfc4175_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                 enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
  const size_t y210_u16s = static_cast<size_t>(w) * h * 2;
  const size_t y210_sz = y210_u16s * sizeof(uint16_t);
  mtl_test::ScopedHpBuf pg_y210(st, y210_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_y210);
  ASSERT_NE(pg_y210.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg(pg_sz, 0);
  std::vector<uint16_t> pg_y210_2(y210_u16s, 0);

  auto* y210_u16 = static_cast<uint16_t*>(pg_y210.data());
  fill_masked_u16(y210_u16, y210_u16s, 0xFFC0);

  auto* pg_be = reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(pg.data());
  int ret =
      (dma_level == MTL_SIMD_LEVEL_MAX)
          ? st20_y210_to_rfc4175_422be10_dma(dma, y210_u16, pg_y210.iova(), pg_be, w, h)
          : st20_y210_to_rfc4175_422be10_simd_dma(dma, y210_u16, pg_y210.iova(), pg_be, w,
                                                  h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_rfc4175_422be10_to_y210_simd(pg_be, pg_y210_2.data(), w, h,
                                                 MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(y210_u16, pg_y210_2.data(), y210_sz));
}

void cvt_rfc4175_422be12_to_yuv422p12le(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                        enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 6 / 2;
  const size_t pixels = static_cast<size_t>(w) * h;
  mtl_test::ScopedHpBuf pg(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg);
  ASSERT_NE(pg.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_2(pg_sz, 0);
  std::vector<uint16_t> p12(pixels * 2, 0);

  st_test_rand_data(pg.bytes(), pg_sz, 0);

  auto* pg_be = static_cast<struct st20_rfc4175_422_12_pg2_be*>(pg.data());
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be12_to_yuv422p12le_dma(
                      dma, pg_be, pg.iova(), p12.data(), p12.data() + pixels,
                      p12.data() + pixels * 3 / 2, w, h)
                : st20_rfc4175_422be12_to_yuv422p12le_simd_dma(
                      dma, pg_be, pg.iova(), p12.data(), p12.data() + pixels,
                      p12.data() + pixels * 3 / 2, w, h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_yuv422p12le_to_rfc4175_422be12_simd(
                   p12.data(), p12.data() + pixels, p12.data() + pixels * 3 / 2,
                   reinterpret_cast<struct st20_rfc4175_422_12_pg2_be*>(pg_2.data()), w,
                   h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg.bytes(), pg_2.data(), pg_sz));
}

void cvt_rfc4175_422be12_to_422le12(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                    enum mtl_simd_level dma_level) {
  const size_t pg_sz = static_cast<size_t>(w) * h * 6 / 2;
  mtl_test::ScopedHpBuf pg_be(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_be);
  ASSERT_NE(pg_be.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_le(pg_sz, 0);
  std::vector<uint8_t> pg_be_2(pg_sz, 0);

  st_test_rand_data(pg_be.bytes(), pg_sz, 0);

  auto* src = static_cast<struct st20_rfc4175_422_12_pg2_be*>(pg_be.data());
  auto* dst = reinterpret_cast<struct st20_rfc4175_422_12_pg2_le*>(pg_le.data());
  int ret = (dma_level == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be12_to_422le12_dma(dma, src, pg_be.iova(), dst, w, h)
                : st20_rfc4175_422be12_to_422le12_simd_dma(dma, src, pg_be.iova(), dst, w,
                                                           h, dma_level);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(
      0, st20_rfc4175_422le12_to_422be12_simd(
             dst, reinterpret_cast<struct st20_rfc4175_422_12_pg2_be*>(pg_be_2.data()), w,
             h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz));
}

}  // namespace

/* rfc4175_422be10 <-> yuv422p10le */
TEST(Cvt, rfc4175_422be10_to_yuv422p10le_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_yuv422p10le, CvtSweep::kAuto1080p);
}
TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_yuv422p10le, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_yuv422p10le, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

TEST(Cvt, yuv422p10le_to_rfc4175_422be10_dma) {
  run_cvt_dma_suite(cvt_yuv422p10le_to_rfc4175_422be10, CvtSweep::kAuto1080p);
}
TEST(Cvt, yuv422p10le_to_rfc4175_422be10_avx512_dma) {
  run_cvt_dma_suite(cvt_yuv422p10le_to_rfc4175_422be10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}

/* rfc4175_422be10 <-> rfc4175_422le10 */
TEST(Cvt, rfc4175_422be10_to_422le10_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le10, CvtSweep::kAuto1080pAnd4K);
}
TEST(Cvt, rfc4175_422be10_to_422le10_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, rfc4175_422be10_to_422le10_avx512_vbmi_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

TEST(Cvt, rfc4175_422le10_to_422be10_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422le10_to_422be10, CvtSweep::kAuto1080pAnd4K);
}
TEST(Cvt, rfc4175_422le10_to_422be10_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422le10_to_422be10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, rfc4175_422le10_to_422be10_avx512_vbmi_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422le10_to_422be10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

/* rfc4175_422be10 -> rfc4175_422le8 */
TEST(Cvt, rfc4175_422be10_to_422le8_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le8, CvtSweep::kAuto1080pAnd4K);
}
TEST(Cvt, rfc4175_422be10_to_422le8_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le8, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, rfc4175_422be10_to_422le8_avx512_vbmi_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_422le8, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

/* rfc4175_422be10 <-> v210 */
TEST(Cvt, rfc4175_422be10_to_v210_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_v210, CvtSweep::kAuto1080p);
}
TEST(Cvt, rfc4175_422be10_to_v210_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_v210, CvtSweep::kSimdV210,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, rfc4175_422be10_to_v210_avx512_vbmi_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_v210, CvtSweep::kSimdV210,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

TEST(Cvt, v210_to_rfc4175_422be10_dma) {
  run_cvt_dma_suite(cvt_v210_to_rfc4175_422be10, CvtSweep::kAuto1080p);
}
TEST(Cvt, v210_to_rfc4175_422be10_avx512_dma) {
  run_cvt_dma_suite(cvt_v210_to_rfc4175_422be10, CvtSweep::kSimdV210,
                    MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, v210_to_rfc4175_422be10_vbmi_dma) {
  run_cvt_dma_suite(cvt_v210_to_rfc4175_422be10, CvtSweep::kSimdV210,
                    MTL_SIMD_LEVEL_AVX512_VBMI2);
}

/* rfc4175_422be10 <-> y210 */
TEST(Cvt, rfc4175_422be10_to_y210_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_y210, CvtSweep::kAuto1080p);
}
TEST(Cvt, rfc4175_422be10_to_y210_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be10_to_y210, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, y210_to_rfc4175_422be10_dma) {
  run_cvt_dma_suite(cvt_y210_to_rfc4175_422be10, CvtSweep::kAuto1080p);
}
TEST(Cvt, y210_to_rfc4175_422be10_avx512_dma) {
  run_cvt_dma_suite(cvt_y210_to_rfc4175_422be10, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}

/* rfc4175_422be12 -> yuv422p12le and 422le12 */
TEST(Cvt, rfc4175_422be12_to_yuv422p12le_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be12_to_yuv422p12le, CvtSweep::kAuto1080p);
}
TEST(Cvt, rfc4175_422be12_to_yuv422p12le_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be12_to_yuv422p12le, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}

TEST(Cvt, rfc4175_422be12_to_422le12_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be12_to_422le12, CvtSweep::kAuto1080pAnd4K);
}
TEST(Cvt, rfc4175_422be12_to_422le12_avx512_dma) {
  run_cvt_dma_suite(cvt_rfc4175_422be12_to_422le12, CvtSweep::kSimdStandard,
                    MTL_SIMD_LEVEL_AVX512);
}
