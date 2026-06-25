/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST22p (compressed video) RX pipeline-layer concurrency unit
 * tests.
 *
 * Exposes the two role-halves of the lock-free RX framebuffer ring so a
 * gtest can drive them from independent threads:
 *   - producer (transport): inject_frame (FREE->DECODED via frame_ready)
 *   - consumer (app):        get_frame (DECODED->IN_USER) / put_frame (->FREE)
 *
 * The ctx is hand-initialised in the derive (no-decoder) path so frame_ready
 * advances a frame straight to DECODED, the state get_frame consumes. This
 * isolates the claim/lifecycle state machine so the test can hammer it with
 * many threads and assert single-ownership + conservation + deadlock-freedom.
 */

#ifndef _ST22P_RX_PIPELINE_HARNESS_H_
#define _ST22P_RX_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut22p_ctx ut22p_ctx;

int ut22p_init(void);

ut22p_ctx* ut22p_ctx_create(int framebuff_cnt);
void ut22p_ctx_destroy(ut22p_ctx* ctx);

int ut22p_framebuff_cnt(const ut22p_ctx* ctx);

/* producer (transport) side: drive one frame to DECODED. Returns 0 on success,
 * -EBUSY when no FREE framebuffer is available. */
int ut22p_inject_frame(ut22p_ctx* ctx, enum st_frame_status status, uint32_t timestamp);

/* consumer (app) side */
struct st_frame* ut22p_get_frame(ut22p_ctx* ctx);
int ut22p_put_frame(ut22p_ctx* ctx, struct st_frame* frame);

/* Buffer index that a user-facing frame belongs to. */
int ut22p_frame_idx(const struct st_frame* frame);

/* Raw stat value of framebuffer i (for diagnostics). */
int ut22p_frame_stat(const ut22p_ctx* ctx, int i);

#ifdef __cplusplus
}
#endif

#endif /* _ST22P_RX_PIPELINE_HARNESS_H_ */
