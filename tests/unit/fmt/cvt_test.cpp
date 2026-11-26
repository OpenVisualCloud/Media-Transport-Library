/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * Pure-CPU color conversion, ST 2110-31 AM824/AES3, st_frame_convert, and
 * field split/merge unit tests.
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <mtl_api.h>
#include <st30_api.h>
#include <st_convert_api.h>
#include <st_pipeline_api.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace {

/* Compact dimensions covering full AVX-512 64-byte vectors + scalar tails. */
constexpr int kFullW = 192, kFullH = 64;
constexpr int kTailW = 198, kTailH = 15; /* pg2=99/row, 99*15=1485 (%3==0, %12!=0) */
constexpr int kTail444W = 200, kTail444H = 17; /* multiple of 4 for pg4, odd h */
constexpr int kTail420W = 202, kTail420H = 14; /* even w/h for 4:2:0, odd pg2/row */
constexpr int kV210BadW = 194, kV210BadH = 11; /* w*h % 6 != 0 -> invalid v210 */

uint32_t xorshift32(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

void fill_bytes(uint8_t* p, size_t sz, uint32_t seed = 0x12345678u) {
  for (size_t i = 0; i < sz; ++i) p[i] = static_cast<uint8_t>(xorshift32(seed));
}

void fill_v210(uint8_t* p, size_t sz, uint32_t seed = 0x23456789u) {
  fill_bytes(p, sz, seed);
  for (size_t i = 3; i < sz; i += 4) p[i] &= 0x3Fu;
}

void fill_u16_masked(uint16_t* p, size_t cnt, uint16_t mask,
                     uint32_t seed = 0x3456789Au) {
  for (size_t i = 0; i < cnt; ++i) p[i] = static_cast<uint16_t>(xorshift32(seed)) & mask;
}

bool simd_level_available(enum mtl_simd_level lvl) {
  const enum mtl_simd_level cpu = mtl_get_simd_level();
  if (lvl == MTL_SIMD_LEVEL_NONE) return true;
  if (lvl == MTL_SIMD_LEVEL_MAX) return cpu > MTL_SIMD_LEVEL_NONE;
  return lvl <= cpu;
}

/* Sweep SIMD levels on full + tail dims, cross-checking SIMD vs scalar and all 64
 * remainders (w=2, h=64..127) for pg2 kernels. Levels above host CPU support are
 * skipped rather than silently falling back to scalar self-comparison. */
template <typename Fn>
void sweep_simd_rt(Fn fn, std::initializer_list<enum mtl_simd_level> levels,
                   int tail_w = kTailW, int tail_h = kTailH, bool rem64 = true) {
  int ran = 0;
  for (auto lvl : levels) {
    if (!simd_level_available(lvl)) continue;
    ++ran;
    fn(kFullW, kFullH, lvl, lvl);
    fn(tail_w, tail_h, lvl, lvl);
    if (lvl != MTL_SIMD_LEVEL_NONE && lvl != MTL_SIMD_LEVEL_MAX) {
      fn(tail_w, tail_h, MTL_SIMD_LEVEL_NONE, lvl);
      fn(tail_w, tail_h, lvl, MTL_SIMD_LEVEL_NONE);
      if (rem64) {
        for (int h = 64; h < 128; ++h) fn(2, h, lvl, lvl);
      }
    }
  }
  if (ran == 0) {
    GTEST_SKIP() << "Host CPU SIMD level "
                 << mtl_get_simd_level_name(mtl_get_simd_level())
                 << " does not support any requested SIMD level";
  }
}

template <typename Fn>
void sweep_simd_1way(Fn fn, std::initializer_list<enum mtl_simd_level> levels) {
  int ran = 0;
  for (auto lvl : levels) {
    if (!simd_level_available(lvl)) continue;
    ++ran;
    fn(kFullW, kFullH, lvl);
    fn(kTailW, kTailH, lvl);
    if (lvl != MTL_SIMD_LEVEL_NONE && lvl != MTL_SIMD_LEVEL_MAX) {
      for (int h = 64; h < 128; ++h) fn(2, h, lvl);
    }
  }
  if (ran == 0) {
    GTEST_SKIP() << "Host CPU SIMD level "
                 << mtl_get_simd_level_name(mtl_get_simd_level())
                 << " does not support any requested SIMD level";
  }
}

template <typename T>
T* ptr(std::vector<uint8_t>& v) {
  return reinterpret_cast<T*>(v.data());
}
template <typename SrcPg, typename DstPg, typename FwdFn, typename BackFn>
void check_packed_both(int w, int h, size_t bytes, FwdFn fwd, BackFn back,
                       enum mtl_simd_level fl, enum mtl_simd_level bl) {
  std::vector<uint8_t> a(bytes), b(bytes), c(bytes);
  fill_bytes(a.data(), bytes);
  EXPECT_EQ(0, fwd(ptr<SrcPg>(a), ptr<DstPg>(b), w, h, fl));
  EXPECT_EQ(0, back(ptr<DstPg>(b), ptr<SrcPg>(c), w, h, bl));
  EXPECT_EQ(a, c);
  fill_bytes(b.data(), bytes, 0x99887766u);
  EXPECT_EQ(0, back(ptr<DstPg>(b), ptr<SrcPg>(a), w, h, fl));
  EXPECT_EQ(0, fwd(ptr<SrcPg>(a), ptr<DstPg>(c), w, h, bl));
  EXPECT_EQ(b, c);
}

template <typename SrcPg, typename DstPg, typename FwdFn>
void check_packed_vs_scalar(int w, int h, size_t bytes, FwdFn fwd,
                            enum mtl_simd_level lvl) {
  /* Never compare scalar against scalar on hosts without the requested SIMD level. */
  if (lvl == MTL_SIMD_LEVEL_NONE || !simd_level_available(lvl)) return;
  std::vector<uint8_t> src(bytes), ref(bytes), got(bytes);
  fill_bytes(src.data(), bytes);
  EXPECT_EQ(0, fwd(ptr<SrcPg>(src), ptr<DstPg>(ref), w, h, MTL_SIMD_LEVEL_NONE));
  EXPECT_EQ(0, fwd(ptr<SrcPg>(src), ptr<DstPg>(got), w, h, lvl));
  EXPECT_EQ(ref, got);
}

/* Packed <-> Planar 16-bit bidirectional round-trip (4:2:2 ChromaDiv=2, 4:4:4 =1). */
template <typename Pg, int ChromaDiv, typename ToP, typename ToPg, typename... LvlArgs>
void check_planar16(int w, int h, size_t pg_bytes, uint16_t mask, ToP to_p, ToPg to_pg,
                    LvlArgs... lvls) {
  const size_t px = static_cast<size_t>(w) * h;
  const size_t uv = (ChromaDiv == 2) ? (px * 3 / 2) : (px * 2);
  const size_t n16 = (ChromaDiv == 2) ? (px * 2) : (px * 3);
  std::vector<uint8_t> pg(pg_bytes), pg2(pg_bytes);
  std::vector<uint16_t> p(n16), p2(n16);
  auto call_to_p = [&](std::vector<uint8_t>& s, uint16_t* d, auto... l) {
    return to_p(ptr<Pg>(s), d, d + px, d + uv, w, h, l...);
  };
  auto call_to_pg = [&](uint16_t* s, std::vector<uint8_t>& d, auto... l) {
    return to_pg(s, s + px, s + uv, ptr<Pg>(d), w, h, l...);
  };

  fill_bytes(pg.data(), pg_bytes);
  if constexpr (sizeof...(LvlArgs) == 2) {
    auto [fl, bl] = std::tuple{lvls...};
    EXPECT_EQ(0, call_to_p(pg, p.data(), fl));
    EXPECT_EQ(0, call_to_pg(p.data(), pg2, bl));
    EXPECT_EQ(pg, pg2);
    fill_u16_masked(p.data(), n16, mask);
    EXPECT_EQ(0, call_to_pg(p.data(), pg, fl));
    EXPECT_EQ(0, call_to_p(pg, p2.data(), bl));
  } else {
    EXPECT_EQ(0, call_to_p(pg, p.data()));
    EXPECT_EQ(0, call_to_pg(p.data(), pg2));
    EXPECT_EQ(pg, pg2);
    fill_u16_masked(p.data(), n16, mask);
    EXPECT_EQ(0, call_to_pg(p.data(), pg));
    EXPECT_EQ(0, call_to_p(pg, p2.data()));
  }
  EXPECT_EQ(p, p2);
}

/* 3-way rotations: BE -> LE -> Planar -> BE and BE -> Planar -> LE -> BE. */
template <typename BePg, typename LePg, int ChromaDiv, typename BeToLe, typename LeToP,
          typename PToBe, typename BeToP, typename PToLe, typename LeToBe>
void check_rotations(int w, int h, size_t pg_bytes, BeToLe be2le, LeToP le2p, PToBe p2be,
                     BeToP be2p, PToLe p2le, LeToBe le2be, enum mtl_simd_level l1,
                     enum mtl_simd_level l3) {
  const size_t px = static_cast<size_t>(w) * h;
  const size_t uv = (ChromaDiv == 2) ? (px * 3 / 2) : (px * 2);
  const size_t n16 = (ChromaDiv == 2) ? (px * 2) : (px * 3);
  std::vector<uint8_t> be(pg_bytes), le(pg_bytes), be2(pg_bytes);
  std::vector<uint16_t> p(n16);

  fill_bytes(be.data(), pg_bytes);
  EXPECT_EQ(0, be2le(ptr<BePg>(be), ptr<LePg>(le), w, h, l1));
  EXPECT_EQ(0, le2p(ptr<LePg>(le), p.data(), p.data() + px, p.data() + uv, w, h));
  EXPECT_EQ(0, p2be(p.data(), p.data() + px, p.data() + uv, ptr<BePg>(be2), w, h, l3));
  EXPECT_EQ(be, be2);

  fill_bytes(be.data(), pg_bytes, 0x55667788u);
  EXPECT_EQ(0, be2p(ptr<BePg>(be), p.data(), p.data() + px, p.data() + uv, w, h, l1));
  EXPECT_EQ(0, p2le(p.data(), p.data() + px, p.data() + uv, ptr<LePg>(le), w, h));
  EXPECT_EQ(0, le2be(ptr<LePg>(le), ptr<BePg>(be2), w, h, l3));
  EXPECT_EQ(be, be2);
}

void pack_422_8_to_be10(struct st20_rfc4175_422_10_pg2_be& dst, uint8_t b0, uint8_t y0,
                        uint8_t r0, uint8_t y1) {
  dst.Cb00 = b0;
  dst.Y00 = y0 >> 2;
  dst.Cb00_ = 0;
  dst.Y00_ = (y0 & 0x3) << 2;
  dst.Cr00 = r0 >> 4;
  dst.Y01 = y1 >> 6;
  dst.Cr00_ = (r0 & 0xF) << 2;
  dst.Y01_ = y1 << 2;
}

struct TestFrame {
  struct st_frame f = {};
  std::vector<uint8_t> buf;

  void init(enum st_frame_fmt fmt, uint32_t w, uint32_t h, bool fill, bool align,
            uint32_t seed = 0x5A5A1234u) {
    f = {};
    f.fmt = fmt;
    f.width = w;
    f.height = h;
    const int planes = st_frame_fmt_planes(fmt);
    size_t sz = 0;
    for (int p = 0; p < planes; ++p) {
      const size_t least = st_frame_least_linesize(fmt, w, p);
      f.linesize[p] = align ? MTL_ALIGN(least, 512) : least;
      sz += f.linesize[p] * h;
    }
    buf.assign(sz, 0);
    if (fill) {
      fill_bytes(buf.data(), sz, seed);
      if (fmt == ST_FRAME_FMT_YUV422PLANAR10LE) {
        auto* u16 = reinterpret_cast<uint16_t*>(buf.data());
        for (size_t i = 0; i < sz / 2; ++i) u16[i] &= 0x3FFu;
      } else if (fmt == ST_FRAME_FMT_Y210) {
        auto* u16 = reinterpret_cast<uint16_t*>(buf.data());
        for (size_t i = 0; i < sz / 2; ++i) u16[i] &= 0xFFC0u;
      } else if (fmt == ST_FRAME_FMT_V210) {
        auto* u32 = reinterpret_cast<uint32_t*>(buf.data());
        for (size_t i = 0; i < sz / 4; ++i) u32[i] &= 0x3FFFFFFFu;
      }
    }
    f.addr[0] = buf.data();
    for (int p = 1; p < planes; ++p) {
      f.addr[p] = static_cast<uint8_t*>(f.addr[p - 1]) + st_frame_plane_size(&f, p - 1);
    }
    f.data_size = f.buffer_size = sz;
  }
};

void expect_frames_equal_lines(const struct st_frame& a, const struct st_frame& b) {
  const int planes = st_frame_fmt_planes(a.fmt);
  const uint32_t h = st_frame_data_height(const_cast<struct st_frame*>(&a));
  for (int p = 0; p < planes; ++p) {
    const size_t line_bytes = st_frame_least_linesize(a.fmt, a.width, p);
    for (uint32_t line = 0; line < h; ++line) {
      const uint8_t* pa = static_cast<const uint8_t*>(a.addr[p]) + a.linesize[p] * line;
      const uint8_t* pb = static_cast<const uint8_t*>(b.addr[p]) + b.linesize[p] * line;
      ASSERT_EQ(0, std::memcmp(pa, pb, line_bytes)) << "plane=" << p << " line=" << line;
    }
  }
}

using ScopedFrame = std::unique_ptr<struct st_frame, decltype(&st_frame_free)>;

}  // namespace

