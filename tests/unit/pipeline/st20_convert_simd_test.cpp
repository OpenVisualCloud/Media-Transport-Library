/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The AVX-512 st20 converters against the scalar ones. On random samples, also with bits
 * above the sample depth, each kernel must write exactly the bytes the scalar converter
 * writes, and so must its _simd() dispatcher at the AVX2, AVX512 and AVX512_VBMI2 levels
 * the CPU has and at AVX512 on a CPU the harness reports as AVX2. The harness counts the
 * kernel calls of the dispatcher: the AVX-512 levels must call the kernel, and the scalar
 * reference, the AVX2 level and the AVX2 CPU must not. Every plane ends at an
 * inaccessible page, so a read or a write past its end crashes the case. On a CPU without
 * AVX-512, or a libmtl built without the kernels, the MatchesScalar cases skip: a green
 * run there, such as on a hosted runner without AVX512VL, has not checked the kernels.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='*St20ConvertSimd*'
 */

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "pipeline/st_convert_harness.h"
#include "st_convert_api.h"

namespace {

/* size bytes that end where an inaccessible page starts */
class GuardedBuf {
 public:
  explicit GuardedBuf(size_t size) : size_(size) {
    const size_t page = sysconf(_SC_PAGESIZE);
    map_size_ = (size + page - 1) / page * page + page;
    map_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
    if (map_ == MAP_FAILED) {
      map_ = nullptr;
      return;
    }
    uint8_t* guard = static_cast<uint8_t*>(map_) + map_size_ - page;
    if (mprotect(guard, page, PROT_NONE) == 0) data_ = guard - size;
  }
  ~GuardedBuf() {
    if (map_) munmap(map_, map_size_);
  }
  GuardedBuf(const GuardedBuf&) = delete;
  GuardedBuf& operator=(const GuardedBuf&) = delete;

  uint8_t* data() {
    return data_;
  }
  size_t size() const {
    return size_;
  }

