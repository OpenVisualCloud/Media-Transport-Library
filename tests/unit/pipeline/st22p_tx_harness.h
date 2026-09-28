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

/**
 * Turn on ST22P_TX_FLAG_BLOCK_GET behaviour for the app side: init the block
 * cond/mutex and set the wait timeout of ut22p_tx_get_frame(). Call before
 * any ut22p_tx_get_frame().
 */
void ut22p_tx_ctx_enable_blocking(ut22p_tx_ctx* ctx, uint64_t timeout_ns);

/**
 * Turn on ST22_ENCODER_RESP_FLAG_BLOCK_GET behaviour for the encoder-plugin
 * side: init the encode block cond/mutex and set its wait timeout. Call before
 * any ut22p_tx_encode_get_frame().
 */
void ut22p_tx_ctx_enable_encode_blocking(ut22p_tx_ctx* ctx, uint64_t timeout_ns);

/** Wake a blocking encode_get_frame sleeper, via the callback the encoder
 * device is registered with (wraps tx_st22p_encode_wake_block). */
void ut22p_tx_encode_wake_block(ut22p_tx_ctx* ctx);

/* encoder-plugin side: claim READY->IN_ENCODING. Returns 0 when a frame was
 * handed back, -EBUSY when none was. */
int ut22p_tx_encode_get_frame(ut22p_tx_ctx* ctx);

/** Set the mock PTP wall-clock time returned by mt_get_ptp_time(). */
void ut22p_tx_set_ptp_ns(ut22p_tx_ctx* ctx, uint64_t ns);

/** OR the given bits into ops.flags (e.g. ST22P_TX_FLAG_DROP_WHEN_LATE). */
void ut22p_tx_set_flags(ut22p_tx_ctx* ctx, uint32_t flags);

/** Set ops.fps and the cached frame period tx_st22p_if_frame_late() reads. */
void ut22p_tx_set_fps(ut22p_tx_ctx* ctx, enum st_fps fps);

/** Cache the period of cached_fps as create() does, then point ops.fps at
 * ops_fps. One call, so the two writes cannot be ordered the other way. */
void ut22p_tx_set_fps_mismatch(ut22p_tx_ctx* ctx, enum st_fps cached_fps,
                               enum st_fps ops_fps);

/** Register ops.notify_frame_done. */
void ut22p_tx_set_notify_frame_done(ut22p_tx_ctx* ctx,
                                    int (*cb)(void* priv, struct st_frame* frame),
                                    void* priv);

/** Register ops.notify_frame_late. */
void ut22p_tx_set_notify_frame_late(ut22p_tx_ctx* ctx,
                                    int (*cb)(void* priv, uint64_t epoch_skipped),
                                    void* priv);

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
