/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for the NEW-API (unified session) video RX frame-count tests.
 *
 * Mirrors tests/unit/pipeline/st20p_harness.h, but pins the unified-session
 * RX contract instead of the pipeline one:
 *   - s->stats.buffers_processed bumps in video_rx_buffer_get() (app consumes),
 *     NOT when the datapath enqueues a frame.
 *   - s->stats.buffers_dropped bumps in video_rx_notify_frame_ready() when the
 *     ready_ring is full (back-pressure; the frame is returned to the library).
 *   - a non-COMPLETE frame is still delivered by buffer_get with
 *     status MTL_FRAME_STATUS_INCOMPLETE and the MTL_BUF_FLAG_INCOMPLETE flag.
 *
 * Drives video_rx_notify_frame_ready() directly with synthetic meta — no
 * transport session is stitched in. The unified-session counters are about
 * producer/consumer accounting; transport realism is not required.
 */

#ifndef _ST20_NEW_API_RX_HARNESS_H_
#define _ST20_NEW_API_RX_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_session_api.h"
#include "st20_api.h"
#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20rx_ctx ut20rx_ctx;

int ut20rx_init(void);

/** Create an RX test context whose ready_ring holds exactly framebuff_cnt
 *  frames (so overflow → buffers_dropped is deterministic). */
ut20rx_ctx* ut20rx_ctx_create(int framebuff_cnt);
void ut20rx_ctx_destroy(ut20rx_ctx* ctx);

/** Inject one synthetic frame as if the datapath just received it.
 *  status is an enum st_frame_status (COMPLETE / CORRUPTED / ...).
 *  Returns 0 when enqueued onto the ready_ring, or -ENOSPC when the ring was
 *  full (drives the buffers_dropped path). */
int ut20rx_inject_frame(ut20rx_ctx* ctx, enum st_frame_status status, uint32_t timestamp);

/** Wraps video_rx_buffer_get() with timeout_ms=0 (non-blocking). */
mtl_buffer_t* ut20rx_buffer_get(ut20rx_ctx* ctx);

/** Wraps video_rx_buffer_put(). */
int ut20rx_buffer_put(ut20rx_ctx* ctx, mtl_buffer_t* buf);

/* ── stat accessors (raw s->stats fields) ─────────────────────────────── */

uint64_t ut20rx_buffers_processed(const ut20rx_ctx* ctx);
uint64_t ut20rx_buffers_dropped(const ut20rx_ctx* ctx);
uint64_t ut20rx_bytes_processed(const ut20rx_ctx* ctx);

/* ── vtable stats surfaces ────────────────────────────────────────────── */

/** Wraps the abstract stats_get vtable entry (video_rx_stats_get). */
int ut20rx_stats_get(ut20rx_ctx* ctx, mtl_session_stats_t* stats);

/** Wraps the passthrough io_stats_get vtable entry (video_rx_io_stats_get). */
int ut20rx_io_stats_get(ut20rx_ctx* ctx, struct st20_rx_user_stats* stats);

/** Wraps the shared stats_reset vtable entry (video_session_stats_reset). */
int ut20rx_reset_stats(ut20rx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_NEW_API_RX_HARNESS_H_ */
