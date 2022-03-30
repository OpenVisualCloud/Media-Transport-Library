/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
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
