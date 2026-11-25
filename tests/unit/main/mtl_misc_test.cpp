/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * Pure-CPU version, memcpy, and ST 2110-10 media-clock conversion unit tests.
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <st_api.h>

#include <cstdint>
#include <cstring>

TEST(Misc, version) {
  const char* version_display = mtl_version();
  EXPECT_NE(version_display, nullptr);

  uint32_t version_no =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR, MTL_VERSION_LAST);
  EXPECT_EQ((uint32_t)MTL_VERSION, version_no);
}

TEST(Misc, version_compare) {
  auto version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR + 1, MTL_VERSION_MINOR, MTL_VERSION_LAST);
  EXPECT_LT(MTL_VERSION, version_next);
  version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR + 1, MTL_VERSION_LAST);
  EXPECT_LT(MTL_VERSION, version_next);
  version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR, MTL_VERSION_LAST + 1);
  EXPECT_LT(MTL_VERSION, version_next);
}

static void mtl_memcpy_test(size_t size) {
  ASSERT_GT(size, 0u);
  char* src = new char[size];
  char* dst = new char[size];

  for (size_t i = 0; i < size; i++) src[i] = (char)i;
  memset(dst, 0, size);

  mtl_memcpy(dst, src, size);
  EXPECT_EQ(0, memcmp(src, dst, size));

  delete[] src;
  delete[] dst;
}

TEST(Misc, memcpy) {
  mtl_memcpy_test(1);
  mtl_memcpy_test(4096);
  mtl_memcpy_test(4096 + 100);
}

