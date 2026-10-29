/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "simd_scalar_harness.h"

#include <st_convert_api.h>
#include <st_pipeline_api.h>

#include "st2110/st_avx2.h"
#include "st2110/st_avx512.h"
#include "st2110/st_avx512_vbmi.h"

#define UT_PG2_BYTES(pg, w, h) ((size_t)(w) * (h) / 2 * sizeof(struct pg))
#define UT_FRAME_BYTES(fmt, w, h) st_frame_size(ST_FRAME_FMT_##fmt, w, h, false)
#define UT_FMT(name, size, mask)                       \
  static size_t name##_bytes(uint32_t w, uint32_t h) { \
    return size;                                       \
  }                                                    \
  static const struct ut_cvt_fmt name __rte_unused = {name##_bytes, mask};
#define UT_ALL_BITS (~0ull)
#define UT_EACH_U16(bits) (0x0001000100010001ull * (bits))
#define UT_EACH_U32(bits) (0x0000000100000001ull * (bits))
UT_FMT(be10, UT_FRAME_BYTES(YUV422RFC4175PG2BE10, w, h), UT_ALL_BITS)
UT_FMT(le10, UT_PG2_BYTES(st20_rfc4175_422_10_pg2_le, w, h), UT_ALL_BITS)
UT_FMT(le8, UT_PG2_BYTES(st20_rfc4175_422_8_pg2_le, w, h), UT_ALL_BITS)
UT_FMT(be12, UT_FRAME_BYTES(YUV422RFC4175PG2BE12, w, h), UT_ALL_BITS)
UT_FMT(le12, UT_PG2_BYTES(st20_rfc4175_422_12_pg2_le, w, h), UT_ALL_BITS)
UT_FMT(v210, UT_FRAME_BYTES(V210, w, h), UT_EACH_U32(0x3fffffff))
UT_FMT(y210, UT_FRAME_BYTES(Y210, w, h), UT_EACH_U16(0xffc0))
UT_FMT(p10, UT_FRAME_BYTES(YUV422PLANAR10LE, w, h), UT_EACH_U16(0x03ff))
UT_FMT(p12, UT_FRAME_BYTES(YUV422PLANAR12LE, w, h), UT_EACH_U16(0x0fff))
UT_FMT(p16, UT_FRAME_BYTES(YUV422PLANAR16LE, w, h), UT_ALL_BITS)
UT_FMT(p8, UT_FRAME_BYTES(YUV422PLANAR8, w, h), UT_ALL_BITS)
UT_FMT(p420, UT_FRAME_BYTES(YUV420PLANAR8, w, h), UT_ALL_BITS)

#define UT_PX(w, h) ((size_t)(w) * (h))
#define UT_PLANES(p, T, chroma_div) \
  (T*)(p), (T*)(p) + UT_PX(w, h),   \
      (T*)(p) + ((chroma_div) + 1) * UT_PX(w, h) / (chroma_div)
#define UT_PACKED s, d
#define UT_TO_U16P s, UT_PLANES(d, uint16_t, 2)
#define UT_TO_U8P s, UT_PLANES(d, uint8_t, 2)
#define UT_TO_U8P420 s, UT_PLANES(d, uint8_t, 4)
#define UT_FROM_U16P UT_PLANES(s, uint16_t, 2), d

#define UT_DEF_WRAP(lvl, conv, sfx, wa, ha, in, out, ...)                          \
  static int st20_##conv##_##sfx##_ut(void* s, void* d, uint32_t w, uint32_t h) {  \
    return st20_##conv##_##sfx(__VA_ARGS__, w, h);                                 \
  }                                                                                \
  static int st20_##conv##_##sfx##_ref(void* s, void* d, uint32_t w, uint32_t h) { \
    return st20_##conv##_simd(__VA_ARGS__, w, h, MTL_SIMD_LEVEL_NONE);             \
  }
