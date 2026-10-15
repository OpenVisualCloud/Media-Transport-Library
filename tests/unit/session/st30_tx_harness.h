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
/* ptime, sampling and their samples per packet, then tx_audio_session_init_pacing() */
int ut_txa30_set_ptime(ut_txa30_ctx* ctx, enum st30_ptime ptime,
                       enum st30_sampling sampling);
void ut_txa30_set_cur_epochs(ut_txa30_ctx* ctx, uint64_t cur_epochs);
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

/* tx_audio_session_sync_pacing() without the TSC delay to the epoch */
int ut_txa30_sync_pacing(ut_txa30_ctx* ctx, uint64_t required_tai);

/* One frame-tasklet build of a chained two-port session whose R header alloc fails,
 * reporting the mbufs its pool still has in use. Returns 0 on success. */
int ut_txa30_build_chain_r_dry(ut_txa30_ctx* ctx, unsigned int* pool_in_use);

/* ── accessors ─────────────────────────────────────────────────────────── */
uint64_t ut_txa30_cur_epochs(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_ptp_time_cursor(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_stat_error_user_timestamp(const ut_txa30_ctx* ctx);
int ut_txa30_notify_frame_done_calls(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_notify_frame_done_timestamp(const ut_txa30_ctx* ctx);
uint32_t ut_txa30_notify_frame_done_rtp_timestamp(const ut_txa30_ctx* ctx);
/* RTP header timestamps (host order), one per packet built by
 * ut_txa30_run_frame_tasklet(), in build order -- what actually went out on
 * the wire, independent of what notify_frame_done() was told. */
int ut_txa30_wire_rtp_timestamp_count(const ut_txa30_ctx* ctx);
uint32_t ut_txa30_wire_rtp_timestamp(const ut_txa30_ctx* ctx, int i);

/* RL profiling against a modeled queue of nb_tx_desc descriptors that drains one
 * descriptor per trs / (1 + pads_per_st30_pkt), the trained rate; the mock clock
 * moves only while a full queue holds a busy burst, or in mt_sleep_us() */
int ut_txa30_setup_rl(ut_txa30_ctx* ctx, long double trs_ns, uint16_t nb_tx_desc);
void ut_txa30_teardown_rl(ut_txa30_ctx* ctx);
/* tx_audio_session_profiling_rl_bps() of the first RL queue; 0 on failure */
uint64_t ut_txa30_profile_rl(ut_txa30_ctx* ctx, uint64_t initial_bytes_per_sec);
/* how long the modeled queue still drains what is queued, from the mock clock */
uint64_t ut_txa30_rl_queued_ns(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_slept_ns(const ut_txa30_ctx* ctx);

/* ── session and transmitter over modeled NIC rings ───────────────────── */
enum { UT_TXA30_PIPE_HOLD_MAX = 1024, UT_TXA30_PIPE_CHAIN = 2 };
enum ut_txa30_pipe_pacing {
  UT_TXA30_PIPE_TSC_SHARED, /* the manager ring and audio transmitter, the default */
  UT_TXA30_PIPE_TSC_DEDICATED,
  UT_TXA30_PIPE_RL, /* ST30_TX_RL_QUEUES_USED queues a leg, 3 pads a pkt */
};
/* A frame-level PCM24 stereo 48 kHz session of 10-pkt frames on num_port legs, leg i on
 * port i whose TX queues are common/ut_tx_ring.h rings of nb_tx_desc[i] (rs 32, the
 * PMD's free_thresh), hung or draining a descriptor a ns. The production session tasklet
 * and, when shared, the audio transmitter run on the mock clocks. */
struct ut_txa30_pipe_case {
  enum st30_ptime ptime;
  enum ut_txa30_pipe_pacing pacing;
  int num_port;
  bool copy;
  uint16_t nb_tx_desc[2];
  int ring_mode; /* enum ut_txr_mode */
  uint16_t free_thresh;
  bool hung[2];
  /* exact header pool of each leg and chain pool; tx_audio_session_mempool_init()'s
   * when 0 */
  unsigned int hdr_pool[2];
  unsigned int chain_pool;
  /* mbufs taken out of each exact pool first, as other lcores' caches keep them */
  unsigned int hold;
};
int ut_txa30_pipe_setup(ut_txa30_ctx* ctx, const struct ut_txa30_pipe_case* c);
/* the leg's rings complete no descriptor posted from now on */
void ut_txa30_pipe_hang(ut_txa30_ctx* ctx, int leg);
/* stops at the first failed pkt alloc or refused burst */
void ut_txa30_pipe_run(ut_txa30_ctx* ctx, uint64_t duration_ns, uint64_t step_ns);
/* -EBUSY if a pool still had mbufs out once the rings and inflights were freed */
int ut_txa30_pipe_teardown(ut_txa30_ctx* ctx);
/* the run step a pkt alloc first failed and a NIC ring first refused a pkt; 0 for none */
uint64_t ut_txa30_pipe_first_alloc_fail(const ut_txa30_ctx* ctx);
uint64_t ut_txa30_pipe_first_burst_fail(const ut_txa30_ctx* ctx);
unsigned int ut_txa30_pipe_trans_ring_thresh(const ut_txa30_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30_TX_SESSION_HARNESS_H_ */