namespace {

constexpr uint32_t kVideoSamplingRate = 90 * 1000;
constexpr uint64_t kStartTai = 1764762541892350000ULL;
constexpr uint64_t kNsPerSecond = 1000000000ULL;

uint64_t gcd64(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

uint64_t tai_after_iterations(uint64_t start, uint64_t step_num, uint64_t step_den,
                              size_t iteration) {
  const long double step = (long double)step_num / (long double)step_den;
  const long double offset = step * (long double)iteration;
  return start + (uint64_t)offset;
}

uint32_t expected_floor(uint64_t numerator, uint64_t denominator) {
  return (uint32_t)(numerator / denominator);
}

uint32_t expected_ceil(uint64_t numerator, uint64_t denominator) {
  return (uint32_t)((numerator + denominator - 1) / denominator);
}

struct cadence_case {
  const char* label;
  uint32_t sampling_rate;
  uint64_t step_num;
  uint64_t step_den;
  size_t samples;
};

void ExpectCadence(const cadence_case& tc) {
  SCOPED_TRACE(tc.label);
  const uint64_t diff_num = tc.step_num * tc.sampling_rate;
  const uint64_t diff_den = tc.step_den * kNsPerSecond;
  const uint32_t diff_floor = expected_floor(diff_num, diff_den);
  const uint32_t diff_ceil = expected_ceil(diff_num, diff_den);
  uint32_t prev = st10_tai_to_media_clk(kStartTai, tc.sampling_rate);

  if (diff_floor == diff_ceil) {
    for (size_t i = 1; i < tc.samples; ++i) {
      const uint64_t tai_ns =
          tai_after_iterations(kStartTai, tc.step_num, tc.step_den, i);
      const uint32_t current = st10_tai_to_media_clk(tai_ns, tc.sampling_rate);
      const uint32_t diff = current - prev;
      EXPECT_EQ(diff, diff_floor) << "iteration=" << i << " diff=" << diff;
      prev = current;
    }
    return;
  }

  const uint64_t remainder = diff_num % diff_den;
  ASSERT_NE(remainder, 0ULL);

  const uint64_t g = gcd64(remainder, diff_den);
  const uint32_t ceil_run = (uint32_t)(remainder / g);
  const uint32_t floor_run = (uint32_t)((diff_den / g) - ceil_run);

  bool saw_floor = false;
  bool saw_ceil = false;
  uint32_t run_value = 0;
  uint32_t run_length = 0;
  bool have_run = false;

  for (size_t i = 1; i < tc.samples; ++i) {
    const uint64_t tai_ns = tai_after_iterations(kStartTai, tc.step_num, tc.step_den, i);
    const uint32_t current = st10_tai_to_media_clk(tai_ns, tc.sampling_rate);

    const uint32_t diff = current - prev;
    EXPECT_GE(diff, diff_floor) << "iteration=" << i << " diff=" << diff;
    EXPECT_LE(diff, diff_ceil) << "iteration=" << i << " diff=" << diff;
    EXPECT_TRUE(diff == diff_floor || diff == diff_ceil)
        << "iteration=" << i << " diff=" << diff << " allowed {" << diff_floor << ", "
        << diff_ceil << "}";

    if (!have_run || diff != run_value) {
      run_value = diff;
      run_length = 1;
      have_run = true;
    } else {
      run_length += 1;
    }

    const uint32_t allowed_run = (diff == diff_ceil) ? ceil_run : floor_run;
    EXPECT_LE(run_length, allowed_run)
        << "iteration=" << i << " diff=" << diff << " exceeded cadence run";

    if (diff == diff_floor) saw_floor = true;
    if (diff == diff_ceil) saw_ceil = true;

    prev = current;
  }

  EXPECT_TRUE(saw_floor) << "missing floor diff";
  EXPECT_TRUE(saw_ceil) << "missing ceil diff";
}

void st10_timestamp_roundtrip(uint32_t sampling_rate) {
  const uint64_t tai1 = kStartTai;
  const uint64_t delta_ns = 100000ULL; /* 100 us */
  const uint64_t tai2 = tai1 + delta_ns;

  uint32_t media1 = st10_tai_to_media_clk(tai1, sampling_rate);
  uint32_t media2 = st10_tai_to_media_clk(tai2, sampling_rate);
  EXPECT_GT(media2, media1);

  uint64_t ns_delta = st10_media_clk_to_ns(media2 - media1, sampling_rate);
  EXPECT_NEAR((double)ns_delta, (double)delta_ns, (double)kNsPerSecond / sampling_rate);
}

}  // namespace

TEST(Misc, St10_timestamp) {
  st10_timestamp_roundtrip(90 * 1000);
  st10_timestamp_roundtrip(48 * 1000);
  st10_timestamp_roundtrip(96 * 1000);
}

TEST(Misc, TaiToMClkRoundsDownOnExactHalf) {
  constexpr uint64_t tie_tai = 50000ULL; /* produces remainder == divisor / 2 */
  const uint32_t result = st10_tai_to_media_clk(tie_tai, kVideoSamplingRate);
  EXPECT_EQ(result, 4U);
}

TEST(Misc, TaiToMClkRoundsUpWhenPastHalf) {
  constexpr uint64_t tai = 5556ULL; /* produces remainder > divisor / 2 */
  const uint32_t result = st10_tai_to_media_clk(tai, kVideoSamplingRate);
  EXPECT_EQ(result, 1U);
}

TEST(Misc, TaiToMClkMatchesCommonFrameRates) {
  const cadence_case cases[] = {
      {"59.94fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 60000ULL, 200},
      {"29.97fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 30000ULL, 120},
      {"23.98fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 24000ULL, 120},
      {"120fps", kVideoSamplingRate, kNsPerSecond, 120ULL, 120},
      {"50fps", kVideoSamplingRate, kNsPerSecond, 50ULL, 120},
  };

  for (size_t idx = 0; idx < (sizeof(cases) / sizeof(cases[0])); ++idx) {
    ExpectCadence(cases[idx]);
  }
}

TEST(Misc, MClkToTaiConvertsExactSecondWithoutRounding) {
  const uint64_t ns = st10_media_clk_to_ns(kVideoSamplingRate, kVideoSamplingRate);
  EXPECT_EQ(ns, kNsPerSecond);
}

TEST(Misc, MClkToTaiRoundsUpWhenPastHalf) {
  constexpr uint32_t media_ticks = 5; /* remainder 50,000 (> 45,000) */
  const uint64_t ns = st10_media_clk_to_ns(media_ticks, kVideoSamplingRate);
  EXPECT_EQ(ns, 55556ULL);
}

TEST(Misc, MClkToTaiRoundsDownOnExactHalf) {
  constexpr uint32_t kCustomSamplingRate = 1024;
  constexpr uint32_t media_ticks = 1; /* remainder == sampling_rate / 2 */
  const uint64_t ns = st10_media_clk_to_ns(media_ticks, kCustomSamplingRate);
  EXPECT_EQ(ns, 976562ULL);
}

TEST(Misc, MClkToTaiHandlesAudioSamplingRates) {
  struct media_case {
    const char* label;
    uint32_t sampling_rate;
    uint32_t ticks;
    uint64_t expected_ns;
  };

  const media_case cases[] = {
      {"48kHz one sample", 48 * 1000, 1, 20833ULL},
      {"48kHz millisecond", 48 * 1000, 48, 1000000ULL},
      {"96kHz rounding", 96 * 1000, 5, 52083ULL},
      {"44.1kHz fractional", 44100, 147, 3333333ULL},
  };

  for (size_t idx = 0; idx < (sizeof(cases) / sizeof(cases[0])); ++idx) {
    const media_case& tc = cases[idx];
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(st10_media_clk_to_ns(tc.ticks, tc.sampling_rate), tc.expected_ns);
  }
}

TEST(St10Conversions, ZeroSamplingRateIsGraceful) {
  EXPECT_EQ(st10_tai_to_media_clk(123456789ULL, 0), 0U);
  EXPECT_EQ(st10_media_clk_to_ns(1234U, 0), 0U);
}
