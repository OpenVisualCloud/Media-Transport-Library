/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST20p (video) pipeline-layer unit tests.
 *
 * Pins the pipeline-internal frame-count contract:
 *   - stat_frames_received bumps in st20p_rx_get_frame() (app consumes), NOT
 *     when the underlying transport completes a frame.
 *   - stat_frames_dropped bumps in rx_st20p_frame_ready() when no free
 *     framebuffer is available (back-pressure; the transport-side frame
 *     is discarded by the pipeline).
 *   - stat_frames_corrupted bumps in st20p_rx_get_frame() iff the delivered
 *     frame's status is ST_FRAME_STATUS_CORRUPTED.
 *
 * Drives rx_st20p_frame_ready() directly with synthetic meta — no transport
 * session is stitched in. Pipeline counters are about producer/consumer
 * accounting; transport realism is not required to exercise them.
 */

#ifndef _ST20P_PIPELINE_HARNESS_H_
#define _ST20P_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20p_ctx ut20p_ctx;

int ut20p_init(void);

/** Create a pipeline test context with the given framebuffer ring depth. */
ut20p_ctx* ut20p_ctx_create(int framebuff_cnt);
void ut20p_ctx_destroy(ut20p_ctx* ctx);

/** Inject one synthetic frame into the pipeline as if the transport just
 *  completed it.  status is ST_FRAME_STATUS_COMPLETE or _CORRUPTED.
 *  Returns 0 on accept, -EBUSY when no free framebuf (drives the
 *  stat_frames_dropped path). */
int ut20p_inject_frame(ut20p_ctx* ctx, enum st_frame_status status, uint32_t timestamp);

/** Wraps st20p_rx_get_frame(). */
struct st_frame* ut20p_get_frame(ut20p_ctx* ctx);

/** Wraps st20p_rx_put_frame(). */
int ut20p_put_frame(ut20p_ctx* ctx, struct st_frame* frame);

/* ── stat accessors ───────────────────────────────────────────────── */

uint64_t ut20p_stat_frames_received(const ut20p_ctx* ctx);
uint64_t ut20p_stat_frames_dropped(const ut20p_ctx* ctx);
uint64_t ut20p_stat_frames_corrupted(const ut20p_ctx* ctx);
uint32_t ut20p_stat_busy(const ut20p_ctx* ctx);

/* ── public-API wrappers ─────────────────────────────────────────── */

/** Wraps st20p_rx_get_session_stats() — exercises the atomic overlay. */
int ut20p_get_session_stats(ut20p_ctx* ctx, struct st20_rx_user_stats* stats);

/** Wraps st20p_rx_reset_session_stats(). */
int ut20p_reset_session_stats(ut20p_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20P_PIPELINE_HARNESS_H_ */
