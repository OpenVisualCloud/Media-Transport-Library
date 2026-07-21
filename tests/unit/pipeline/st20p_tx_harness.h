/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST20p (video) TX pipeline-layer concurrency unit tests.
 *
 * Exposes the two role-halves of the lock-free TX framebuffer ring so a
 * gtest can drive them from independent threads:
 *   - producer (app):       get_frame (FREE->IN_USER) / put_frame (->CONVERTED)
 *   - consumer (transport):  next_frame (CONVERTED->IN_TRANSMITTING) /
 *                            frame_done (->FREE)
 *
 * The ctx is hand-initialised in the derive (no-conversion) path so put_frame
 * advances a frame straight to CONVERTED, the state next_frame consumes. This
 * isolates the claim/lifecycle state machine so the test can hammer it with
 * many threads and assert single-ownership + conservation + deadlock-freedom.
 */

#ifndef _ST20P_TX_PIPELINE_HARNESS_H_
#define _ST20P_TX_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20p_tx_ctx ut20p_tx_ctx;

int ut20p_tx_init(void);

ut20p_tx_ctx* ut20p_tx_ctx_create(int framebuff_cnt);
void ut20p_tx_ctx_destroy(ut20p_tx_ctx* ctx);

/**
 * Turn on ST20P_TX_FLAG_BLOCK_GET behaviour: init the block cond/mutex, arm the
 * wake_on_destroy hook, and set the blocking get_frame timeout. Call before any
 * blocking get_frame. Mirrors the block setup st20p_tx_create() performs.
 */
void ut20p_tx_ctx_enable_blocking(ut20p_tx_ctx* ctx, uint64_t timeout_ns);

/** Wake a blocking get_frame sleeper (wraps st20p_tx_wake_block). */
void ut20p_tx_wake_block(ut20p_tx_ctx* ctx);

int ut20p_tx_framebuff_cnt(const ut20p_tx_ctx* ctx);

/* producer (app) side */
struct st_frame* ut20p_tx_get_frame(ut20p_tx_ctx* ctx);
int ut20p_tx_put_frame(ut20p_tx_ctx* ctx, struct st_frame* frame);

/* Cancel a got frame: IN_USER -> FREE (wraps st20p_tx_put_frame_abort). */
int ut20p_tx_put_frame_abort(ut20p_tx_ctx* ctx, struct st_frame* frame);

/* consumer (transport) side: returns 0 and sets *idx on success, -EBUSY when
 * no CONVERTED frame is pending. */
int ut20p_tx_next_frame(ut20p_tx_ctx* ctx, uint16_t* idx);
int ut20p_tx_frame_done(ut20p_tx_ctx* ctx, uint16_t idx);

/**
 * Enable the two-phase external-frame release lifecycle
 * (ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE). frame_done then parks the frame in
 * IN_USER instead of returning it straight to FREE; the app must call
 * ut20p_tx_notify_ext_frame_free() to hand the slot back. Call before starting
 * any worker thread.
 */
void ut20p_tx_ctx_set_manual_release(ut20p_tx_ctx* ctx);

/**
 * Switch the ctx to the internal-converter path (ctx->derive = false, a stub
 * converter installed): put_ext_frame then converts synchronously and fires
 * notify_frame_done early with the frame left CONVERTED, never IN_USER. Also
 * sets ST20P_TX_FLAG_EXT_FRAME. Call before ut20p_tx_get_frame().
 */
void ut20p_tx_ctx_set_internal_converter(ut20p_tx_ctx* ctx);

/** Wraps st20p_tx_put_ext_frame(). */
int ut20p_tx_put_ext_frame(ut20p_tx_ctx* ctx, struct st_frame* frame,
                           struct st_ext_frame* ext_frame);

/** Wraps st20p_tx_notify_ext_frame_free(): IN_USER -> FREE. */
int ut20p_tx_notify_ext_frame_free(ut20p_tx_ctx* ctx, uint16_t idx);

/** Register ops.notify_frame_done, so frame_done()/if_frame_late() invoke it. */
void ut20p_tx_set_notify_frame_done(ut20p_tx_ctx* ctx,
                                    int (*cb)(void* priv, struct st_frame* frame),
                                    void* priv);

/**
 * Set framebuffer i to READY state (not yet converted), so that
 * tx_st20p_convert_get_frame finds it in the concurrent-converter tests.
 */
void ut20p_tx_set_frame_ready(ut20p_tx_ctx* ctx, int idx);

/** Wraps tx_st20p_convert_get_frame() — the external converter claim. */
struct st20_convert_frame_meta* ut20p_tx_convert_get_frame(ut20p_tx_ctx* ctx);

/** Wraps tx_st20p_convert_put_frame(). result 0 = success, <0 = fail. */
int ut20p_tx_convert_put_frame(ut20p_tx_ctx* ctx, struct st20_convert_frame_meta* frame,
                               int result);

/** Buffer index encoded in a convert-frame meta pointer. */
int ut20p_tx_convert_frame_idx(const struct st20_convert_frame_meta* meta);

/* Buffer index that a user-facing frame belongs to. */
int ut20p_tx_frame_idx(const struct st_frame* frame);

/* 1 if every framebuffer is back in the FREE state (no leak). */
int ut20p_tx_all_free(const ut20p_tx_ctx* ctx);

/* Raw stat value of framebuffer i (for diagnostics). */
int ut20p_tx_frame_stat(const ut20p_tx_ctx* ctx, int i);

uint64_t ut20p_tx_stat_frames_sent(const ut20p_tx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20P_TX_PIPELINE_HARNESS_H_ */
