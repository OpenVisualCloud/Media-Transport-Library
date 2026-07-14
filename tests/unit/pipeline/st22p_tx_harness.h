/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST22p (compressed video) TX pipeline-layer concurrency unit
 * tests.
 *
 * Exposes the two role-halves of the lock-free TX framebuffer ring so a
 * gtest can drive them from independent threads:
 *   - producer (app):       get_frame (FREE->IN_USER) / put_frame (->ENCODED)
 *   - consumer (transport):  next_frame (ENCODED->IN_TRANSMITTING) /
 *                            frame_done (->FREE)
 *
 * The ctx is hand-initialised in the derive (no-encoder) path so put_frame
 * advances a frame straight to ENCODED, the state next_frame consumes. This
 * isolates the claim/lifecycle state machine so the test can hammer it with
 * many threads and assert single-ownership + conservation + deadlock-freedom.
 */

#ifndef _ST22P_TX_PIPELINE_HARNESS_H_
#define _ST22P_TX_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut22p_tx_ctx ut22p_tx_ctx;

int ut22p_tx_init(void);

ut22p_tx_ctx* ut22p_tx_ctx_create(int framebuff_cnt);
void ut22p_tx_ctx_destroy(ut22p_tx_ctx* ctx);

int ut22p_tx_framebuff_cnt(const ut22p_tx_ctx* ctx);

/* producer (app) side */
struct st_frame* ut22p_tx_get_frame(ut22p_tx_ctx* ctx);
int ut22p_tx_put_frame(ut22p_tx_ctx* ctx, struct st_frame* frame);

/* consumer (transport) side: returns 0 and sets *idx on success, -EBUSY when
 * no ENCODED frame is pending. */
int ut22p_tx_next_frame(ut22p_tx_ctx* ctx, uint16_t* idx);
int ut22p_tx_frame_done(ut22p_tx_ctx* ctx, uint16_t idx);

/* Buffer index that a user-facing frame belongs to. */
int ut22p_tx_frame_idx(const struct st_frame* frame);

/* 1 if every framebuffer is back in the FREE state (no leak). */
int ut22p_tx_all_free(const ut22p_tx_ctx* ctx);

/* Raw stat value of framebuffer i (for diagnostics). */
int ut22p_tx_frame_stat(const ut22p_tx_ctx* ctx, int i);

#ifdef __cplusplus
}
#endif

#endif /* _ST22P_TX_PIPELINE_HARNESS_H_ */
