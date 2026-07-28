/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the PTP servo / delta-adjustment unit tests.
 *
 * The harness wraps a single `struct mt_ptp_impl` driven entirely
 * in-process: `no_timesync` is forced true so every `rte_eth_timesync_*`
 * call is bypassed and replaced by the `no_timesync_delta` accumulator,
 * letting the PI servo and delta bookkeeping run with no NIC, no
 * hugepages, and no real PHC.
 *
 * Only an opaque typedef plus plain C declarations are exposed here so
 * the header is safe to include from the C++ gtest translation units; no
 * MTL-internal header is pulled in.
 */

#ifndef _UT_PTP_HARNESS_H_
#define _UT_PTP_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror of the production `enum servo_state` ordinals (UNLOCKED=0,
 * JUMP=1, LOCKED=2) so the C++ tests can assert state transitions
 * without including the internal enum. */
#define UT_PTP_SERVO_UNLOCKED 0
#define UT_PTP_SERVO_JUMP 1
#define UT_PTP_SERVO_LOCKED 2

typedef struct ut_ptp_ctx ut_ptp_ctx;

/* Initialise the shared DPDK EAL (idempotent). Returns 0 on success. */
int ut_ptp_init(void);

/* Create a zeroed PTP context (no_timesync=true, coefficient=1.0,
 * feature bit clear, phc2sys inactive). Caller frees with ut_ptp_destroy. */
ut_ptp_ctx* ut_ptp_create(void);
void ut_ptp_destroy(ut_ptp_ctx* ctx);

/* ── setters ──────────────────────────────────────────────────────────── */
void ut_ptp_set_phc2sys_active(ut_ptp_ctx* ctx, bool active);
void ut_ptp_set_t1(ut_ptp_ctx* ctx, uint64_t t1);
void ut_ptp_set_t2(ut_ptp_ctx* ctx, uint64_t t2);
void ut_ptp_set_t3(ut_ptp_ctx* ctx, uint64_t t3);
void ut_ptp_set_t4(ut_ptp_ctx* ctx, uint64_t t4);
/* Drive the async t3 read state directly. */
void ut_ptp_set_t3_sequence_id(ut_ptp_ctx* ctx, uint16_t seq);
void ut_ptp_set_t3_pending(ut_ptp_ctx* ctx, bool pending, uint16_t pending_seq);
void ut_ptp_set_t3_deadline_ns(ut_ptp_ctx* ctx, uint64_t deadline_ns);
/* Return value forced from the mocked rte_eth_timesync_read_tx_timestamp when
 * no_timesync=false (default 0). Negative simulates a not-yet-ready tx ts. */
void ut_ptp_set_read_tx_ret(int ret);
/* Timestamp (ns) the mocked read returns on success (default 0). */
void ut_ptp_set_read_tx_ns(uint64_t ns);
/* Default is true (NIC bypassed). Set false to route the production
 * rte_eth_timesync wrappers through the harness overrides. */
void ut_ptp_set_no_timesync(ut_ptp_ctx* ctx, bool no_timesync);
void ut_ptp_set_coefficient(ut_ptp_ctx* ctx, double coefficient);
void ut_ptp_set_last_sync_ts(ut_ptp_ctx* ctx, uint64_t last_sync_ts);

/* ── exercise the production code under test ──────────────────────────── */
void ut_ptp_adjust_delta(ut_ptp_ctx* ctx, int64_t delta, bool error_correct);
/* Run the static delay_req tx-timestamp (t3) alarm handler. */
void ut_ptp_run_t3_handler(ut_ptp_ctx* ctx);
/* Run the static single-owner completion attempt for the current cycle. */
void ut_ptp_try_complete_result(ut_ptp_ctx* ctx);

/* Direct wrapper over the static `pi_sample`. Operates on the context's
 * own servo. Returns ppb; writes the resulting servo state ordinal to
 * `*out_state` when non-NULL. */
double ut_pi_sample(ut_ptp_ctx* ctx, double offset, double local_ts, int* out_state);

/* Pure-function wrappers. */
uint64_t ut_ptp_correct_ts(ut_ptp_ctx* ctx, uint64_t ts);
/* Host-order seconds/ns are packed into a network-order tmstamp and fed
 * through the production `ptp_net_tmstamp_to_ns`. */
uint64_t ut_ptp_net_tmstamp_to_ns(uint16_t sec_msb, uint32_t sec_lsb, uint32_t ns);

/* ── getters ──────────────────────────────────────────────────────────── */
int64_t ut_ptp_no_timesync_delta(const ut_ptp_ctx* ctx);
int64_t ut_ptp_ptp_delta(const ut_ptp_ctx* ctx);
int64_t ut_ptp_stat_delta_min(const ut_ptp_ctx* ctx);
int64_t ut_ptp_stat_delta_max(const ut_ptp_ctx* ctx);
int32_t ut_ptp_stat_delta_cnt(const ut_ptp_ctx* ctx);
uint64_t ut_ptp_delta_result_cnt(const ut_ptp_ctx* ctx);
bool ut_ptp_locked(const ut_ptp_ctx* ctx);
uint16_t ut_ptp_stat_sync_keep(const ut_ptp_ctx* ctx);
int ut_ptp_servo_count(const ut_ptp_ctx* ctx);
double ut_ptp_servo_drift(const ut_ptp_ctx* ctx);
uint64_t ut_ptp_t3(const ut_ptp_ctx* ctx);
bool ut_ptp_t3_pending(const ut_ptp_ctx* ctx);
int32_t ut_ptp_stat_t3_timeout(const ut_ptp_ctx* ctx);
int32_t ut_ptp_result_claimed(const ut_ptp_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _UT_PTP_HARNESS_H_ */
