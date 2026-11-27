/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "pipeline/st_convert_harness.h"

#include <errno.h>
#include <rte_lcore.h>
#include <stdatomic.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "st2110/st_avx512.h"

static int dispatched;
static enum mtl_simd_level cpu_simd_level = MTL_SIMD_LEVEL_MAX;

static enum mtl_simd_level ut_cvt_cpu_simd_level(void) {
  return cpu_simd_level == MTL_SIMD_LEVEL_MAX ? mtl_get_simd_level() : cpu_simd_level;
}

#ifdef MTL_HAS_AVX512
/* unused only once a dispatcher stops calling its kernel, which the test reports */
static __attribute__((unused)) int ut_cvt_count_yuv422p12le_to_be12(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_422_12_pg2_be* pg,
    uint32_t w, uint32_t h) {
  dispatched++;
  return st20_yuv422p12le_to_rfc4175_422be12_avx512(y, b, r, pg, w, h);
}

/* the converter dispatch below calls these kernels through the counters above */
#define st20_yuv422p12le_to_rfc4175_422be12_avx512 ut_cvt_count_yuv422p12le_to_be12
#endif

#define mtl_get_simd_level ut_cvt_cpu_simd_level

#pragma GCC diagnostic push
#if defined(__GNUC__) && !defined(__clang__)
/* a false positive on st_frame_convert(): it sets the converter whenever it returns 0 */
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "st2110/st_convert.c"
#pragma GCC diagnostic pop

#undef mtl_get_simd_level

#ifdef MTL_HAS_AVX512
#undef st20_yuv422p12le_to_rfc4175_422be12_avx512
#endif

struct ut_nt_ctx {
  struct st_frame_converter converter;
  struct st_frame_nt_scratch scratch;
};

ut_nt_ctx* ut_nt_create(enum st_frame_fmt src_fmt, enum st_frame_fmt dst_fmt) {
  if (ut_eal_init() < 0) return NULL;

  ut_nt_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  if (st_frame_get_converter(src_fmt, dst_fmt, &ctx->converter) < 0) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

void ut_nt_destroy(ut_nt_ctx* ctx) {
  if (!ctx) return;
  st_frame_nt_scratch_uinit(&ctx->scratch);
  free(ctx);
}

ut_nt_convert_func ut_nt_set_convert_func(ut_nt_ctx* ctx, ut_nt_convert_func func) {
  ut_nt_convert_func old = ctx->converter.convert_func;

  ctx->converter.convert_func = func;
  return old;
}

int ut_nt_scratch_init(ut_nt_ctx* ctx, bool use_converter, enum st_frame_fmt dst_fmt,
                       uint32_t width) {
  return st_frame_nt_scratch_init(&ctx->scratch, use_converter ? &ctx->converter : NULL,
                                  dst_fmt, width, (int)rte_socket_id());
}

void ut_nt_scratch_uinit(ut_nt_ctx* ctx) {
  st_frame_nt_scratch_uinit(&ctx->scratch);
}

void* ut_nt_scratch_buf(const ut_nt_ctx* ctx) {
  return ctx->scratch.buf;
}

size_t ut_nt_scratch_size(const ut_nt_ctx* ctx) {
  return ctx->scratch.size;
}

void ut_nt_scratch_hold(ut_nt_ctx* ctx, bool hold) {
  atomic_store(&ctx->scratch.busy, hold ? 1 : 0);
}

bool ut_nt_scratch_held(const ut_nt_ctx* ctx) {
  return atomic_load(&ctx->scratch.busy) != 0;
}

size_t ut_nt_block_scratch_size(enum st_frame_fmt fmt, uint32_t width) {
  return st_frame_nt_scratch_size(fmt, width);
}

int ut_nt_convert(ut_nt_ctx* ctx, struct st_frame* src, struct st_frame* dst) {
  return st_frame_convert_nt(&ctx->converter, &ctx->scratch, src, dst);
}

int ut_nt_convert_buf(ut_nt_ctx* ctx, void* buf, size_t size, struct st_frame* src,
                      struct st_frame* dst) {
  struct st_frame_nt_scratch scratch = {.buf = buf, .size = size};

  return st_frame_convert_nt(&ctx->converter, buf ? &scratch : NULL, src, dst);
}

int ut_cvt_avx512(enum ut_cvt_kernel kernel, uint16_t* planes[3], void* pg, uint32_t w,
                  uint32_t h) {
#ifdef MTL_HAS_AVX512
  uint16_t* p0 = planes[0];
  uint16_t* p1 = planes[1];
  uint16_t* p2 = planes[2];

  switch (kernel) {
    case UT_CVT_YUV422P12LE_TO_BE12:
      return st20_yuv422p12le_to_rfc4175_422be12_avx512(p0, p1, p2, pg, w, h);
  }
  return -EINVAL;
#else
  MTL_MAY_UNUSED(kernel);
  MTL_MAY_UNUSED(planes);
  MTL_MAY_UNUSED(pg);
  MTL_MAY_UNUSED(w);
  MTL_MAY_UNUSED(h);
  return -ENOTSUP;
#endif
}

int ut_cvt_avx512_dispatched(void) {
  int calls = dispatched;

  dispatched = 0;
  return calls;
}

void ut_cvt_set_cpu_simd_level(enum mtl_simd_level level) {
  cpu_simd_level = level;
}
