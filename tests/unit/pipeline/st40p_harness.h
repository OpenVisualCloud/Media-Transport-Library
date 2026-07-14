/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for ST40p (ancillary) pipeline-layer unit tests.
 *
 * Pins the pipeline-internal FRAME_LEVEL zero-copy contract:
 *   - stat_frames_received / stat_frames_corrupted bump in
 *     rx_st40p_frame_ready() (transport delivery), NOT in get_frame() —
 *     unlike ST30p, where the app's get_frame call is the counting point.
 *   - stat_frames_dropped / stat_busy bump 1:1 in rx_st40p_frame_ready()
 *     when no free framebuf is available (back-pressure).
 *   - put_frame / put_frame_abort release the transport-owned UDW slot via
 *     st40_rx_put_framebuff() and must clear frame_info->udw_buff_addr.
 */

#ifndef _ST40P_PIPELINE_HARNESS_H_
#define _ST40P_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st40_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut40p_ctx ut40p_ctx;

int ut40p_init(void);

ut40p_ctx* ut40p_ctx_create(int framebuff_cnt);
void ut40p_ctx_destroy(ut40p_ctx* ctx);

/** Inject one synthetic ANC frame as if the transport just completed it.
 *  udw_addr is any non-NULL sentinel pointer identifying the transport slot.
 *  Returns 0 on accept, -EBUSY when no free framebuf (drives the
 *  stat_frames_dropped path). */
int ut40p_inject_frame(ut40p_ctx* ctx, void* udw_addr, enum st_frame_status status,
                       bool seq_discont, uint32_t timestamp);

struct st40_frame_info* ut40p_get_frame(ut40p_ctx* ctx);
int ut40p_put_frame(ut40p_ctx* ctx, struct st40_frame_info* frame);
int ut40p_put_frame_abort(ut40p_ctx* ctx, struct st40_frame_info* frame);

/* ── concurrency-test helpers ─────────────────────────────────────────── */

/** Buffer index that a user-facing frame belongs to. */
int ut40p_frame_idx(const struct st40_frame_info* frame);

/** Total framebuffer count. */
int ut40p_framebuff_cnt(const ut40p_ctx* ctx);

/** Raw stat value of framebuffer i (for diagnostics). */
int ut40p_frame_stat(const ut40p_ctx* ctx, int i);

uint32_t ut40p_stat_drop_frame(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_received(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_dropped(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_corrupted(const ut40p_ctx* ctx);
uint32_t ut40p_stat_busy(const ut40p_ctx* ctx);

/** put_framebuff() release-wiring spy: what the stub last saw. */
int ut40p_put_framebuff_call_count(void);
void* ut40p_put_framebuff_last_addr(void);
void ut40p_put_framebuff_reset_spy(void);

int ut40p_get_session_stats(ut40p_ctx* ctx, struct st40_rx_user_stats* stats);
int ut40p_reset_session_stats(ut40p_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST40P_PIPELINE_HARNESS_H_ */
