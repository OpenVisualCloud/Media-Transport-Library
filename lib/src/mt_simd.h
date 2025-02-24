/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SIMD_H_
#define _MT_LIB_SIMD_H_

#define STRINGIFY(a) #a

// Push the SIMD compiler flag online to build the code path
#if defined(__clang__)
/* "clang attribute push" support from clang 9.0 */
#define MT_TARGET_CODE_START(Target) \
  _Pragma(STRINGIFY(                 \
      clang attribute push(__attribute__((target(Target))), apply_to = function)))
#define MT_TARGET_CODE_STOP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define MT_TARGET_CODE_START(Target) \
  _Pragma("GCC push_options") _Pragma(STRINGIFY(GCC target(Target)))
#define MT_TARGET_CODE_STOP _Pragma("GCC pop_options")
#else
// MSVC can use intrinsic without appending the SIMD flag
#define MT_TARGET_CODE_START(Target)
#define MT_TARGET_CODE_STOP
#endif

#define MT_TARGET_CODE_START_SSE4_2 MT_TARGET_CODE_START("sse4.2")

#ifdef MTL_HAS_AVX2
#define MT_TARGET_CODE_START_AVX2 MT_TARGET_CODE_START("avx2")
#endif

#ifdef MTL_HAS_AVX512
#define MT_TARGET_CODE_START_AVX512 \
  MT_TARGET_CODE_START("avx512f,avx512cd,avx512vl,avx512dq,avx512bw")
#endif

#ifdef MTL_HAS_AVX512_VBMI2
#define MT_TARGET_CODE_START_AVX512_VBMI2 \
  MT_TARGET_CODE_START(                   \
      "avx512f,avx512cd,avx512vl,avx512dq,avx512bw,avx512vbmi,avx512vbmi2")
#endif

#endif
