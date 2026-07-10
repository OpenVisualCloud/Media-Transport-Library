/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the ST 2110-20 (video) TX epoch/pacing math unit tests.
 *
 * Wraps a single `st_tx_video_session_impl` and exposes thin wrappers over
 * the file-local `calc_frame_count_since_epoch()` and `tv_sync_pacing()`.
 * Both mocked time sources (PTP and TSC) are fully controllable so tests can
 * hit exact boundary values deterministically. All MTL internal types are
 * kept opaque so the C++ test layer never includes libmtl headers.
 */

#ifndef _ST20_TX_SESSION_HARNESS_H_
#define _ST20_TX_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_txv_ctx ut_txv_ctx;

enum ut_txv_pacing_way {
  UT_TXV_PACING_RL = 1,
  UT_TXV_PACING_TSC = 2,
  UT_TXV_PACING_PTP = 4,
};

/* Initialise the shared DPDK EAL. Idempotent — safe to call once per gtest
 * fixture SetUp(). Returns 0 on success, < 0 on failure. */
int ut_txv_init(void);

/* Create a context with default pacing (frame_time=1ms, max_onward_epochs=3,
 * all other pacing fields zeroed). Caller owns the returned pointer and must
 * free it with ut_txv_destroy(). Returns NULL on allocation failure. */
ut_txv_ctx* ut_txv_create(void);
void ut_txv_destroy(ut_txv_ctx* ctx);

/* ── pacing setup ──────────────────────────────────────────────────────── */
void ut_txv_set_frame_time(ut_txv_ctx* ctx, long double frame_time_ns);
void ut_txv_set_max_onward_epochs(ut_txv_ctx* ctx, uint32_t max_onward_epochs);
void ut_txv_set_cur_epochs(ut_txv_ctx* ctx, uint64_t cur_epochs);
void ut_txv_set_tr_offset(ut_txv_ctx* ctx, long double tr_offset_ns);
void ut_txv_set_vrx(ut_txv_ctx* ctx, uint32_t vrx);
void ut_txv_set_trs(ut_txv_ctx* ctx, long double trs_ns);
/* Toggle ST20_TX_FLAG_EXACT_USER_PACING on the session's ops.flags. */
void ut_txv_set_exact_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_USER_PACING on the session's ops.flags. */
void ut_txv_set_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Media (RTP) sampling clock rate, e.g. 90000 for video. */
void ut_txv_set_sampling_clock_rate(ut_txv_ctx* ctx, uint32_t sampling_rate);
/* Real TAI wall-clock cursor used as the source time for the non-user-
 * timestamp RTP derivation path in tv_update_rtp_time_stamp(). */
void ut_txv_set_ptp_time_cursor(ut_txv_ctx* ctx, uint64_t tai_ns);

/* Mocked time sources consumed by tv_sync_pacing() via mt_get_ptp_time()/
 * mt_get_tsc(). Take effect immediately; no auto-advance between calls. */
void ut_txv_set_mock_ptp_time(ut_txv_ctx* ctx, uint64_t ptp_ns);
void ut_txv_set_mock_tsc_time(ut_txv_ctx* ctx, uint64_t tsc_ns);

/* ── code under test ───────────────────────────────────────────────────── */
uint64_t ut_txv_calc_frame_count_since_epoch(ut_txv_ctx* ctx, uint64_t cur_tai,
                                             uint64_t required_tai);
/* Drives the mocked ptp/tsc values set above; required_tai as in the
 * production st20_tx_ops (0 = no user-supplied timestamp). */
int ut_txv_sync_pacing(ut_txv_ctx* ctx, uint64_t required_tai);
uint64_t ut_txv_pacing_required_tai(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp);
int ut_txv_run_frame_tasklet(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc,
                             uint64_t* packet_ptp);
int ut_txv_run_transmitter_boundary(ut_txv_ctx* ctx, enum ut_txv_pacing_way way,
                                    uint64_t delta_ns, int* bursts_before_target,
                                    int* bursts_at_target);
/* Drives tv_update_rtp_time_stamp() directly with the pacing state set up
 * above (ptp_time_cursor, sampling_clock_rate). */
void ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp);

/* ── accessors ─────────────────────────────────────────────────────────── */
uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx);
long double ut_txv_tsc_time_cursor(const ut_txv_ctx* ctx);
long double ut_txv_ptp_time_cursor(const ut_txv_ctx* ctx);
uint64_t ut_txv_tsc_time_frame_start(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_onward(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_drop(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_error_user_timestamp(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_mismatch(const ut_txv_ctx* ctx);
int ut_txv_notify_late_calls(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_late_last_delta(const ut_txv_ctx* ctx);
int ut_txv_get_next_frame_calls(const ut_txv_ctx* ctx);
int ut_txv_notify_frame_done_calls(const ut_txv_ctx* ctx);
uint16_t ut_txv_notify_frame_done_idx(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_frame_done_timestamp(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_frame_done_epoch(const ut_txv_ctx* ctx);
bool ut_txv_frame_is_waiting(const ut_txv_ctx* ctx);
int ut_txv_frame_refcnt(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_port_build(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_port_frames(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_exceed_frame_time(const ut_txv_ctx* ctx);
/* Result of the last ut_txv_update_rtp_time_stamp() call. */
uint32_t ut_txv_rtp_time_stamp(const ut_txv_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_TX_SESSION_HARNESS_H_ */
