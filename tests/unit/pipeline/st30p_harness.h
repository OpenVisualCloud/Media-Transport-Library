/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST30p (audio) pipeline-layer unit tests.
 *
 * Pins the pipeline-internal frame-count contract:
 *   - stat_frames_received bumps in st30p_rx_get_frame() (app consumes), NOT
 *     when the underlying transport completes a frame.
 *   - stat_frames_dropped bumps in rx_st30p_frame_ready() when no free
 *     framebuffer is available (back-pressure; transport-side buffer is
 *     released by the pipeline returning -EBUSY).
 *
 * ST30p has no stat_frames_corrupted — audio frames are never delivered
 * with status CORRUPTED at this layer.
 */

#ifndef _ST30P_PIPELINE_HARNESS_H_
#define _ST30P_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st30_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut30p_ctx ut30p_ctx;

int ut30p_init(void);

ut30p_ctx* ut30p_ctx_create(int framebuff_cnt);
void ut30p_ctx_destroy(ut30p_ctx* ctx);

/** Inject one synthetic audio frame as if the transport just completed it.
 *  status is ST_FRAME_STATUS_COMPLETE or _CORRUPTED.
 *  Returns 0 on accept, -EBUSY when no free framebuf (drives the
 *  stat_frames_dropped path). */
int ut30p_inject_frame(ut30p_ctx* ctx, enum st_frame_status status, uint32_t timestamp);

struct st30_frame* ut30p_get_frame(ut30p_ctx* ctx);
int ut30p_put_frame(ut30p_ctx* ctx, struct st30_frame* frame);

/* Buffer index that a user-facing frame belongs to (for ownership tracking). */
int ut30p_frame_idx(const struct st30_frame* frame);
int ut30p_framebuff_cnt(const ut30p_ctx* ctx);

/* Raw stat value of framebuffer i (for diagnostics). */
int ut30p_frame_stat(const ut30p_ctx* ctx, int i);

uint64_t ut30p_stat_frames_received(const ut30p_ctx* ctx);
uint64_t ut30p_stat_frames_dropped(const ut30p_ctx* ctx);
uint64_t ut30p_stat_frames_corrupted(const ut30p_ctx* ctx);
uint32_t ut30p_stat_busy(const ut30p_ctx* ctx);

int ut30p_get_session_stats(ut30p_ctx* ctx, struct st30_rx_user_stats* stats);
int ut30p_reset_session_stats(ut30p_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30P_PIPELINE_HARNESS_H_ */