 private:
  size_t size_;
  size_t map_size_ = 0;
  void* map_ = nullptr;
  uint8_t* data_ = nullptr;
};

using Planes = std::vector<std::unique_ptr<GuardedBuf>>;

struct Kernel {
  const char* name;
  enum ut_cvt_kernel id;
  /* bytes per pixel of each src and dst plane, in eighths */
  std::vector<size_t> src_eighths;
  std::vector<size_t> dst_eighths;
  uint32_t pg_px;
};

void PrintTo(const Kernel& k, std::ostream* os) {
  *os << k.name;
}

const Kernel kKernels[] = {
    {"Yuv422p12leToBe12", UT_CVT_YUV422P12LE_TO_BE12, {16, 8, 8}, {24}, 2},
};

/* the three planes and the pgroups of a conversion, whichever way it goes */
struct Bufs {
  uint16_t* p[3];
  void* pg;
};

Bufs Split(const Kernel& k, Planes& src, Planes& dst) {
  const bool to_pg = k.src_eighths.size() == 3;
  Planes& planar = to_pg ? src : dst;
  Bufs b;
  for (int i = 0; i < 3; i++) b.p[i] = reinterpret_cast<uint16_t*>(planar[i]->data());
  b.pg = (to_pg ? dst : src)[0]->data();
  return b;
}

/* the kernel's _simd() converter at level */
int Simd(const Kernel& k, Planes& src, Planes& dst, uint32_t w, uint32_t h,
         enum mtl_simd_level level) {
  Bufs b = Split(k, src, dst);
  switch (k.id) {
    case UT_CVT_YUV422P12LE_TO_BE12:
      return st20_yuv422p12le_to_rfc4175_422be12_simd(
          b.p[0], b.p[1], b.p[2], static_cast<st20_rfc4175_422_12_pg2_be*>(b.pg), w, h,
          level);
  }
  return -EINVAL;
}

/* the kernel itself, called as its _simd() converter calls it */
int Avx512(const Kernel& k, Planes& src, Planes& dst, uint32_t w, uint32_t h) {
  Bufs b = Split(k, src, dst);
  return ut_cvt_avx512(k.id, b.p, b.pg, w, h);
}

Planes Alloc(const std::vector<size_t>& eighths, uint32_t w, uint32_t h) {
  Planes planes;
  for (size_t e : eighths)
    planes.push_back(std::make_unique<GuardedBuf>((size_t)w * h * e / 8));
  return planes;
}

void FillRandom(Planes& planes, uint32_t seed) {
  uint32_t x = seed;
  for (auto& p : planes) {
    for (size_t i = 0; i < p->size(); i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      p->data()[i] = x;
    }
  }
}

void Fill(Planes& planes, uint8_t value) {
  for (auto& p : planes) memset(p->data(), value, p->size());
}

void ExpectSameBytes(Planes& got, Planes& want) {
  for (size_t i = 0; i < want.size(); i++) {
    const uint8_t* g = got[i]->data();
    const uint8_t* e = want[i]->data();
    if (!memcmp(g, e, want[i]->size())) continue;
    size_t off = 0;
    while (g[off] == e[off]) off++;
    ADD_FAILURE() << "plane " << i << ": first different byte at offset " << off << " of "
                  << want[i]->size();
  }
}

class St20ConvertSimdTest
    : public ::testing::TestWithParam<std::tuple<Kernel, uint32_t, uint32_t>> {};

uint32_t Width(const St20ConvertSimdTest::ParamType& param) {
  return std::get<1>(param) * std::get<0>(param).pg_px;
}

/* The kernels convert w * h pixels as one run, so the scalar tail is w * h modulo a 32
 * pixel batch: at height 1, lines of 1 to 17 pgroups put it below, one pgroup short of,
 * at and past one batch; heights 3 and 17 and lines of 961 pgroups move it, and lines of
 * 1920 pgroups leave none at any height. */
TEST_P(St20ConvertSimdTest, MatchesScalar) {
  const Kernel& k = std::get<0>(GetParam());
  const uint32_t w = Width(GetParam()), h = std::get<2>(GetParam());
  if (mtl_get_simd_level() < MTL_SIMD_LEVEL_AVX512)
    GTEST_SKIP() << "no avx512 on this cpu";
  Planes src = Alloc(k.src_eighths, w, h);
  Planes scalar = Alloc(k.dst_eighths, w, h);
  Planes simd = Alloc(k.dst_eighths, w, h);
  for (auto& p : src) ASSERT_NE(p->data(), nullptr);
  for (auto& p : scalar) ASSERT_NE(p->data(), nullptr);
  for (auto& p : simd) ASSERT_NE(p->data(), nullptr);
  FillRandom(src, 0x2468ace);
  Fill(scalar, 0x5a);
  ut_cvt_avx512_dispatched();
  ASSERT_EQ(Simd(k, src, scalar, w, h, MTL_SIMD_LEVEL_NONE), 0);
  ASSERT_EQ(ut_cvt_avx512_dispatched(), 0);

  Fill(simd, 0xa5);
  const int ret = Avx512(k, src, simd, w, h);
  if (ret == -ENOTSUP) GTEST_SKIP() << "libmtl built without the avx512 converters";
  ASSERT_EQ(ret, 0);
  ExpectSameBytes(simd, scalar);

  for (enum mtl_simd_level level :
       {MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512_VBMI2}) {
    if (mtl_get_simd_level() < level) continue;
    SCOPED_TRACE(mtl_get_simd_level_name(level));
    Fill(simd, 0xa5);
    ASSERT_EQ(Simd(k, src, simd, w, h, level), 0);
    EXPECT_EQ(ut_cvt_avx512_dispatched(), level >= MTL_SIMD_LEVEL_AVX512 ? 1 : 0);
    ExpectSameBytes(simd, scalar);
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllKernels, St20ConvertSimdTest,
    ::testing::Combine(::testing::ValuesIn(kKernels),
                       ::testing::Values(1u, 7u, 8u, 9u, 15u, 16u, 17u, 961u, 1920u),
                       ::testing::Values(1u, 3u, 17u)),
    [](const ::testing::TestParamInfo<St20ConvertSimdTest::ParamType>& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             std::to_string(Width(info.param)) + "x" +
             std::to_string(std::get<2>(info.param));
    });

class St20ConvertSimdCpuTest : public ::testing::TestWithParam<Kernel> {
 protected:
  void TearDown() override {
    ut_cvt_set_cpu_simd_level(MTL_SIMD_LEVEL_MAX);
  }
};

/* the kernel's EVEX instructions would fault on such a CPU */
TEST_P(St20ConvertSimdCpuTest, Avx2CpuStaysScalar) {
  const Kernel& k = GetParam();
  const uint32_t w = 1920, h = 3;
  Planes src = Alloc(k.src_eighths, w, h);
  Planes scalar = Alloc(k.dst_eighths, w, h);
  Planes simd = Alloc(k.dst_eighths, w, h);
  for (auto& p : src) ASSERT_NE(p->data(), nullptr);
  for (auto& p : scalar) ASSERT_NE(p->data(), nullptr);
  for (auto& p : simd) ASSERT_NE(p->data(), nullptr);
  FillRandom(src, 0x2468ace);
  Fill(scalar, 0x5a);
  ASSERT_EQ(Simd(k, src, scalar, w, h, MTL_SIMD_LEVEL_NONE), 0);

  ut_cvt_set_cpu_simd_level(MTL_SIMD_LEVEL_AVX2);
  Fill(simd, 0xa5);
  ut_cvt_avx512_dispatched();
  ASSERT_EQ(Simd(k, src, simd, w, h, MTL_SIMD_LEVEL_AVX512), 0);
  EXPECT_EQ(ut_cvt_avx512_dispatched(), 0);
  ExpectSameBytes(simd, scalar);
}

INSTANTIATE_TEST_SUITE_P(AllKernels, St20ConvertSimdCpuTest,
                         ::testing::ValuesIn(kKernels),
                         [](const ::testing::TestParamInfo<Kernel>& info) {
                           return std::string(info.param.name);
                         });

}  // namespace