TEST(Cvt, simd_level) {
  enum mtl_simd_level cpu_level = mtl_get_simd_level();
  const char* name = mtl_get_simd_level_name(cpu_level);
  EXPECT_GE(cpu_level, MTL_SIMD_LEVEL_NONE);
  EXPECT_LT(cpu_level, MTL_SIMD_LEVEL_MAX);
  ASSERT_NE(name, nullptr);
  RecordProperty("cpu_simd_level", name);
  std::printf("[  SIMD    ] CPU SIMD level: %s (%d)\n", name,
              static_cast<int>(cpu_level));
}

/* ── 4:2:2 10-bit conversions ────────────────────────────────────────── */

TEST(Cvt, rfc4175_422be10_yuv422p10le) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        check_planar16<st20_rfc4175_422_10_pg2_be, 2>(
            w, h, static_cast<size_t>(w) * h * 5 / 2, 0x3FFu,
            st20_rfc4175_422be10_to_yuv422p10le_simd,
            st20_yuv422p10le_to_rfc4175_422be10_simd, fl, bl);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512_VBMI2,
       MTL_SIMD_LEVEL_MAX});
}

TEST(Cvt, rfc4175_422le10_yuv422p10le) {
  for (auto [w, h] : {std::pair{kFullW, kFullH}, std::pair{kTailW, kTailH}}) {
    check_planar16<st20_rfc4175_422_10_pg2_le, 2>(
        w, h, static_cast<size_t>(w) * h * 5 / 2, 0x3FFu,
        st20_rfc4175_422le10_to_yuv422p10le, st20_yuv422p10le_to_rfc4175_422le10);
  }
}

