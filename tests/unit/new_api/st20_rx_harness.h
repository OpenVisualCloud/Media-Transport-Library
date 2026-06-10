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

/** Inject one frame with caller-supplied metadata. The harness picks the
 *  cyclic frame slot; the caller owns every meta field (status, tfmt,
 *  rtp_timestamp, pkts_total, pkts_recv[], second_field, ...). Same return
 *  contract as ut20rx_inject_frame. */
int ut20rx_inject_meta(ut20rx_ctx* ctx, const struct st20_rx_frame_meta* meta);

/** Attach user metadata to a frame slot before injecting it, exercising the
 *  rx_fill_user_metadata pass-through (frame_trans->user_meta → buf->user_meta). */
void ut20rx_set_frame_user_meta(ut20rx_ctx* ctx, int idx, void* meta, size_t size);

/** Switch out of derive mode so buffer_get runs rx_convert_and_fill_buffer.
 *  Provides one app-format destination buffer for every frame slot. */
void ut20rx_enable_convert(ut20rx_ctx* ctx, enum st_frame_fmt app_fmt, void* app_buf,
                           size_t app_size);

/** Count of times the stubbed converter ran since ctx_create (0 in derive mode). */
int ut20rx_convert_calls(const ut20rx_ctx* ctx);

/** Drive video_rx_notify_detected() directly (auto-detect format event). */
int ut20rx_notify_detected(ut20rx_ctx* ctx, uint32_t width, uint32_t height,
                           enum st_fps fps, enum st20_packing packing, bool interlaced);

/** Non-blocking drain of one session event (shared video_session_event_poll). */
int ut20rx_poll_event(ut20rx_ctx* ctx, mtl_event_t* event);

/** Reconfigure the session for USER_OWNED buffer-post mode (no app
 *  query_ext_frame). Returns 0 on success. */
int ut20rx_enable_user_owned_post(ut20rx_ctx* ctx);

/** Reconfigure for USER_OWNED with an explicit (app) query_ext_frame callback,
 *  so notify_frame_ready takes the per-frame opaque-saving branch. */
int ut20rx_enable_user_owned_query_ext(ut20rx_ctx* ctx);

/** Number of low-level frames (mtl_session_video_frame_count). */
uint32_t ut20rx_frame_count(ut20rx_ctx* ctx);
/** Current wrapper pool size (s->buffer_count). */
uint32_t ut20rx_buffer_count(ut20rx_ctx* ctx);
/** Drop the wrapper pool, then rebuild it via the production create-path
 *  helper (mtl_session_init_buffers). Returns its result. */
int ut20rx_init_buffers(ut20rx_ctx* ctx);

/** Wraps the mem_register vtable entry (user-owned zero-copy registration). */
int ut20rx_mem_register(ut20rx_ctx* ctx, void* addr, size_t size);

/** Wraps the buffer_post vtable entry (post a user buffer for receiving). */
int ut20rx_post_user_buffer(ut20rx_ctx* ctx, void* data, size_t size, void* user_ctx);

/** Drive the query_ext_frame wrapper directly; on success *out binds to a
 *  posted user buffer slot. */
int ut20rx_query_ext_frame(ut20rx_ctx* ctx, struct st20_ext_frame* out,
                           struct st20_rx_frame_meta* meta);

/** Drive the framebuff_cnt < 2 clamp through mtl_video_rx_session_init without a
 *  NIC: st20_rx_create is stubbed to capture ops.framebuff_cnt and fail, so init
 *  returns before any transport dereference. Returns the clamped framebuff_cnt. */
uint16_t ut20rx_clamp_framebuff_cnt(uint32_t requested);

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
