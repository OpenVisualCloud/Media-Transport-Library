/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Each per-level SIMD converter against the scalar path of its public *_simd(), called
 * directly: a dispatcher hides a failing SIMD routine by falling back to scalar.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='Converters/SimdScalarTest.*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "convert/simd_scalar_harness.h"

namespace {

constexpr uint32_t kSeed = 2110;
constexpr uint8_t kFill = 0xa5;
constexpr size_t kGuard = 64; /* masked SIMD stores are not instrumented by ASan */
constexpr int kRandomSizes = 3;
constexpr uint32_t kRandomMaxW = 4096;
constexpr uint32_t kRandomMaxH = 32;

struct Size {
  uint32_t w, h;
};

const std::vector<Size> kSizes = [] {
  std::vector<Size> sizes = {{2, 1}, {722, 111}, {1921, 1079}, {1920, 1080}};
  std::mt19937 rng(kSeed);
  for (int i = 0; i < kRandomSizes; i++) /* a braced list is evaluated left to right */
    sizes.push_back(
        {uint32_t(1 + rng() % kRandomMaxW), uint32_t(1 + rng() % kRandomMaxH)});
  return sizes;
}();

using Param = std::tuple<size_t, size_t>;

Size RowSize(const ut_cvt_row& row, size_t size_idx) {
  const Size& raw = kSizes[size_idx];
  return {(raw.w + row.w_align - 1) / row.w_align * row.w_align,
          (raw.h + row.h_align - 1) / row.h_align * row.h_align};
}

std::string Describe(const Param& param) {
  const ut_cvt_row& row = ut_cvt_rows[std::get<0>(param)];
  const Size size = RowSize(row, std::get<1>(param));
  return std::string(row.name) + "_" + std::to_string(size.w) + "x" +
         std::to_string(size.h);
}

bool GuardIntact(const std::vector<uint8_t>& buf, size_t dst_bytes) {
  return std::all_of(buf.data() + dst_bytes, buf.data() + buf.size(),
                     [](uint8_t b) { return b == kFill; });
}

class SimdScalarTest : public testing::TestWithParam<Param> {};
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(SimdScalarTest);

/* Pins: each SIMD routine returns 0 and writes exactly the bytes scalar writes */
TEST_P(SimdScalarTest, MatchesScalar) {
  const ut_cvt_row& row = ut_cvt_rows[std::get<0>(GetParam())];
  if (mtl_get_simd_level() < row.level)
    GTEST_SKIP() << row.name << " needs " << mtl_get_simd_level_name(row.level)
                 << ", this CPU has " << mtl_get_simd_level_name(mtl_get_simd_level());

  /* Arrange */
  const Size size = RowSize(row, std::get<1>(GetParam()));
  std::vector<uint8_t> src(row.src->bytes(size.w, size.h));
  std::mt19937_64 rng(kSeed);
  for (size_t i = 0; i < src.size(); i += sizeof(uint64_t)) {
    const uint64_t word = rng() & row.src->mask;
    memcpy(&src[i], &word, std::min(sizeof(word), src.size() - i));
  }
  const size_t dst_bytes = row.dst->bytes(size.w, size.h);
  std::vector<uint8_t> simd_out(dst_bytes + kGuard, kFill);
  std::vector<uint8_t> scalar_out(dst_bytes + kGuard, kFill);

  /* Act */
  const int simd_ret = row.simd(src.data(), simd_out.data(), size.w, size.h);
  const int scalar_ret = row.scalar(src.data(), scalar_out.data(), size.w, size.h);

  /* Assert */
  const std::string where = Describe(GetParam()) + " seed " + std::to_string(kSeed);
  ASSERT_EQ(scalar_ret, 0) << where;
  ASSERT_EQ(simd_ret, 0) << where;
  EXPECT_TRUE(GuardIntact(scalar_out, dst_bytes))
      << where << ", scalar reference wrote past dst";
  EXPECT_TRUE(GuardIntact(simd_out, dst_bytes)) << where << ", SIMD wrote past dst";
  EXPECT_EQ(memcmp(simd_out.data(), scalar_out.data(), dst_bytes), 0)
      << where << ", first differing byte "
      << std::mismatch(simd_out.begin(), simd_out.end(), scalar_out.begin()).first -
             simd_out.begin();
}

INSTANTIATE_TEST_SUITE_P(Converters, SimdScalarTest,
                         testing::Combine(testing::Range<size_t>(0, ut_cvt_rows_count),
                                          testing::Range<size_t>(0, kSizes.size())),
                         [](const testing::TestParamInfo<Param>& info) {
                           return Describe(info.param);
                         });

}  // namespace