TEST(Cvt, rfc4175_422be10_422le10) {
  const auto levels = {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX512,
                       MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_MAX};
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        check_packed_both<st20_rfc4175_422_10_pg2_be, st20_rfc4175_422_10_pg2_le>(
            w, h, static_cast<size_t>(w) * h * 5 / 2,
            st20_rfc4175_422be10_to_422le10_simd, st20_rfc4175_422le10_to_422be10_simd,
            fl, bl);
      },
      levels);
  for (auto lvl : levels) {
    check_packed_vs_scalar<st20_rfc4175_422_10_pg2_le, st20_rfc4175_422_10_pg2_be>(
        kFullW, kFullH, static_cast<size_t>(kFullW) * kFullH * 5 / 2,
        st20_rfc4175_422le10_to_422be10_simd, lvl);
  }
}

TEST(Cvt, rfc4175_422be10_to_422le8) {
  sweep_simd_1way(
      [](int w, int h, enum mtl_simd_level lvl) {
        const size_t px = static_cast<size_t>(w) * h;
        std::vector<uint8_t> pg10(px * 5 / 2), pg8(px * 2), out(px * 2);
        fill_bytes(pg8.data(), pg8.size());
        auto* s8 = ptr<st20_rfc4175_422_8_pg2_le>(pg8);
        auto* p10 = ptr<st20_rfc4175_422_10_pg2_be>(pg10);
        for (size_t i = 0; i < px / 2; ++i) {
          pack_422_8_to_be10(p10[i], s8[i].Cb00, s8[i].Y00, s8[i].Cr00, s8[i].Y01);
        }
        EXPECT_EQ(0, st20_rfc4175_422be10_to_422le8_simd(
                         p10, ptr<st20_rfc4175_422_8_pg2_le>(out), w, h, lvl));
        EXPECT_EQ(pg8, out);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512_VBMI2,
       MTL_SIMD_LEVEL_MAX});
}

