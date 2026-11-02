/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "pipeline/st_convert_harness.h"

#include <rte_lcore.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "common/ut_common.h"
#include "st2110/st_convert.h"

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
