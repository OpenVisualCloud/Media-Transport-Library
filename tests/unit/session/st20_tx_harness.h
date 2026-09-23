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
void ut_txv_set_warm_pkts(ut_txv_ctx* ctx, uint32_t warm_pkts);
/* ops.interlaced: selects the ST 2110-21 6.2 field/frame-grid schedule. */
void ut_txv_set_interlaced(ut_txv_ctx* ctx, bool enable);
/* s->s_type: the ST22 shape st22_tx_create() gives an ST22_TYPE_RTP_LEVEL session.
 * st22_info stays NULL, which is what distinguishes it from ST22 frame level and
 * makes its field parity unreadable. */
void ut_txv_set_st22_rtp_level(ut_txv_ctx* ctx, bool enable);
/* s->second_field -- the parity tv_init_next_meta() hands the NEXT frame. The
 * tasklet paths read it from there, and production flips it after each frame. */
void ut_txv_set_second_field(ut_txv_ctx* ctx, bool second_field);
/* Toggle ST20_TX_FLAG_EXACT_USER_PACING on the session's ops.flags. */
void ut_txv_set_exact_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_USER_PACING on the session's ops.flags. */
void ut_txv_set_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_USER_TIMESTAMP on the session's ops.flags: the RTP
 * timestamp is taken verbatim from the app-supplied timestamp instead of the
 * pacing-scheduled instant. Selects the branch of tv_update_rtp_time_stamp()
 * whose returned "reporting" TAI instant must match the RTP timestamp it
 * derived. */
void ut_txv_set_user_timestamp(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH on the session's ops.flags: the RTP
 * timestamp is derived from the bare epoch, omitting tr_offset. Added to pin
 * that this basis (not pacing->ptp_time_cursor) is also what gets reported
 * back as frame->timestamp. */
void ut_txv_set_rtp_timestamp_epoch(ut_txv_ctx* ctx, bool enable);
/* Set ops.rtp_timestamp_delta_us, added to the RTP-timestamp source instant
 * (never to the actual TX schedule). Added to pin that the reported
 * frame->timestamp includes this delta too, so it still reconstructs
 * frame->rtp_timestamp. */
void ut_txv_set_rtp_timestamp_delta_us(ut_txv_ctx* ctx, int32_t delta_us);
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
/* Drives tv_tasklet_rtp() with one app packet whose st20_rfc4175_rtp_hdr row
 * number carries the given field parity, so the production builders' F-bit
 * extraction is what tv_sync_pacing() is told. tx_no_chain selects tv_build_rtp()
 * over tv_build_rtp_chain(). Sets ops.type to ST20_TYPE_RTP_LEVEL; combine with
 * ut_txv_set_st22_rtp_level() for the ST22 shape. Outputs pacing->cur_epochs, and
 * fails if no packet reached the TX ring. Returns 0 on success. */
int ut_txv_run_rtp_tasklet(ut_txv_ctx* ctx, bool second_field, bool tx_no_chain,
                           uint64_t* epoch);
/* Drives tv_tasklet_st22()'s frame->tx_st22_meta assignment -- the compressed-
 * video (ST22) mirror of tv_tasklet_frame()'s frame->tv_meta assignment above,
 * so both production call sites are held to the same timestamp/rtp_timestamp
 * contract, not just the uncompressed-video one. tv_tasklet_st22() returns
 * before building any packet, so this needs only a ring (for its
 * rte_ring_full() guard), no mempool or packet payload setup. Outputs the
 * frame's timestamp/rtp_timestamp directly rather than routing through a
 * notify_frame_done callback, since st22_tx_video_info has no equivalent mock
 * plumbing in this harness yet. */
int ut_txv_run_st22_next_frame_step(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp, uint64_t* frame_timestamp,
                                    uint32_t* frame_rtp_timestamp);
int ut_txv_run_transmitter_boundary(ut_txv_ctx* ctx, enum ut_txv_pacing_way way,
                                    uint64_t delta_ns, int* bursts_before_target,
                                    int* bursts_at_target);
/* Drives tv_update_rtp_time_stamp() directly with the pacing state set up
 * above (ptp_time_cursor, sampling_clock_rate). */
void ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp);
/* Drives the real tv_init_pacing() -- the only place pacing->tr_offset is derived
 * for a live session. Forces TSC pacing so no NIC queue or rate-limiter training
 * is needed. Returns tv_init_pacing()'s status; read the result with
 * ut_txv_pacing_tr_offset() below. */
int ut_txv_init_pacing(ut_txv_ctx* ctx, uint32_t height, bool interlaced,
                       enum st_fps fps);
/* Drives st_tai_round_to_media_clk_ns() -- the same media-clock snap
 * tv_sync_pacing() applies to the scheduled instant, so tests can express an
 * expectation as the raw ST 2110-21 sum instead of a pre-rounded constant. */
uint64_t ut_txv_round_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate);

/* ── session-owned mempool release (tv_mempool_free) ──────────────────── */
/* Install a private header mempool on port P, as tv_mempool_init() would.
 * Returns 0 on success. ut_txv_destroy() releases whatever is left over. */
int ut_txv_install_hdr_mempool(ut_txv_ctx* ctx);
/* Take one mbuf out of the installed pool, mimicking an mbuf the transmitter
 * still holds. Returns 0 on success. */
int ut_txv_hold_hdr_mbuf(ut_txv_ctx* ctx);
void ut_txv_release_hdr_mbuf(ut_txv_ctx* ctx);
/* Mark the installed pool as borrowed rather than session-owned. */
void ut_txv_set_tx_mono_pool(ut_txv_ctx* ctx, bool enable);
void ut_txv_set_hdr_mempool_reuse_rx(ut_txv_ctx* ctx, bool enable);
/* Drives tv_mempool_free(): 0 when every pool was released, < 0 otherwise. */
int ut_txv_mempool_free(ut_txv_ctx* ctx);
/* Whether the session still points at the pool. */
bool ut_txv_hdr_mempool_installed(const ut_txv_ctx* ctx);
/* Whether the pool itself still exists, independent of the session's pointer. */
bool ut_txv_hdr_mempool_alive(const ut_txv_ctx* ctx);

/* ── accessors ─────────────────────────────────────────────────────────── */
uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx);
long double ut_txv_pacing_tr_offset(const ut_txv_ctx* ctx);
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
/* Parity the session actually scheduled, as the app sees it in notify_frame_done(). */
bool ut_txv_notify_frame_done_second_field(const ut_txv_ctx* ctx);
/* The frame->rtp_timestamp the app would see in notify_frame_done(); paired
 * with ut_txv_notify_frame_done_timestamp() above to check that one
 * reconstructs the other via st10_tai_to_media_clk(). */
uint32_t ut_txv_notify_frame_done_rtp_timestamp(const ut_txv_ctx* ctx);
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