TEST(Cvt, rfc4175_422be10_to_yuv422p8) {
  sweep_simd_1way(
      [](int w, int h, enum mtl_simd_level lvl) {
        const size_t px = static_cast<size_t>(w) * h;
        std::vector<uint8_t> pg10(px * 5 / 2), p8(px * 2), out(px * 2);
        fill_bytes(p8.data(), p8.size());
        auto* p10 = ptr<st20_rfc4175_422_10_pg2_be>(pg10);
        for (size_t i = 0; i < px / 2; ++i) {
          pack_422_8_to_be10(p10[i], p8[px + i], p8[2 * i], p8[px * 3 / 2 + i],
                             p8[2 * i + 1]);
        }
        EXPECT_EQ(
            0, st20_rfc4175_422be10_to_yuv422p8_simd(p10, out.data(), out.data() + px,
                                                     out.data() + px * 3 / 2, w, h, lvl));
        EXPECT_EQ(p8, out);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX2, MTL_SIMD_LEVEL_AVX512,
       MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_MAX});
}

TEST(Cvt, rfc4175_422be10_to_yuv420p8) {
  if (mtl_get_simd_level() < MTL_SIMD_LEVEL_AVX512) {
    GTEST_SKIP() << "CPU SIMD level " << mtl_get_simd_level_name(mtl_get_simd_level())
                 << " < AVX512 required for 422be10_to_yuv420p8 SIMD comparison";
  }
  for (auto [w, h] : {std::pair{kFullW, kFullH}, std::pair{kTail420W, kTail420H}}) {
    const size_t px = static_cast<size_t>(w) * h;
    std::vector<uint8_t> pg10(px * 5 / 2), ref(px * 3 / 2), got(px * 3 / 2);
    fill_bytes(pg10.data(), pg10.size());
    auto* p10 = ptr<st20_rfc4175_422_10_pg2_be>(pg10);
    EXPECT_EQ(0, st20_rfc4175_422be10_to_yuv420p8_simd(p10, ref.data(), ref.data() + px,
                                                       ref.data() + px * 5 / 4, w, h,
                                                       MTL_SIMD_LEVEL_NONE));
    for (auto lvl : {MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_MAX}) {
      if (!simd_level_available(lvl)) continue;
      EXPECT_EQ(
          0, st20_rfc4175_422be10_to_yuv420p8_simd(p10, got.data(), got.data() + px,
                                                   got.data() + px * 5 / 4, w, h, lvl));
      EXPECT_EQ(ref, got);
    }
  }
}

