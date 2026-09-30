/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST30p (audio) TX pipeline-layer concurrency unit tests.
 *
 * Exposes the two role-halves of the lock-free TX framebuffer ring so a
 * gtest can drive them from independent threads:
 *   - producer (app):       get_frame (FREE->IN_USER) / put_frame (->READY)
 *   - consumer (transport):  next_frame (READY->IN_TRANSMITTING) /
 *                            frame_done (->FREE)
 *
 * The ring helpers bypass create_transport: there is no real DPDK session, the
 * framebuffers are plain heap memory and the ctx is hand-initialised into the
 * "ready" state. This isolates the claim/lifecycle state machine so the test
 * can hammer it with many threads and assert single-ownership + conservation +
 * deadlock-freedom.
 */

#ifndef _ST30P_TX_PIPELINE_HARNESS_H_
#define _ST30P_TX_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st30_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut30p_tx_ctx ut30p_tx_ctx;

int ut30p_tx_init(void);

ut30p_tx_ctx* ut30p_tx_ctx_create(int framebuff_cnt);
void ut30p_tx_ctx_destroy(ut30p_tx_ctx* ctx);

/**
 * Turn on ST30P_TX_FLAG_BLOCK_GET behaviour: init the block cond/mutex, arm the
 * wake_on_destroy hook, and set the blocking get_frame timeout. Call before any
 * blocking get_frame. Mirrors the block setup st30p_tx_create() performs.
 *
 * timeout_ns is how long a blocking get_frame() should wait for a free frame
 * before giving up and returning NULL.
 */
void ut30p_tx_ctx_enable_blocking(ut30p_tx_ctx* ctx, uint64_t timeout_ns);

/**
 * App wake (wraps st30p_tx_wake_block()). Ends a get_frame() blocked at the
 * time of the call; a wake with no thread blocked is a no-op.
 */
void ut30p_tx_wake_block(ut30p_tx_ctx* ctx);

/** The wake a frame done sends (wraps tx_st30p_notify_frame_available()). */
void ut30p_tx_frame_free_wake(ut30p_tx_ctx* ctx);

/**
 * Force ctx->lc_destroying, bypassing the CAS handshake real st30p_tx_free()
 * uses. Lets a test flip the flag mid-call to race the destroy-vs-claim
 * window inside the blocking wait loop, without a full teardown.
 */
void ut30p_tx_force_destroying(ut30p_tx_ctx* ctx);

int ut30p_tx_framebuff_cnt(const ut30p_tx_ctx* ctx);

/* producer (app) side */
struct st30_frame* ut30p_tx_get_frame(ut30p_tx_ctx* ctx);
int ut30p_tx_put_frame(ut30p_tx_ctx* ctx, struct st30_frame* frame);

/* Cancel a got frame: IN_USER -> FREE (wraps st30p_tx_put_frame_abort). */
int ut30p_tx_put_frame_abort(ut30p_tx_ctx* ctx, struct st30_frame* frame);

/* consumer (transport) side: returns 0 and sets *idx on success, -EBUSY when
 * no READY frame is pending. */
int ut30p_tx_next_frame(ut30p_tx_ctx* ctx, uint16_t* idx);
int ut30p_tx_frame_done(ut30p_tx_ctx* ctx, uint16_t idx);

/**
 * Set framebuffer i directly to READY state so concurrent-transport tests
 * can prime slots without going through get_frame/put_frame.
 */
void ut30p_tx_set_frame_ready(ut30p_tx_ctx* ctx, int idx);

/* Buffer index that a user-facing frame belongs to. */
int ut30p_tx_frame_idx(const struct st30_frame* frame);

/* 1 if every framebuffer is back in the FREE state (no leak). */
int ut30p_tx_all_free(const ut30p_tx_ctx* ctx);

/* Raw stat value of framebuffer i (for diagnostics). */
int ut30p_tx_frame_stat(const ut30p_tx_ctx* ctx, int i);

uint64_t ut30p_tx_stat_frames_sent(const ut30p_tx_ctx* ctx);

/**
 * Run st30p_tx_create() on a one-port instance whose NIC is on nic_socket, with
 * ops.flags = flags and ops.socket_id = socket_id. Returns the socket the pipeline
 * ctx was placed on, or INT_MIN if the create failed before its transport step.
 */
int ut30p_tx_create_ctx_socket(int nic_socket, uint32_t flags, int socket_id);

/** Register ops.notify_frame_late (and ops.priv). */
void ut30p_tx_set_notify_frame_late(ut30p_tx_ctx* ctx,
                                    int (*cb)(void* priv, uint64_t epoch_skipped),
                                    void* priv);

/**
 * Run create_transport against a stub transport create, then fire the late
 * callback it registered the way the transport session does. Returns the
 * callback's return, or -ENOENT when no late callback was registered.
 */
int ut30p_tx_transport_report_late(ut30p_tx_ctx* ctx, uint64_t epoch_skipped);

/** The pipeline handle, for calling the public st*p API directly. */
st30p_tx_handle ut30p_tx_handle(ut30p_tx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30P_TX_PIPELINE_HARNESS_H_ */
