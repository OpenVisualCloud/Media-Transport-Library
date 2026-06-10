/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for the NEW-API (unified session) video TX tests.
 *
 * Drives the FREE -> APP_OWNED -> READY -> TRANSMITTING -> FREE frame state
 * machine of lib/src/new_api/mt_session_video_tx.c directly, with no NIC, no
 * transport session and no EAL datapath. A hand-built mtl_session_impl +
 * st_tx_video_session_impl + video_tx_ctx is wired so the production
 * buffer_get / buffer_put / get_next_frame / frame_done callbacks operate on
 * our in-memory frames.
 *
 * mt_get_ptp_time() is an inline that calls impl->inf[port].ptp_get_time_fn,
 * so the late-drop wall clock is controlled via ut20tx_set_ptp_now() (stored
 * in impl.ptp_usync and returned by the harness ptp stub) — no link override.
 */

#ifndef _ST20_NEW_API_TX_HARNESS_H_
#define _ST20_NEW_API_TX_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_session_api.h"
#include "st20_api.h"
#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20tx_ctx ut20tx_ctx;

int ut20tx_init(void);

/** Create a TX test context with framebuff_cnt library-owned frames, all FREE,
 *  derive=true (no conversion). */
ut20tx_ctx* ut20tx_ctx_create(int framebuff_cnt);
void ut20tx_ctx_destroy(ut20tx_ctx* ctx);

/* drop-when-late / pacing controls (mirror what session_init would set) */
void ut20tx_set_drop_when_late(ut20tx_ctx* ctx, bool on);
void ut20tx_set_user_pacing(ut20tx_ctx* ctx, bool on);
void ut20tx_set_fps(ut20tx_ctx* ctx, enum st_fps fps);
/** Set the wall clock returned by mt_get_ptp_time() (TAI ns). */
void ut20tx_set_ptp_now(ut20tx_ctx* ctx, uint64_t ns);

/* ── user-owned mode drivers ─────────────────────────────────────────── */

/** Switch the session to MTL_BUFFER_USER_OWNED and init the user-buf ring. */
int ut20tx_set_user_owned(ut20tx_ctx* ctx);
/** Post an external user buffer (queued for get_next_frame to bind). */
int ut20tx_post_user_buffer(ut20tx_ctx* ctx, void* data, void* user_ctx);
/** Stamp a frame slot's tv_meta with a TAI timestamp, bypassing buffer_put. */
void ut20tx_frame_set_timestamp(ut20tx_ctx* ctx, uint16_t idx, uint64_t tai_ns);
/** Read the per-frame user_ctx slot (user-owned completion bookkeeping). */
void* ut20tx_user_buf_ctx(ut20tx_ctx* ctx, uint16_t idx);

/* ── frame state machine drivers ─────────────────────────────────────── */

/** Wraps the buffer_get vtable entry (FREE -> APP_OWNED). NULL on -ETIMEDOUT. */
mtl_buffer_t* ut20tx_buffer_get(ut20tx_ctx* ctx);
/** Stamp a TAI timestamp onto a buffer before put (drives tv_meta passthrough). */
void ut20tx_buffer_set_timestamp(mtl_buffer_t* buf, uint64_t tai_ns);
/** Attach user metadata to a buffer before put. */
void ut20tx_buffer_set_user_meta(mtl_buffer_t* buf, void* meta, size_t size);
/** Wraps the buffer_put vtable entry (APP_OWNED -> READY). */
int ut20tx_buffer_put(ut20tx_ctx* ctx, mtl_buffer_t* buf);
/** Wraps video_tx_get_next_frame (READY -> TRANSMITTING). 0 + *idx, or -EBUSY. */
int ut20tx_get_next_frame(ut20tx_ctx* ctx, uint16_t* idx);
/** Wraps video_tx_notify_frame_done (TRANSMITTING -> FREE). */
int ut20tx_frame_done(ut20tx_ctx* ctx, uint16_t idx);

/* ── state / metadata inspection ─────────────────────────────────────── */

/** Returns the enum tx_frame_state of frame idx (0=FREE,1=APP_OWNED,2=READY,
 *  3=TRANSMITTING). */
int ut20tx_frame_state(ut20tx_ctx* ctx, uint16_t idx);
/** Returns the tv_meta.user_meta pointer recorded on frame idx by buffer_put. */
const void* ut20tx_frame_user_meta(ut20tx_ctx* ctx, uint16_t idx);
/** Returns the transport framebuffer address of frame idx. */
void* ut20tx_frame_addr(ut20tx_ctx* ctx, uint16_t idx);

/* ── stat accessors (raw s->stats fields) ────────────────────────────── */

uint64_t ut20tx_buffers_processed(const ut20tx_ctx* ctx);
uint64_t ut20tx_buffers_dropped(const ut20tx_ctx* ctx);
uint64_t ut20tx_bytes_processed(const ut20tx_ctx* ctx);

/* ── vtable stats surfaces ────────────────────────────────────────────── */

int ut20tx_stats_get(ut20tx_ctx* ctx, mtl_session_stats_t* stats);
int ut20tx_io_stats_get(ut20tx_ctx* ctx, struct st20_tx_user_stats* stats);
int ut20tx_reset_stats(ut20tx_ctx* ctx);

/* ── event drain ──────────────────────────────────────────────────────── */

/** Non-blocking event_poll wrapper. 0 + *ev on an event, -ETIMEDOUT if none. */
int ut20tx_poll_event(ut20tx_ctx* ctx, mtl_event_t* ev);
/** event_poll wrapper with an explicit timeout (drives the blocking path). */
int ut20tx_poll_event_timeout(ut20tx_ctx* ctx, mtl_event_t* ev, uint32_t timeout_ms);
/** Post an event through the producer path (mtl_session_event_post). */
int ut20tx_post_event(ut20tx_ctx* ctx, const mtl_event_t* ev);
/** Read the producer-side dropped-event counter (s->events_dropped). */
uint64_t ut20tx_events_dropped(const ut20tx_ctx* ctx);
/** Wraps the get_event_fd vtable slot; -ENOSYS if the slot is NULL. */
int ut20tx_get_event_fd(ut20tx_ctx* ctx);
/** Set the session stopped flag (drives the poll -EAGAIN path). */
void ut20tx_set_stopped(ut20tx_ctx* ctx);
/** Drive the full production stop path (sets stopped + signals the eventfd to
 *  wake a consumer already blocked in event_poll). */
void ut20tx_stop(ut20tx_ctx* ctx);

/* ── slice op ─────────────────────────────────────────────────────────── */

/** Wraps the slice_ready vtable entry (slice TX is not implemented). */
int ut20tx_slice_ready(ut20tx_ctx* ctx, mtl_buffer_t* buf, uint16_t lines);

/* ── buffer pool sizing ───────────────────────────────────────────────── */

/** Number of low-level frames (mtl_session_video_frame_count). */
uint32_t ut20tx_frame_count(ut20tx_ctx* ctx);
/** Current wrapper pool size (s->buffer_count). */
uint32_t ut20tx_buffer_count(ut20tx_ctx* ctx);
/** Drop the wrapper pool, then rebuild it via the production create-path
 *  helper (mtl_session_init_buffers). Returns its result. */
int ut20tx_init_buffers(ut20tx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_NEW_API_TX_HARNESS_H_ */
