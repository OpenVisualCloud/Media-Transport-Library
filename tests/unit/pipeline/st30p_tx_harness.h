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
 * The harness bypasses create_transport: there is no real DPDK session, the
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

int ut30p_tx_framebuff_cnt(const ut30p_tx_ctx* ctx);

/* producer (app) side */
struct st30_frame* ut30p_tx_get_frame(ut30p_tx_ctx* ctx);
int ut30p_tx_put_frame(ut30p_tx_ctx* ctx, struct st30_frame* frame);

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

#ifdef __cplusplus
}
#endif

#endif /* _ST30P_TX_PIPELINE_HARNESS_H_ */
