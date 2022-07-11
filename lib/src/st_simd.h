/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_SIMD_H_
#define _ST_LIB_SIMD_H_

#define STRINGIFY(a) #a

// Push the SIMD compiler flag online to build the code path
#if defined(__clang__)
#define ST_TARGET_CODE_START(Target) \
  _Pragma(STRINGIFY(                 \
      clang attribute push(__attribute__((target(Target))), apply_to = function)))
#define ST_TARGET_CODE_STOP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define ST_TARGET_CODE_START(Target) \
  _Pragma("GCC push_options") _Pragma(STRINGIFY(GCC target(Target)))
#define ST_TARGET_CODE_STOP _Pragma("GCC pop_options")
#else
// MSVS can use intrinsic without appending the SIMD flag
#define ST_TARGET_CODE_START(Target)
#define ST_TARGET_CODE_STOP
#endif

#define ST_TARGET_CODE_START_SSE4_2 ST_TARGET_CODE_START("sse4.2")

#ifdef ST_HAS_AVX2
#define ST_TARGET_CODE_START_AVX2 ST_TARGET_CODE_START("avx2")
#endif

#ifdef ST_HAS_AVX512
#define ST_TARGET_CODE_START_AVX512 \
  ST_TARGET_CODE_START("avx512f,avx512cd,avx512vl,avx512dq,avx512bw")
#endif

#ifdef ST_HAS_AVX512_VBMI2
#define ST_TARGET_CODE_START_AVX512_VBMI2 \
  ST_TARGET_CODE_START(                   \
      "avx512f,avx512cd,avx512vl,avx512dq,avx512bw,avx512vbmi,avx512vbmi2")
#endif

#endif