#define UT_DEF_ROW(lvl, conv, sfx, wa, ha, in, out, ...)                  \
  {"st20_" #conv "_" #sfx,   MTL_SIMD_LEVEL_##lvl,     wa, ha, &in, &out, \
   st20_##conv##_##sfx##_ut, st20_##conv##_##sfx##_ref},

#ifdef MTL_HAS_AVX2
#define UT_AVX2_ROWS(X, L)                                            \
  X(L, rfc4175_422be10_to_422le10, avx2, 2, 1, be10, le10, UT_PACKED) \
  X(L, rfc4175_422le10_to_422be10, avx2, 2, 1, le10, be10, UT_PACKED)
#else
#define UT_AVX2_ROWS(X, L)
#endif
#ifdef MTL_HAS_AVX512
#define UT_AVX512_ROWS(X, L)                                                  \
  X(L, rfc4175_422be10_to_yuv422p10le, avx512, 2, 1, be10, p10, UT_TO_U16P)   \
  X(L, rfc4175_422be10_to_422le10, avx512, 2, 1, be10, le10, UT_PACKED)       \
  X(L, rfc4175_422be10_to_422le8, avx512, 2, 1, be10, le8, UT_PACKED)         \
  X(L, rfc4175_422le10_to_v210, avx512, 6, 1, le10, v210, UT_PACKED)          \
  X(L, rfc4175_422be10_to_v210, avx512, 6, 1, be10, v210, UT_PACKED)          \
  X(L, yuv422p10le_to_rfc4175_422be10, avx512, 2, 1, p10, be10, UT_FROM_U16P) \
  X(L, rfc4175_422le10_to_422be10, avx512, 2, 1, le10, be10, UT_PACKED)       \
  X(L, v210_to_rfc4175_422be10, avx512, 6, 1, v210, be10, UT_PACKED)          \
  X(L, rfc4175_422be10_to_y210, avx512, 2, 1, be10, y210, UT_PACKED)          \
  X(L, y210_to_rfc4175_422be10, avx512, 2, 1, y210, be10, UT_PACKED)          \
  X(L, rfc4175_422be12_to_422le12, avx512, 2, 1, be12, le12, UT_PACKED)       \
  X(L, rfc4175_422be12_to_yuv422p12le, avx512, 2, 1, be12, p12, UT_TO_U16P)   \
  X(L, rfc4175_422be10_to_yuv422p8, avx512, 2, 1, be10, p8, UT_TO_U8P)        \
  X(L, rfc4175_422be10_to_yuv420p8, avx512, 2, 2, be10, p420, UT_TO_U8P420)   \
  X(L, rfc4175_422be10_to_yuv422p16le, avx512, 2, 1, be10, p16, UT_TO_U16P)   \
  X(L, yuv422p16le_to_rfc4175_422be10, avx512, 2, 1, p16, be10, UT_FROM_U16P)
#else
#define UT_AVX512_ROWS(X, L)
#endif
#ifdef MTL_HAS_AVX512_VBMI2
/* the VBMI v210 routines return -EINVAL unless the pixel count is a multiple of 24 */
#define UT_VBMI_ROWS(X, L)                                                       \
  X(L, rfc4175_422be10_to_yuv422p10le, avx512_vbmi, 2, 1, be10, p10, UT_TO_U16P) \
  X(L, rfc4175_422be10_to_422le10, avx512_vbmi, 2, 1, be10, le10, UT_PACKED)     \
  X(L, rfc4175_422be10_to_422le8, avx512_vbmi, 2, 1, be10, le8, UT_PACKED)       \
  X(L, rfc4175_422le10_to_v210, avx512_vbmi, 24, 1, le10, v210, UT_PACKED)       \
  X(L, rfc4175_422be10_to_v210, avx512_vbmi, 24, 1, be10, v210, UT_PACKED)       \
  X(L, yuv422p10le_to_rfc4175_422be10, vbmi, 2, 1, p10, be10, UT_FROM_U16P)      \
  X(L, rfc4175_422le10_to_422be10, vbmi, 2, 1, le10, be10, UT_PACKED)            \
  X(L, v210_to_rfc4175_422be10, avx512_vbmi, 24, 1, v210, be10, UT_PACKED)       \
  X(L, rfc4175_422be12_to_yuv422p12le, avx512_vbmi, 2, 1, be12, p12, UT_TO_U16P)
#else
#define UT_VBMI_ROWS(X, L)
#endif

#define UT_ROWS(X) \
  UT_AVX2_ROWS(X, AVX2) UT_AVX512_ROWS(X, AVX512) UT_VBMI_ROWS(X, AVX512_VBMI2)

UT_ROWS(UT_DEF_WRAP)

/* the empty last row keeps the table valid C when no SIMD level is built */
const struct ut_cvt_row ut_cvt_rows[] = {UT_ROWS(UT_DEF_ROW){NULL}};
const size_t ut_cvt_rows_count = MTL_ARRAY_SIZE(ut_cvt_rows) - 1;
