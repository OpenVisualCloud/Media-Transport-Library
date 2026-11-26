/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The le10-to-be10 DMA converter that cannot allocate its DMA context converts on the
 * CPU, in the same direction and between the same buffers.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20CvtDmaFallbackTest.*'
 */

#include <dlfcn.h>
#include <gtest/gtest.h>

#include <cstdint>
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