TEST(Cvt, rfc4175_422_10_v210) {
  using Be10 = st20_rfc4175_422_10_pg2_be;
  using Le10 = st20_rfc4175_422_10_pg2_le;
  auto check_via_le = [](int w, int h, enum mtl_simd_level lvl) {
    const bool fail = (w * h % 6) != 0;
    const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
    const size_t v_sz = (static_cast<size_t>(w) * h * 8 + 2) / 3;
    std::vector<uint8_t> src(pg_sz), mid(pg_sz, 0), dst(pg_sz, 0), v(v_sz, 0);

    fill_bytes(src.data(), pg_sz);
    EXPECT_EQ(fail,
              st20_rfc4175_422le10_to_v210_simd(src.data(), v.data(), w, h, lvl) != 0);
    EXPECT_EQ(fail, st20_v210_to_rfc4175_422le10(v.data(), dst.data(), w, h) != 0);
    EXPECT_EQ(fail, src != dst);

    std::fill(dst.begin(), dst.end(), 0);
    EXPECT_EQ(fail, st20_rfc4175_422be10_to_v210_simd(ptr<Be10>(src), v.data(), w, h,
                                                      lvl) != 0);
    EXPECT_EQ(fail, st20_v210_to_rfc4175_422le10(v.data(), mid.data(), w, h) != 0);
    st20_rfc4175_422le10_to_422be10(ptr<Le10>(mid), ptr<Be10>(dst), w, h);
    EXPECT_EQ(fail, src != dst);
  };
  auto check_be_rt = [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
    const bool fail = (w * h % 6) != 0;
    const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
    const size_t v_sz = (static_cast<size_t>(w) * h * 8 + 2) / 3;
    std::vector<uint8_t> be(pg_sz), be2(pg_sz, 0), v(v_sz, 0), v2(v_sz, 0);

    fill_bytes(be.data(), pg_sz);
    EXPECT_EQ(fail,
              st20_rfc4175_422be10_to_v210_simd(ptr<Be10>(be), v.data(), w, h, fl) != 0);
    EXPECT_EQ(fail,
              st20_v210_to_rfc4175_422be10_simd(v.data(), ptr<Be10>(be2), w, h, bl) != 0);
    EXPECT_EQ(fail, be != be2);

    fill_v210(v.data(), v_sz);
    EXPECT_EQ(fail,
              st20_v210_to_rfc4175_422be10_simd(v.data(), ptr<Be10>(be), w, h, fl) != 0);
    EXPECT_EQ(fail,
              st20_rfc4175_422be10_to_v210_simd(ptr<Be10>(be), v2.data(), w, h, bl) != 0);
    EXPECT_EQ(fail, v != v2);
  };

  for (auto lvl : {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512,
                   MTL_SIMD_LEVEL_AVX512_VBMI2, MTL_SIMD_LEVEL_MAX}) {
    if (!simd_level_available(lvl)) continue;
    for (auto [w, h] : {std::pair{kFullW, kFullH}, std::pair{kTailW, kTailH},
                        std::pair{kV210BadW, kV210BadH}}) {
      check_via_le(w, h, lvl);
      check_be_rt(w, h, lvl, lvl);
    }
    if (lvl != MTL_SIMD_LEVEL_NONE && lvl != MTL_SIMD_LEVEL_MAX) {
      check_be_rt(kFullW, kFullH, MTL_SIMD_LEVEL_NONE, lvl);
      check_be_rt(kFullW, kFullH, lvl, MTL_SIMD_LEVEL_NONE);
    }
  }
}

TEST(Cvt, rfc4175_422be10_y210) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        const size_t pg_sz = static_cast<size_t>(w) * h * 5 / 2;
        const size_t n16 = static_cast<size_t>(w) * h * 2;
        std::vector<uint8_t> pg(pg_sz), pg2(pg_sz);
        std::vector<uint16_t> y(n16), y2(n16);
        auto* be = reinterpret_cast<st20_rfc4175_422_10_pg2_be*>(pg.data());
        auto* be2 = reinterpret_cast<st20_rfc4175_422_10_pg2_be*>(pg2.data());

        fill_bytes(pg.data(), pg_sz);
        EXPECT_EQ(0, st20_rfc4175_422be10_to_y210_simd(be, y.data(), w, h, fl));
        EXPECT_EQ(0, st20_y210_to_rfc4175_422be10_simd(y.data(), be2, w, h, bl));
        EXPECT_EQ(pg, pg2);

        fill_u16_masked(y.data(), n16, 0xFFC0u);
        EXPECT_EQ(0, st20_y210_to_rfc4175_422be10_simd(y.data(), be, w, h, fl));
        EXPECT_EQ(0, st20_rfc4175_422be10_to_y210_simd(be, y2.data(), w, h, bl));
        EXPECT_EQ(y, y2);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_MAX});
}

