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

using Pg10Be = struct st20_rfc4175_422_10_pg2_be;
using Pg10Le = struct st20_rfc4175_422_10_pg2_le;
using Pg12Be = struct st20_rfc4175_422_12_pg2_be;
using Pg12Le = struct st20_rfc4175_422_12_pg2_le;
using Pg8Le = struct st20_rfc4175_422_8_pg2_le;
using CvtCaseFn = void (*)(mtl_handle st, mtl_udma_handle dma, int w, int h,
                           enum mtl_simd_level lvl);

struct CvtDimCase {
  int w;
  int h;
};

struct CvtCase {
  const char* name;
  CvtCaseFn fn;
  bool is_v210;
  bool has_vbmi2;
};

void fill_masked_u16(uint16_t* dst, size_t count, uint16_t mask) {
  st_test_rand_data(reinterpret_cast<uint8_t*>(dst), count * sizeof(uint16_t), 0);
  for (size_t i = 0; i < count; i++) dst[i] &= mask;
}

template <typename SrcT, typename DstT, typename DmaFn, typename SimdFn, typename InvFn>
void run_packed_dma(mtl_handle st, mtl_udma_handle dma, int w, int h,
                    enum mtl_simd_level lvl, size_t src_sz, size_t dst_sz, uint16_t mask,
                    DmaFn dma_fn, SimdFn simd_fn, InvFn inv_fn) {
  mtl_test::ScopedHpBuf src(st, src_sz, MTL_PORT_P, true);
  ASSERT_TRUE(src);
  ASSERT_NE(src.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> mid(dst_sz, 0), back(src_sz, 0);

  if (mask)
    fill_masked_u16(static_cast<uint16_t*>(src.data()), src_sz / sizeof(uint16_t), mask);
  else
    st_test_rand_data(src.bytes(), src_sz, 0);

  auto* s = static_cast<SrcT*>(src.data());
  auto* d = reinterpret_cast<DstT*>(mid.data());
  int ret = (lvl == MTL_SIMD_LEVEL_MAX) ? dma_fn(dma, s, src.iova(), d, w, h)
                                        : simd_fn(dma, s, src.iova(), d, w, h, lvl);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0,
            inv_fn(d, reinterpret_cast<SrcT*>(back.data()), w, h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(src.bytes(), back.data(), src_sz));
}

template <typename PgT, typename DmaFn, typename SimdFn, typename InvFn>
void run_planar_dma(mtl_handle st, mtl_udma_handle dma, int w, int h,
                    enum mtl_simd_level lvl, size_t pg_sz, DmaFn dma_fn, SimdFn simd_fn,
                    InvFn inv_fn) {
  const size_t px = static_cast<size_t>(w) * h;
  mtl_test::ScopedHpBuf pg(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg);
  ASSERT_NE(pg.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> back(pg_sz, 0);
  std::vector<uint16_t> pl(px * 2, 0);

  st_test_rand_data(pg.bytes(), pg_sz, 0);
  auto* s = static_cast<PgT*>(pg.data());
  uint16_t *y = pl.data(), *b = y + px, *r = y + px * 3 / 2;
  int ret = (lvl == MTL_SIMD_LEVEL_MAX) ? dma_fn(dma, s, pg.iova(), y, b, r, w, h)
                                        : simd_fn(dma, s, pg.iova(), y, b, r, w, h, lvl);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(
      0, inv_fn(y, b, r, reinterpret_cast<PgT*>(back.data()), w, h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(pg.bytes(), back.data(), pg_sz));
}

void cvt_yuv422p10le_to_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                                enum mtl_simd_level lvl) {
  const size_t px = static_cast<size_t>(w) * h;
  const size_t pg_sz = px * 5 / 2, pl_sz = px * 2 * sizeof(uint16_t);
  mtl_test::ScopedHpBuf p10(st, pl_sz, MTL_PORT_P, true);
  ASSERT_TRUE(p10);
  ASSERT_NE(p10.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg(pg_sz, 0);
  std::vector<uint16_t> p10_2(px * 2, 0);

  auto* y = static_cast<uint16_t*>(p10.data());
  fill_masked_u16(y, px * 2, 0x3ff);
  auto* dst = reinterpret_cast<Pg10Be*>(pg.data());
  const mtl_iova_t bi = p10.iova() + static_cast<mtl_iova_t>(px * 2);
  const mtl_iova_t ri = p10.iova() + static_cast<mtl_iova_t>(px * 3);
  int ret = (lvl == MTL_SIMD_LEVEL_MAX)
                ? st20_yuv422p10le_to_rfc4175_422be10_dma(dma, y, p10.iova(), y + px, bi,
                                                          y + px * 3 / 2, ri, dst, w, h)
                : st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
                      dma, y, p10.iova(), y + px, bi, y + px * 3 / 2, ri, dst, w, h, lvl);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, st20_rfc4175_422be10_to_yuv422p10le_simd(
                   dst, p10_2.data(), p10_2.data() + px, p10_2.data() + px * 3 / 2, w, h,
                   MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, memcmp(y, p10_2.data(), pl_sz));
}

void cvt_422be10_to_422le8(mtl_handle st, mtl_udma_handle dma, int w, int h,
                           enum mtl_simd_level lvl) {
  const size_t px = static_cast<size_t>(w) * h;
  const size_t sz_10 = px * 5 / 2, sz_8 = px * 2;
  mtl_test::ScopedHpBuf pg_10(st, sz_10, MTL_PORT_P, true);
  ASSERT_TRUE(pg_10);
  ASSERT_NE(pg_10.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_8(sz_8, 0), pg_8_2(sz_8, 0);

  auto* src = static_cast<Pg10Be*>(pg_10.data());
  st_test_rand_data(pg_8.data(), sz_8, 0);
  const auto* p8 = reinterpret_cast<const Pg8Le*>(pg_8.data());
  for (size_t i = 0; i < px / 2; i++) {
    src[i].Cb00 = p8[i].Cb00;
    src[i].Y00 = p8[i].Y00 >> 2;
    src[i].Cb00_ = 0;
    src[i].Y00_ = (p8[i].Y00 & 0x3) << 2;
    src[i].Cr00 = p8[i].Cr00 >> 4;
    src[i].Y01 = p8[i].Y01 >> 6;
    src[i].Cr00_ = (p8[i].Cr00 & 0xF) << 2;
    src[i].Y01_ = p8[i].Y01 << 2;
  }

  auto* dst = reinterpret_cast<Pg8Le*>(pg_8_2.data());
  int ret = (lvl == MTL_SIMD_LEVEL_MAX)
                ? st20_rfc4175_422be10_to_422le8_dma(dma, src, pg_10.iova(), dst, w, h)
                : st20_rfc4175_422be10_to_422le8_simd_dma(dma, src, pg_10.iova(), dst, w,
                                                          h, lvl);
  EXPECT_EQ(0, ret);
  EXPECT_EQ(0, memcmp(pg_8.data(), pg_8_2.data(), sz_8));
}

void cvt_422be10_to_v210(mtl_handle st, mtl_udma_handle dma, int w, int h,
                         enum mtl_simd_level lvl) {
  const bool fail = ((w * h) % 6) != 0; /* v210 requires pg count multiple of 3 */
  const size_t px = static_cast<size_t>(w) * h;
  const size_t pg_sz = px * 5 / 2, v210_sz = px * 8 / 3;
  mtl_test::ScopedHpBuf pg_be(st, pg_sz, MTL_PORT_P, true);
  ASSERT_TRUE(pg_be);
  ASSERT_NE(pg_be.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be_2(pg_sz, 0), pg_le(pg_sz, 0), v210(v210_sz, 0);

  st_test_rand_data(pg_be.bytes(), pg_sz, 0);
  auto* s = static_cast<Pg10Be*>(pg_be.data());
  int ret =
      (lvl == MTL_SIMD_LEVEL_MAX)
          ? st20_rfc4175_422be10_to_v210_dma(dma, s, pg_be.iova(), v210.data(), w, h)
          : st20_rfc4175_422be10_to_v210_simd_dma(dma, s, pg_be.iova(), v210.data(), w, h,
                                                  lvl);
  EXPECT_EQ(ret != 0, fail);
  EXPECT_EQ(st20_v210_to_rfc4175_422le10(v210.data(), pg_le.data(), w, h) != 0, fail);
  st20_rfc4175_422le10_to_422be10_simd(reinterpret_cast<Pg10Le*>(pg_le.data()),
                                       reinterpret_cast<Pg10Be*>(pg_be_2.data()), w, h,
                                       MTL_SIMD_LEVEL_NONE);
  EXPECT_EQ(memcmp(pg_be.bytes(), pg_be_2.data(), pg_sz) != 0, fail);
}

void cvt_v210_to_422be10(mtl_handle st, mtl_udma_handle dma, int w, int h,
                         enum mtl_simd_level lvl) {
  const bool fail = ((w * h) % 6) != 0; /* v210 requires pg count multiple of 3 */
  const size_t px = static_cast<size_t>(w) * h;
  const size_t pg_sz = px * 5 / 2, v210_sz = px * 8 / 3;
  mtl_test::ScopedHpBuf v210(st, v210_sz, MTL_PORT_P, true);
  ASSERT_TRUE(v210);
  ASSERT_NE(v210.iova(), MTL_BAD_IOVA);
  std::vector<uint8_t> pg_be(pg_sz, 0), pg_be_2(pg_sz, 0);

  st_test_rand_data(pg_be.data(), pg_sz, 0);
  EXPECT_EQ(
      st20_rfc4175_422be10_to_v210_simd(reinterpret_cast<Pg10Be*>(pg_be.data()),
                                        v210.bytes(), w, h, MTL_SIMD_LEVEL_NONE) != 0,
      fail);
  auto* dst = reinterpret_cast<Pg10Be*>(pg_be_2.data());
  int ret =
      (lvl == MTL_SIMD_LEVEL_MAX)
          ? st20_v210_to_rfc4175_422be10_dma(dma, v210.bytes(), v210.iova(), dst, w, h)
          : st20_v210_to_rfc4175_422be10_simd_dma(dma, v210.bytes(), v210.iova(), dst, w,
                                                  h, lvl);
  EXPECT_EQ(ret != 0, fail);
  EXPECT_EQ(memcmp(pg_be.data(), pg_be_2.data(), pg_sz) != 0, fail);
}

#define PACKED_CVT(name, S, D, snum, sden, dnum, dden, mask, fwd, inv, vbmi)          \
  {name,                                                                              \
   [](mtl_handle st, mtl_udma_handle d, int w, int h, enum mtl_simd_level l) {        \
     const size_t px = static_cast<size_t>(w) * h;                                    \
     run_packed_dma<S, D>(st, d, w, h, l, px * (snum) / (sden), px * (dnum) / (dden), \
                          mask, fwd##_dma, fwd##_simd_dma, inv##_simd);               \
   },                                                                                 \
   false, vbmi}

#define PLANAR_CVT(name, PgT, num, fwd, inv, vbmi)                               \
  {name,                                                                         \
   [](mtl_handle st, mtl_udma_handle d, int w, int h, enum mtl_simd_level l) {   \
     run_planar_dma<PgT>(st, d, w, h, l, static_cast<size_t>(w) * h * (num) / 2, \
                         fwd##_dma, fwd##_simd_dma, inv##_simd);                 \
   },                                                                            \
   false, vbmi}

const CvtCase kCvtCases[] = {
    PLANAR_CVT("422be10_to_yuv422p10le", Pg10Be, 5, st20_rfc4175_422be10_to_yuv422p10le,
               st20_yuv422p10le_to_rfc4175_422be10, true),
    {"yuv422p10le_to_422be10", cvt_yuv422p10le_to_422be10, false, false},
    PACKED_CVT("422be10_to_422le10", Pg10Be, Pg10Le, 5, 2, 5, 2, 0,
               st20_rfc4175_422be10_to_422le10, st20_rfc4175_422le10_to_422be10, true),
    PACKED_CVT("422le10_to_422be10", Pg10Le, Pg10Be, 5, 2, 5, 2, 0,
               st20_rfc4175_422le10_to_422be10, st20_rfc4175_422be10_to_422le10, true),
    {"422be10_to_422le8", cvt_422be10_to_422le8, false, true},
    {"422be10_to_v210", cvt_422be10_to_v210, true, true},
    {"v210_to_422be10", cvt_v210_to_422be10, true, true},
    PACKED_CVT("422be10_to_y210", Pg10Be, uint16_t, 5, 2, 4, 1, 0,
               st20_rfc4175_422be10_to_y210, st20_y210_to_rfc4175_422be10, false),
    PACKED_CVT("y210_to_422be10", uint16_t, Pg10Be, 4, 1, 5, 2, 0xFFC0,
               st20_y210_to_rfc4175_422be10, st20_rfc4175_422be10_to_y210, false),
    PLANAR_CVT("422be12_to_yuv422p12le", Pg12Be, 6, st20_rfc4175_422be12_to_yuv422p12le,
               st20_yuv422p12le_to_rfc4175_422be12, false),
    PACKED_CVT("422be12_to_422le12", Pg12Be, Pg12Le, 6, 2, 6, 2, 0,
               st20_rfc4175_422be12_to_422le12, st20_rfc4175_422le12_to_422be12, false),
};

#undef PACKED_CVT
#undef PLANAR_CVT

void run_cvt_dma_tier(enum mtl_simd_level simd_level) {
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

  /* All positive dimensions exceed the 256 KB DMA cache threshold; {602, 211}
   * also exercises invalid v210 pixel-group counts. */
  static constexpr CvtDimCase kAutoDims[] = {{1920, 1080}};
  static constexpr CvtDimCase kStdDims[] = {{1920, 1080}, {602, 211}};
  static constexpr CvtDimCase kV210Dims[] = {
      {1920, 1080}, {600, 211}, {606, 211}, {602, 211}};

  for (const auto& c : kCvtCases) {
    if (simd_level == MTL_SIMD_LEVEL_AVX512_VBMI2 && !c.has_vbmi2) continue;
    const CvtDimCase* dims = (simd_level == MTL_SIMD_LEVEL_MAX)
                                 ? kAutoDims
                                 : (c.is_v210 ? kV210Dims : kStdDims);
    const size_t count = (simd_level == MTL_SIMD_LEVEL_MAX) ? 1 : (c.is_v210 ? 4 : 2);
    for (size_t i = 0; i < count; i++) {
      SCOPED_TRACE(c.name);
      ASSERT_NO_FATAL_FAILURE(c.fn(st, dma.get(), dims[i].w, dims[i].h, simd_level));
    }
  }
}

}  // namespace

TEST(Cvt, dma_auto) {
  run_cvt_dma_tier(MTL_SIMD_LEVEL_MAX);
}
TEST(Cvt, dma_avx512) {
  run_cvt_dma_tier(MTL_SIMD_LEVEL_AVX512);
}
TEST(Cvt, dma_avx512_vbmi2) {
  run_cvt_dma_tier(MTL_SIMD_LEVEL_AVX512_VBMI2);
}
