/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST22p (compressed video) RX pipeline-layer concurrency and
 * blocking-wait unit tests.
 *
 * Exposes the role-halves of the lock-free RX framebuffer ring so a
 * gtest can drive them from independent threads:
 *   - producer (transport):    inject_frame (FREE->DECODED via frame_ready)
 *   - consumer (app):          get_frame (DECODED->IN_USER) / put_frame (->FREE)
 *   - decoder plugin:          decode_get_frame (READY->IN_DECODING)
 * plus the blocking-wait setup the app and plugin sides each have.
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

/**
 * Turn on ST22P_RX_FLAG_BLOCK_GET behaviour: init the block cond/mutex and set
 * the blocking get_frame timeout. Call before any blocking get_frame. Mirrors
 * the block setup st22p_rx_create() performs.
 */
void ut22p_ctx_enable_blocking(ut22p_ctx* ctx, uint64_t timeout_ns);

/** Wake a blocking get_frame sleeper (wraps st22p_rx_wake_block). */
void ut22p_wake_block(ut22p_ctx* ctx);

/**
 * Turn on ST22_DECODER_RESP_FLAG_BLOCK_GET behaviour for the decoder-plugin
 * side: init the decode block cond/mutex and set its wait timeout. Call before
 * any ut22p_decode_get_frame().
 */
void ut22p_ctx_enable_decode_blocking(ut22p_ctx* ctx, uint64_t timeout_ns);

/** Wake a blocking decode_get_frame sleeper, via the callback the decoder
 * device is registered with (wraps rx_st22p_decode_wake_block). */
void ut22p_decode_wake_block(ut22p_ctx* ctx);

/* decoder-plugin side: claim READY->IN_DECODING. Returns 0 when a frame was
 * handed back, -EBUSY when none was. Only -EBUSY is reachable here: the ctx is
 * built in derive mode, where frame_ready never leaves a frame in READY. */
int ut22p_decode_get_frame(ut22p_ctx* ctx);

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

/* Times the harness stub ran; 0 means put_frame stopped reaching the transport
 * put path. */
uint64_t ut22p_stub_call_count(void);

#ifdef __cplusplus
}
#endif

#endif /* _ST22P_RX_PIPELINE_HARNESS_H_ */
