/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The DMA-offloaded st20 converters. The le10-to-be10 one that cannot allocate its DMA
 * context converts on the CPU, in the same direction and between the same buffers. The
 * be12-to-yuv422p12le one, dispatched at AVX512 VBMI2 over a dma_skeleton dmadev, copies
 * the frame through the device and writes what the scalar converter writes.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='*St20CvtDma*'
 */

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <rte_malloc.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "dma/st_cvt_dma_harness.h"

TEST(St20CvtDmaFallbackTest, Le10ToBe10VbmiConvertsWithoutDmaContext) {
  if (mtl_get_simd_level() < MTL_SIMD_LEVEL_AVX512_VBMI2 ||
      !dlsym(RTLD_DEFAULT, "st20_rfc4175_422le10_to_422be10_avx512_vbmi_dma"))
    GTEST_SKIP() << "no AVX512 VBMI2 converter on this CPU or in this libmtl";

  const uint32_t w = 32, h = 2;
  const size_t pg2 = w * h / 2;
  std::vector<uint8_t> be(pg2 * sizeof(st20_rfc4175_422_10_pg2_be));
  std::vector<uint8_t> le(pg2 * sizeof(st20_rfc4175_422_10_pg2_le));
  std::vector<uint8_t> out(be.size(), 0);
  for (size_t i = 0; i < be.size(); i++) be[i] = static_cast<uint8_t>(i * 73 + 41);
  ASSERT_EQ(st20_rfc4175_422be10_to_422le10_simd(
                reinterpret_cast<st20_rfc4175_422_10_pg2_be*>(be.data()),
                reinterpret_cast<st20_rfc4175_422_10_pg2_le*>(le.data()), w, h,
                MTL_SIMD_LEVEL_NONE),
            0);
  const std::vector<uint8_t> le_in = le;

  ASSERT_EQ(
      ut_cvt_dma_le10_to_be10(reinterpret_cast<st20_rfc4175_422_10_pg2_le*>(le.data()),
                              reinterpret_cast<st20_rfc4175_422_10_pg2_be*>(out.data()),
                              w, h, MTL_SIMD_LEVEL_AVX512_VBMI2),
      0);
  ASSERT_GE(ut_cvt_dma_failed_ctx_inits(), 1)
      << "the DMA context allocation never failed";

  EXPECT_EQ(out, be);
  EXPECT_EQ(le, le_in);
}

namespace {
struct St20CvtDmaSkeletonCase {
  uint32_t w, h;
  uint64_t copies; /* of 262080 B (43680 pgroups) each, through 4 cache slots */
  const char* name;
};

void PrintTo(const St20CvtDmaSkeletonCase& c, std::ostream* os) {
  *os << c.w << "x" << c.h;
}
}  // namespace

class St20CvtDmaSkeletonTest : public ::testing::TestWithParam<St20CvtDmaSkeletonCase> {
 protected:
  void SetUp() override {
    if (mtl_get_simd_level() < MTL_SIMD_LEVEL_AVX512_VBMI2 ||
        !dlsym(RTLD_DEFAULT, "st20_rfc4175_422be12_to_yuv422p12le_avx512_vbmi_dma"))
      GTEST_SKIP() << "no AVX512 VBMI2 converter on this CPU or in this libmtl";
    skel_ = ut_cvt_dma_skeleton_create();
    ASSERT_NE(skel_, nullptr);
  }

  void TearDown() override {
    EXPECT_EQ(ut_cvt_dma_skeleton_destroy(skel_), 0);
  }

  ut_cvt_dma_skeleton* skel_ = nullptr;
};

TEST_P(St20CvtDmaSkeletonTest, Be12ToYuv422p12leVbmiMatchesScalar) {
  const uint32_t w = GetParam().w, h = GetParam().h;
  const size_t y_cnt = (size_t)w * h;
  const size_t pg_size = y_cnt / 2 * sizeof(st20_rfc4175_422_12_pg2_be);
  auto* pg = static_cast<st20_rfc4175_422_12_pg2_be*>(rte_malloc(nullptr, pg_size, 64));
  ASSERT_NE(pg, nullptr);
  std::mt19937 rng(1);
  uint8_t* bytes = reinterpret_cast<uint8_t*>(pg);
  std::generate(bytes, bytes + pg_size, [&rng] { return static_cast<uint8_t>(rng()); });

  std::vector<uint16_t> want(y_cnt * 2), got(y_cnt * 2);
  uint16_t* ws = want.data();
  uint16_t* gs = got.data();
  EXPECT_EQ(st20_rfc4175_422be12_to_yuv422p12le_simd(
                pg, ws, ws + y_cnt, ws + y_cnt * 3 / 2, w, h, MTL_SIMD_LEVEL_NONE),
            0);
  EXPECT_EQ(st20_rfc4175_422be12_to_yuv422p12le_simd_dma(
                ut_cvt_dma_skeleton_handle(skel_), pg, rte_malloc_virt2iova(pg), gs,
                gs + y_cnt, gs + y_cnt * 3 / 2, w, h, MTL_SIMD_LEVEL_AVX512_VBMI2),
            0);
  rte_free(pg);

  EXPECT_EQ(ut_cvt_dma_skeleton_completed(skel_), GetParam().copies)
      << "copies the device completed";
  size_t first_diff =
      std::mismatch(got.begin(), got.end(), want.begin()).first - got.begin();
  EXPECT_EQ(first_diff, got.size()) << "first differing sample";
}

/* 1922x1045 is 22 VBMI copies but 23 of the AVX512 kernel's 43648-pgroup ones, and ends
 * in a scalar tail of 5 pgroups. */
INSTANTIATE_TEST_SUITE_P(
    Frames, St20CvtDmaSkeletonTest,
    ::testing::Values(St20CvtDmaSkeletonCase{1920, 1080, 23, "P1080"},
                      St20CvtDmaSkeletonCase{1922, 1045, 22, "W1922H1045"}),
    [](const auto& info) { return std::string(info.param.name); });
