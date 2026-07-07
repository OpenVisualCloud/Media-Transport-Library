/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-20 (video) TX epoch/pacing math unit tests.
 *
 * Includes the production st_tx_video_session.c directly so the file-local
 * static functions (calc_frame_count_since_epoch, tv_sync_pacing,
 * validate_user_timestamp, transmission_start_time, ...) become visible in
 * this translation unit. Non-static symbols duplicate those in libmtl;
 * --allow-multiple-definition resolves this. USDT is disabled to avoid
 * probe-semaphore link references.
 *
 * mt_get_tsc() is mocked with a preprocessor seam instead of any production
 * source change: mt_main.h is included first, under its real name, so the
 * genuine mt_get_tsc() compiles normally; its include guard then makes the
 * copy pulled in transitively by st_tx_video_session.c a no-op, so the
 * `#define mt_get_tsc ...` below only rewrites the call sites written inside
 * st_tx_video_session.c itself. mt_get_ptp_time() needs no such seam -- it
 * already dispatches through the pre-existing, production `ptp_get_time_fn`
 * function-pointer field on `struct mt_interface`.
 *
 * Only pacing math is exercised here -- no mbuf pool, queue, or packet I/O
 * is set up, since neither target function touches packets.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"
#include "st2110/st_tx_video_session.h"

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut_txv_ctx {
  struct mtl_main_impl impl;
  struct st_tx_video_session_impl session;
  uint64_t mock_ptp_ns;
  uint64_t mock_tsc_ns;
  int notify_late_calls;
  uint64_t notify_late_last_delta;
};

#include "session/st20_tx_harness.h"

/* ── mocked time sources ──────────────────────────────────────────────── */

static uint64_t ut_txv_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl; /* impl is ctx's first member */
  return ctx->mock_ptp_ns;
}

static uint64_t ut_txv_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_txv_ctx* ctx = (struct ut_txv_ctx*)impl;
  return ctx->mock_tsc_ns;
}

static int ut_txv_notify_frame_late(void* priv, uint64_t epoch_skipped) {
  struct ut_txv_ctx* ctx = priv;
  ctx->notify_late_calls++;
  ctx->notify_late_last_delta = epoch_skipped;
  return 0;
}

/* Seam: from here on, calls to mt_get_tsc() written inside st_tx_video_session.c
 * resolve to our mock instead. The real mt_get_tsc() compiled above (via the
 * explicit mt_main.h include) is unaffected. */
#define mt_get_tsc ut_txv_tsc_time_fn
#include "st2110/st_tx_video_session.c"
#undef mt_get_tsc

/* ── init (delegates to common) ───────────────────────────────────────── */

int ut_txv_init(void) {
  return ut_eal_init();
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut_txv_ctx* ut_txv_create(void) {
  ut_txv_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut_txv_ptp_time_fn;

  struct st_tx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->idx = 0;
  s->pacing.frame_time = 1000000.0L; /* 1ms, round number for simple math */
  s->pacing.max_onward_epochs = 3;
  s->ops.notify_frame_late = ut_txv_notify_frame_late;
  s->ops.priv = ctx;

  return ctx;
}

void ut_txv_destroy(ut_txv_ctx* ctx) {
  free(ctx);
}

/* ── pacing setup ─────────────────────────────────────────────────────── */

void ut_txv_set_frame_time(ut_txv_ctx* ctx, long double frame_time_ns) {
  ctx->session.pacing.frame_time = frame_time_ns;
}

void ut_txv_set_max_onward_epochs(ut_txv_ctx* ctx, uint32_t max_onward_epochs) {
  ctx->session.pacing.max_onward_epochs = max_onward_epochs;
}

void ut_txv_set_cur_epochs(ut_txv_ctx* ctx, uint64_t cur_epochs) {
  ctx->session.pacing.cur_epochs = cur_epochs;
}

void ut_txv_set_tr_offset(ut_txv_ctx* ctx, long double tr_offset_ns) {
  ctx->session.pacing.tr_offset = tr_offset_ns;
}

void ut_txv_set_vrx(ut_txv_ctx* ctx, uint32_t vrx) {
  ctx->session.pacing.vrx = vrx;
}

void ut_txv_set_trs(ut_txv_ctx* ctx, long double trs_ns) {
  ctx->session.pacing.trs = trs_ns;
}

void ut_txv_set_exact_user_pacing(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_EXACT_USER_PACING;
}

void ut_txv_set_user_pacing(ut_txv_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST20_TX_FLAG_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST20_TX_FLAG_USER_PACING;
}

void ut_txv_set_mock_ptp_time(ut_txv_ctx* ctx, uint64_t ptp_ns) {
  ctx->mock_ptp_ns = ptp_ns;
}

void ut_txv_set_mock_tsc_time(ut_txv_ctx* ctx, uint64_t tsc_ns) {
  ctx->mock_tsc_ns = tsc_ns;
}

/* ── code under test ──────────────────────────────────────────────────── */

uint64_t ut_txv_calc_frame_count_since_epoch(ut_txv_ctx* ctx, uint64_t cur_tai,
                                             uint64_t required_tai) {
  return calc_frame_count_since_epoch(&ctx->session, cur_tai, required_tai);
}

int ut_txv_sync_pacing(ut_txv_ctx* ctx, uint64_t required_tai) {
  return tv_sync_pacing(&ctx->impl, &ctx->session, required_tai);
}

uint64_t ut_txv_pacing_required_tai(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp) {
  return tv_pacing_required_tai(&ctx->session, tfmt, timestamp);
}

/* ── accessors ─────────────────────────────────────────────────────────── */

uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.cur_epochs;
}

long double ut_txv_tsc_time_cursor(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.tsc_time_cursor;
}

long double ut_txv_ptp_time_cursor(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.ptp_time_cursor;
}

uint64_t ut_txv_tsc_time_frame_start(const ut_txv_ctx* ctx) {
  return ctx->session.pacing.tsc_time_frame_start;
}

uint64_t ut_txv_stat_epoch_onward(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_onward;
}

uint64_t ut_txv_stat_epoch_drop(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_drop;
}

uint64_t ut_txv_stat_error_user_timestamp(const ut_txv_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_error_user_timestamp;
}

int ut_txv_notify_late_calls(const ut_txv_ctx* ctx) {
  return ctx->notify_late_calls;
}

uint64_t ut_txv_notify_late_last_delta(const ut_txv_ctx* ctx) {
  return ctx->notify_late_last_delta;
}
