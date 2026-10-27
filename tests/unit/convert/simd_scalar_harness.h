/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for simd_scalar_test.cpp: st_avx*.h include st_main.h, which is C only.
 */

#ifndef TESTS_UNIT_CONVERT_SIMD_SCALAR_HARNESS_H
#define TESTS_UNIT_CONVERT_SIMD_SCALAR_HARNESS_H

#include <mtl_api.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ut_cvt_fmt {
  size_t (*bytes)(uint32_t w, uint32_t h);
  uint64_t mask; /* the sample bits of every little-endian 64-bit word */
};

/* A per-level routine and its scalar reference; planar planes are contiguous. */
struct ut_cvt_row {
  const char* name;
  enum mtl_simd_level level;
  uint32_t w_align, h_align;
  const struct ut_cvt_fmt *src, *dst;
  int (*simd)(void* src, void* dst, uint32_t w, uint32_t h);
  int (*scalar)(void* src, void* dst, uint32_t w, uint32_t h);
};

/* One row per per-level routine libmtl was built with. */
extern const struct ut_cvt_row ut_cvt_rows[];
extern const size_t ut_cvt_rows_count;

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_CONVERT_SIMD_SCALAR_HARNESS_H */