TEST(Cvt, yuv422p16le_rfc4175_422be10) {
  auto run = [](int w, int h, enum mtl_simd_level lvl) {
    const size_t px = static_cast<size_t>(w) * h;
    std::vector<uint8_t> pg(px * 5 / 2);
    std::vector<uint16_t> in(px * 2), out(px * 2);
    uint32_t seed = 0x456789ABu;
    for (auto& v : in)
      v = static_cast<uint16_t>(((xorshift32(seed) & 0x3FFu) << 6) | 0x3Fu);
    auto* be = reinterpret_cast<st20_rfc4175_422_10_pg2_be*>(pg.data());
    EXPECT_EQ(0, st20_yuv422p16le_to_rfc4175_422be10_simd(
                     in.data(), in.data() + px, in.data() + px * 3 / 2, be, w, h, lvl));
    EXPECT_EQ(
        0, st20_rfc4175_422be10_to_yuv422p16le_simd(be, out.data(), out.data() + px,
                                                    out.data() + px * 3 / 2, w, h, lvl));
    for (auto& v : in) v &= static_cast<uint16_t>(~0x3Fu);
    EXPECT_EQ(in, out);
  };
  for (auto lvl : {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_MAX}) {
    if (!simd_level_available(lvl)) continue;
    run(kFullW, kFullH, lvl);
    run(kTailW, kTailH, lvl);
  }
}

TEST(Cvt, rotate_rfc4175_422_10) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level l1, enum mtl_simd_level l3) {
        check_rotations<st20_rfc4175_422_10_pg2_be, st20_rfc4175_422_10_pg2_le, 2>(
            w, h, static_cast<size_t>(w) * h * 5 / 2,
            st20_rfc4175_422be10_to_422le10_simd, st20_rfc4175_422le10_to_yuv422p10le,
            st20_yuv422p10le_to_rfc4175_422be10_simd,
            st20_rfc4175_422be10_to_yuv422p10le_simd, st20_yuv422p10le_to_rfc4175_422le10,
            st20_rfc4175_422le10_to_422be10_simd, l1, l3);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512_VBMI2,
       MTL_SIMD_LEVEL_MAX},
      kTailW, kTailH, false);
}

/* ── 4:2:2 12-bit, 4:4:4 10-bit & 12-bit conversions ─────────────────── */

