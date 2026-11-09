/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the st_frame_convert_nt() unit tests. Plain C declarations only, so
 * the header is safe to include from C++; st_convert.h cannot be compiled by g++ because
 * struct st_frame_nt_scratch uses _Atomic.
 */

#ifndef TESTS_UNIT_PIPELINE_ST_CONVERT_HARNESS_H
#define TESTS_UNIT_PIPELINE_ST_CONVERT_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_nt_ctx ut_nt_ctx;
typedef int (*ut_nt_convert_func)(struct st_frame* src, struct st_frame* dst);

/** The src_fmt to dst_fmt converter and a zeroed scratch, as a session ctx holds them;
 *  NULL if there is no such converter. */
ut_nt_ctx* ut_nt_create(enum st_frame_fmt src_fmt, enum st_frame_fmt dst_fmt);
/** st_frame_nt_scratch_uinit(), then frees ctx. */
void ut_nt_destroy(ut_nt_ctx* ctx);

/** Replace the converter's convert_func; returns the one it replaced. */
ut_nt_convert_func ut_nt_set_convert_func(ut_nt_ctx* ctx, ut_nt_convert_func func);

/** st_frame_nt_scratch_init() on the EAL socket, with the converter or with none. */
int ut_nt_scratch_init(ut_nt_ctx* ctx, bool use_converter, enum st_frame_fmt dst_fmt,
                       uint32_t width);
/** st_frame_nt_scratch_uinit(). */
void ut_nt_scratch_uinit(ut_nt_ctx* ctx);
void* ut_nt_scratch_buf(const ut_nt_ctx* ctx);
size_t ut_nt_scratch_size(const ut_nt_ctx* ctx);
/** Hold or release the scratch as another app thread converting would. */
void ut_nt_scratch_hold(ut_nt_ctx* ctx, bool hold);
bool ut_nt_scratch_held(const ut_nt_ctx* ctx);

/** st_frame_nt_scratch_size(). */
size_t ut_nt_block_scratch_size(enum st_frame_fmt fmt, uint32_t width);

/** st_frame_convert_nt() with the converter and the scratch. */
int ut_nt_convert(ut_nt_ctx* ctx, struct st_frame* src, struct st_frame* dst);
/** st_frame_convert_nt() with the converter and a free scratch of the caller's size
 *  bytes at buf, or with no scratch for a NULL buf. */
int ut_nt_convert_buf(ut_nt_ctx* ctx, void* buf, size_t size, struct st_frame* src,
                      struct st_frame* dst);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_PIPELINE_ST_CONVERT_HARNESS_H */
