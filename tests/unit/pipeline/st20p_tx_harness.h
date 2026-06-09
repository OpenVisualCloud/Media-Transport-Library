/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST20p (video) pipeline-layer TX unit tests.
 *
 * Parallel TX mirror of tests/unit/pipeline/st20p_harness.h (RX). Drives the
 * pipeline TX frame state machine of lib/src/st2110/pipeline/st20_pipeline_tx.c
 * directly through st20p_tx_get_frame / st20p_tx_put_frame and the static
 * tx_st20p_next_frame / tx_st20p_frame_done callbacks, with no transport
 * session stitched in. derive=true so the converter path is bypassed.
 *
 * mt_get_ptp_time() is controlled via ut20ptx_set_ptp_now() (stored in
 * impl.ptp_usync and returned by the harness ptp stub), so the late-drop
 * predicate tx_st20p_if_frame_late can be exercised deterministically.
 */

#ifndef _ST20P_PIPELINE_TX_HARNESS_H_
#define _ST20P_PIPELINE_TX_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st20_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20ptx_ctx ut20ptx_ctx;

int ut20ptx_init(void);

ut20ptx_ctx* ut20ptx_ctx_create(int framebuff_cnt);
void ut20ptx_ctx_destroy(ut20ptx_ctx* ctx);

/* drop-when-late / pacing controls */
void ut20ptx_set_drop_when_late(ut20ptx_ctx* ctx, bool on);
void ut20ptx_set_user_pacing(ut20ptx_ctx* ctx, bool on);
void ut20ptx_set_fps(ut20ptx_ctx* ctx, enum st_fps fps);
void ut20ptx_set_ptp_now(ut20ptx_ctx* ctx, uint64_t ns);

/* ── frame state machine drivers ─────────────────────────────────────── */

/** Wraps st20p_tx_get_frame (FREE -> IN_USER). NULL when no free frame. */
struct st_frame* ut20ptx_get_frame(ut20ptx_ctx* ctx);
/** Wraps st20p_tx_put_frame (IN_USER -> CONVERTED in derive mode). */
int ut20ptx_put_frame(ut20ptx_ctx* ctx, struct st_frame* frame);
/** Wraps tx_st20p_next_frame (CONVERTED -> IN_TRANSMITTING, or drop). 0 + *idx
 *  + fills *meta, or -EBUSY. */
int ut20ptx_next_frame(ut20ptx_ctx* ctx, uint16_t* idx, struct st20_tx_frame_meta* meta);
/** Wraps tx_st20p_frame_done (IN_TRANSMITTING -> FREE). */
int ut20ptx_frame_done(ut20ptx_ctx* ctx, uint16_t idx);

/* ── inspection ───────────────────────────────────────────────────────── */

/** Returns enum st20p_tx_frame_status of framebuff idx. */
int ut20ptx_frame_stat(ut20ptx_ctx* ctx, uint16_t idx);
/** Count of ops.notify_frame_late() callbacks fired (drop-when-late events). */
uint32_t ut20ptx_notify_late_cnt(const ut20ptx_ctx* ctx);

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20ptx_stat_frames_dropped(const ut20ptx_ctx* ctx);
uint64_t ut20ptx_stat_frames_sent(const ut20ptx_ctx* ctx);

/* ── public-API wrappers ──────────────────────────────────────────────── */

int ut20ptx_get_session_stats(ut20ptx_ctx* ctx, struct st20_tx_user_stats* stats);
int ut20ptx_reset_session_stats(ut20ptx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20P_PIPELINE_TX_HARNESS_H_ */