TEST(Cvt, rfc4175_422_12) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        check_planar16<st20_rfc4175_422_12_pg2_be, 2>(
            w, h, static_cast<size_t>(w) * h * 3, 0xFFFu,
            st20_rfc4175_422be12_to_yuv422p12le_simd,
            st20_yuv422p12le_to_rfc4175_422be12_simd, fl, bl);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_AVX512_VBMI2,
       MTL_SIMD_LEVEL_MAX});
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        check_packed_both<st20_rfc4175_422_12_pg2_be, st20_rfc4175_422_12_pg2_le>(
            w, h, static_cast<size_t>(w) * h * 3, st20_rfc4175_422be12_to_422le12_simd,
            st20_rfc4175_422le12_to_422be12_simd, fl, bl);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_AVX512, MTL_SIMD_LEVEL_MAX});

  const size_t sz = static_cast<size_t>(kFullW) * kFullH * 3;
  check_packed_vs_scalar<st20_rfc4175_422_12_pg2_le, st20_rfc4175_422_12_pg2_be>(
      kFullW, kFullH, sz, st20_rfc4175_422le12_to_422be12_simd, MTL_SIMD_LEVEL_MAX);
  check_planar16<st20_rfc4175_422_12_pg2_le, 2>(kFullW, kFullH, sz, 0xFFFu,
                                                st20_rfc4175_422le12_to_yuv422p12le,
                                                st20_yuv422p12le_to_rfc4175_422le12);
  check_rotations<st20_rfc4175_422_12_pg2_be, st20_rfc4175_422_12_pg2_le, 2>(
      kFullW, kFullH, sz, st20_rfc4175_422be12_to_422le12_simd,
      st20_rfc4175_422le12_to_yuv422p12le, st20_yuv422p12le_to_rfc4175_422be12_simd,
      st20_rfc4175_422be12_to_yuv422p12le_simd, st20_yuv422p12le_to_rfc4175_422le12,
      st20_rfc4175_422le12_to_422be12_simd, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_444_10) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        const size_t sz = static_cast<size_t>(w) * h * 15 / 4;
        check_planar16<st20_rfc4175_444_10_pg4_be, 1>(
            w, h, sz, 0x3FFu, st20_rfc4175_444be10_to_444p10le_simd,
            st20_444p10le_to_rfc4175_444be10_simd, fl, bl);
        check_packed_both<st20_rfc4175_444_10_pg4_be, st20_rfc4175_444_10_pg4_le>(
            w, h, sz, st20_rfc4175_444be10_to_444le10_simd,
            st20_rfc4175_444le10_to_444be10_simd, fl, bl);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_MAX}, kTail444W, kTail444H, false);

  const size_t sz = static_cast<size_t>(kFullW) * kFullH * 15 / 4;
  check_packed_vs_scalar<st20_rfc4175_444_10_pg4_le, st20_rfc4175_444_10_pg4_be>(
      kFullW, kFullH, sz, st20_rfc4175_444le10_to_444be10_simd, MTL_SIMD_LEVEL_MAX);
  check_planar16<st20_rfc4175_444_10_pg4_le, 1>(kFullW, kFullH, sz, 0x3FFu,
                                                st20_rfc4175_444le10_to_yuv444p10le,
                                                st20_yuv444p10le_to_rfc4175_444le10);
  check_planar16<st20_rfc4175_444_10_pg4_le, 1>(kFullW, kFullH, sz, 0x3FFu,
                                                st20_rfc4175_444le10_to_gbrp10le,
                                                st20_gbrp10le_to_rfc4175_444le10);
  check_rotations<st20_rfc4175_444_10_pg4_be, st20_rfc4175_444_10_pg4_le, 1>(
      kFullW, kFullH, sz, st20_rfc4175_444be10_to_444le10_simd,
      st20_rfc4175_444le10_to_yuv444p10le, st20_444p10le_to_rfc4175_444be10_simd,
      st20_rfc4175_444be10_to_444p10le_simd, st20_444p10le_to_rfc4175_444le10,
      st20_rfc4175_444le10_to_444be10_simd, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

TEST(Cvt, rfc4175_444_12) {
  sweep_simd_rt(
      [](int w, int h, enum mtl_simd_level fl, enum mtl_simd_level bl) {
        const size_t sz = static_cast<size_t>(w) * h * 9 / 2;
        check_planar16<st20_rfc4175_444_12_pg2_be, 1>(
            w, h, sz, 0xFFFu, st20_rfc4175_444be12_to_444p12le_simd,
            st20_444p12le_to_rfc4175_444be12_simd, fl, bl);
        check_packed_both<st20_rfc4175_444_12_pg2_be, st20_rfc4175_444_12_pg2_le>(
            w, h, sz, st20_rfc4175_444be12_to_444le12_simd,
            st20_rfc4175_444le12_to_444be12_simd, fl, bl);
      },
      {MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_MAX}, kTailW, kTailH, false);

  const size_t sz = static_cast<size_t>(kFullW) * kFullH * 9 / 2;
  check_packed_vs_scalar<st20_rfc4175_444_12_pg2_le, st20_rfc4175_444_12_pg2_be>(
      kFullW, kFullH, sz, st20_rfc4175_444le12_to_444be12_simd, MTL_SIMD_LEVEL_MAX);
  check_planar16<st20_rfc4175_444_12_pg2_le, 1>(kFullW, kFullH, sz, 0xFFFu,
                                                st20_rfc4175_444le12_to_yuv444p12le,
                                                st20_yuv444p12le_to_rfc4175_444le12);
  check_planar16<st20_rfc4175_444_12_pg2_le, 1>(kFullW, kFullH, sz, 0xFFFu,
                                                st20_rfc4175_444le12_to_gbrp12le,
                                                st20_gbrp12le_to_rfc4175_444le12);
  check_rotations<st20_rfc4175_444_12_pg2_be, st20_rfc4175_444_12_pg2_le, 1>(
      kFullW, kFullH, sz, st20_rfc4175_444be12_to_444le12_simd,
      st20_rfc4175_444le12_to_yuv444p12le, st20_444p12le_to_rfc4175_444be12_simd,
      st20_rfc4175_444be12_to_444p12le_simd, st20_444p12le_to_rfc4175_444le12,
      st20_rfc4175_444le12_to_444be12_simd, MTL_SIMD_LEVEL_NONE, MTL_SIMD_LEVEL_NONE);
}

/* ── ST 2110-31 AM824 <-> AES3 ───────────────────────────────────────── */

TEST(Cvt, st31_am824_aes3_roundtrip) {
  for (int blocks : {1, 10, 100}) {
    const int subframes = blocks * 2 * 192;
    const size_t bytes = static_cast<size_t>(subframes) * 4;
    std::vector<struct st31_am824> am1(subframes), am2(subframes);
    std::vector<struct st31_aes3> aes1(subframes), aes2(subframes);

    fill_bytes(reinterpret_cast<uint8_t*>(am1.data()), bytes);
    for (int i = 0; i < subframes; ++i) {
      am1[i].unused = 0;
      am1[i].b = (i % (192 * 2) == 0) ? 1 : 0;
      am1[i].f = (i % 2 == 0) ? 1 : 0;
    }
    EXPECT_EQ(0, st31_am824_to_aes3(am1.data(), aes1.data(), subframes));
    EXPECT_EQ(0, st31_aes3_to_am824(aes1.data(), am2.data(), subframes));
    EXPECT_EQ(0, std::memcmp(am1.data(), am2.data(), bytes));

    fill_bytes(reinterpret_cast<uint8_t*>(aes1.data()), bytes, 0x6789ABCDu);
    for (int i = 0; i < subframes; ++i) {
      aes1[i].preamble = (i % (192 * 2) == 0) ? 0x2 : ((i % 2 == 0) ? 0x0 : 0x1);
    }
    EXPECT_EQ(0, st31_aes3_to_am824(aes1.data(), am1.data(), subframes));
    EXPECT_EQ(0, st31_am824_to_aes3(am1.data(), aes2.data(), subframes));
    EXPECT_EQ(0, std::memcmp(aes1.data(), aes2.data(), bytes));
  }
}

/* ── st_frame_convert & field split/merge ────────────────────────────── */

TEST(Cvt, st_frame_convert_fail_resolution) {
  for (auto [dw, dh] : {std::pair{1920u, 1088u}, {1280u, 1080u}, {3840u, 2160u}}) {
    struct st_frame src = {}, dst = {}, back = {};
    src.fmt = back.fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    dst.fmt = ST_FRAME_FMT_Y210;
    src.width = back.width = 1920;
    src.height = back.height = 1080;
    dst.width = dw;
    dst.height = dh;
    EXPECT_NE(0, st_frame_convert(&src, &dst));
    EXPECT_NE(0, st_frame_convert(&dst, &back));
  }
}

TEST(Cvt, st_frame_convert_fail_fmt) {
  const std::pair<enum st_frame_fmt, enum st_frame_fmt> cases[] = {
      {ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST_FRAME_FMT_YUV444PLANAR10LE},
      {ST_FRAME_FMT_Y210, ST_FRAME_FMT_V210},
      {ST_FRAME_FMT_GBRPLANAR10LE, ST_FRAME_FMT_YUV420CUSTOM8},
  };
  for (auto [sf, df] : cases) {
    struct st_frame src = {}, dst = {}, back = {};
    src.width = dst.width = back.width = kFullW;
    src.height = dst.height = back.height = kFullH;
    src.fmt = back.fmt = sf;
    dst.fmt = df;
    EXPECT_NE(0, st_frame_convert(&src, &dst));
    EXPECT_NE(0, st_frame_convert(&dst, &back));
  }
}

TEST(Cvt, st_frame_convert_rotate) {
  constexpr auto kBe10 = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  constexpr auto kP10 = ST_FRAME_FMT_YUV422PLANAR10LE;
  constexpr auto kY210 = ST_FRAME_FMT_Y210;
  constexpr auto kV210 = ST_FRAME_FMT_V210;
  const struct {
    uint32_t w, h;
    enum st_frame_fmt sf, df;
    bool ps, pd, pb;
  } cases[] = {
      {192, 64, kBe10, kY210, false, false, false},
      {198, 64, kV210, kBe10, false, false, false},
      {192, 64, kBe10, kP10, false, false, false},
      {192, 64, kBe10, kP10, true, true, true},
      {198, 64, kY210, kBe10, true, true, true},
      {192, 64, kBe10, kY210, true, true, true},
      {192, 64, kBe10, kP10, false, true, true},
      {198, 64, kY210, kBe10, true, false, true},
      {192, 64, kBe10, kY210, true, true, false},
  };
  uint32_t seed = 1;
  for (const auto& c : cases) {
    TestFrame src, dst, back;
    src.init(c.sf, c.w, c.h, true, c.ps, seed++);
    dst.init(c.df, c.w, c.h, false, c.pd);
    back.init(c.sf, c.w, c.h, false, c.pb);
    EXPECT_EQ(0, st_frame_convert(&src.f, &dst.f));
    EXPECT_EQ(0, st_frame_convert(&dst.f, &back.f));
    expect_frames_equal_lines(src.f, back.f);
  }
}

TEST(Cvt, field_to_frame) {
  auto alloc = [](enum st_frame_fmt fmt, bool interlaced) {
    return ScopedFrame(st_frame_create_by_malloc(fmt, kFullW, kFullH, interlaced),
                       &st_frame_free);
  };
  for (auto fmt : {ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST_FRAME_FMT_YUV422PLANAR10LE}) {
    auto frame = alloc(fmt, false), first = alloc(fmt, true);
    auto second = alloc(fmt, true), back = alloc(fmt, false);
    ASSERT_TRUE(frame && first && second && back);
    second->second_field = true;
    fill_bytes(static_cast<uint8_t*>(frame->addr[0]), frame->buffer_size);
    EXPECT_GE(st_field_split(frame.get(), first.get(), second.get()), 0);
    EXPECT_GE(st_field_merge(first.get(), second.get(), back.get()), 0);
    EXPECT_EQ(0, std::memcmp(frame->addr[0], back->addr[0], frame->buffer_size));
  }
}
