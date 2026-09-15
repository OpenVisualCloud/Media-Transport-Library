/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the ST 2110-30 (audio) TX pacing/timestamp unit tests.
 *
 * Wraps a single `st_tx_audio_session_impl` and drives the production frame
 * tasklet (`tx_audio_session_tasklet_frame()`) so tests observe exactly what
 * the app would see via `notify_frame_done()` and what actually went out on
 * the wire. Both mocked time sources (PTP and TSC) are fully controllable so
 * tests can hit exact boundary values deterministically. All MTL internal
 * types are kept opaque so the C++ test layer never includes libmtl headers.
 */

#ifndef _ST30_TX_SESSION_HARNESS_H_
#define _ST30_TX_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "st30_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_txa30_ctx ut_txa30_ctx;

/* Initialise the shared DPDK EAL. Idempotent -- safe to call once per gtest
 * fixture SetUp(). Returns 0 on success, < 0 on failure. */
int ut_txa30_init(void);

/* Create a context with default pacing (1ms ptime @ 48kHz sampling, 48
 * samples/packet, max_onward_epochs=3). Caller owns the returned pointer and
 * must free it with ut_txa30_destroy(). Returns NULL on allocation failure. */
ut_txa30_ctx* ut_txa30_create(void);
void ut_txa30_destroy(ut_txa30_ctx* ctx);

/* ── pacing setup ──────────────────────────────────────────────────────── */
/* Toggle ST30_TX_FLAG_USER_PACING on the session's ops.flags. */
void ut_txa30_set_user_pacing(ut_txa30_ctx* ctx, bool enable);
/* Toggle ST30_TX_FLAG_USER_TIMESTAMP on the session's ops.flags: the RTP
 * timestamp is taken from the app-supplied timestamp instead of the
 * pacing-scheduled instant. Selects the branch of
 * tx_audio_pacing_update_rtp_time_stamp() whose returned "reporting" TAI
 * instant must match the RTP timestamp it derived. */
void ut_txa30_set_user_timestamp(ut_txa30_ctx* ctx, bool enable);
/* Set ops.rtp_timestamp_delta_us, added to the RTP-timestamp source instant. */
void ut_txa30_set_rtp_timestamp_delta_us(ut_txa30_ctx* ctx, int32_t delta_us);

/* Mocked time sources consumed by tx_audio_session_sync_pacing() via
 * mt_get_ptp_time()/mt_get_tsc(). Take effect immediately. */
void ut_txa30_set_mock_ptp_time(ut_txa30_ctx* ctx, uint64_t ptp_ns);
void ut_txa30_set_mock_tsc_time(ut_txa30_ctx* ctx, uint64_t tsc_ns);

/* ── code under test ───────────────────────────────────────────────────── */
/* Drives the production frame tasklet (tx_audio_session_tasklet_frame())
 * once per packet of a `total_pkts`-packet frame. Packet 0's app-supplied
 * basis is (tfmt, timestamp); the harness's get_next_frame stub is only
 * consulted for that first packet, matching production (only pkt_idx==0
 * fetches a new frame). Captures notify_frame_done()'s reported meta and the
 * RTP header timestamp actually written into each packet's wire bytes.
 * Returns 0 on success. */
int ut_txa30_run_frame_tasklet(ut_txa30_ctx* ctx, enum st10_timestamp_fmt tfmt,
                               uint64_t timestamp, int total_pkts);

/* ── accessors ─────────────────────────────────────────────────────────── */
uint64_t ut_txa30_stat_error_user_timestamp(const ut_txa30_ctx* ctx);
int ut_txa30_notify_frame_done_calls(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_notify_frame_done_timestamp(const ut_txa30_ctx* ctx);
uint32_t ut_txa30_notify_frame_done_rtp_timestamp(const ut_txa30_ctx* ctx);
/* RTP header timestamps (host order), one per packet built by
 * ut_txa30_run_frame_tasklet(), in build order -- what actually went out on
 * the wire, independent of what notify_frame_done() was told. */
int ut_txa30_wire_rtp_timestamp_count(const ut_txa30_ctx* ctx);
uint32_t ut_txa30_wire_rtp_timestamp(const ut_txa30_ctx* ctx, int i);

#ifdef __cplusplus
}
#endif

#endif /* _ST30_TX_SESSION_HARNESS_H_ */
