/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness API for the ST 2110-30 (audio) RX session unit tests.
 *
 * The harness wraps a single `st_rx_audio_session_impl` configured for a
 * tiny synthetic geometry. ST30 uses TIMESTAMP-only redundancy filtering
 * (no per-frame bitmap), which is reflected in the available accessors.
 *
 * Two feeder families are exposed:
 *
 *   ut30_feed_*                — call the per-packet handler directly.
 *                                Use to assert on filter/state behaviour.
 *   ut30_feed_*_via_wrapper    — call the public `_handle_mbuf` wrapper
 *                                the production tasklet uses. Use whenever
 *                                the test asserts on per-port `err_packets`
 *                                or `packets` accounting.
 */

#ifndef _ST30_SESSION_HARNESS_H_
#define _ST30_SESSION_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut30_test_ctx ut30_test_ctx;

/* Initialise the shared DPDK EAL and mempool. Idempotent — safe to call
 * once per gtest fixture SetUp(). Returns 0 on success, < 0 on failure. */
int ut30_init(void);

/* Create a context with `num_port` enabled (1 = single-port, 2 = redundant).
 * Caller owns the returned pointer and must free it with ut30_ctx_destroy().
 * Returns NULL on allocation failure. */
ut30_test_ctx* ut30_ctx_create(int num_port);
void ut30_ctx_destroy(ut30_test_ctx* ctx);

/* Feed one RFC 3550 audio packet directly to the per-packet handler.
 * Returns the production handler's return code (0 = accepted, < 0 = error
 * reason; e.g. -EIO for redundancy filtering). */
int ut30_feed_pkt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                  enum mtl_session_port port);

/* Convenience: feed `count` consecutive packets starting at `seq_start`,
 * all with the same `ts`. */
void ut30_feed_burst(ut30_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     enum mtl_session_port port);

/* Negative-test feeders: override one RTP field while keeping the others
 * at session defaults. */
int ut30_feed_pkt_pt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                     enum mtl_session_port port, uint8_t payload_type);
int ut30_feed_pkt_ssrc(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                       enum mtl_session_port port, uint32_t ssrc);
int ut30_feed_pkt_len(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                      enum mtl_session_port port, uint32_t payload_len);

/* Configure the session's expected RTP payload type / SSRC. Call after
 * ut30_ctx_create() and before feeding any packet. */
void ut30_ctx_set_pt(ut30_test_ctx* ctx, uint8_t pt);
void ut30_ctx_set_ssrc(ut30_test_ctx* ctx, uint32_t ssrc);

/* Wrapper feeder — drives the production `_handle_mbuf` wrapper instead of
 * the per-packet handler. Use this whenever the test asserts on per-port
 * `err_packets` or per-port `packets`. All RTP fields are explicit so a
 * single function covers happy-path and negative-path tests. */
int ut30_feed_pkt_via_wrapper(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                              enum mtl_session_port port, uint8_t pt, uint32_t ssrc,
                              uint32_t payload_len);

/* Per-port counter accessors. */
uint64_t ut30_stat_port_err_packets(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_pkts(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_bytes(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_lost(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_reordered(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_duplicates(const ut30_test_ctx* ctx, enum mtl_session_port port);

/* Session-wide counters. `unrecovered` counts gaps the redundancy filter
 * could not heal; `redundant` counts cross-port duplicates dropped by the
 * filter; `received` counts accepted packets after filtering. */
uint64_t ut30_stat_unrecovered(const ut30_test_ctx* ctx);
uint64_t ut30_stat_redundant(const ut30_test_ctx* ctx);
uint64_t ut30_stat_received(const ut30_test_ctx* ctx);
uint64_t ut30_stat_lost_pkts(const ut30_test_ctx* ctx);

/* Last accepted RTP sequence number (a.k.a. session "watermark") — used by
 * tests that need to set up a known starting point before injecting reorder
 * or wrap. */
int ut30_session_seq_id(const ut30_test_ctx* ctx);

/* Number of frames delivered since session reset (live value of the
 * stat counter). */
int ut30_frames_received(const ut30_test_ctx* ctx);

/* Number of audio packets that constitute one frame in this harness's
 * synthetic configuration. Tests use it to derive expected counts. */
int ut30_pkts_per_frame(const ut30_test_ctx* ctx);

/* Per-reason drop counters. */
uint64_t ut30_stat_wrong_pt(const ut30_test_ctx* ctx);
uint64_t ut30_stat_wrong_ssrc(const ut30_test_ctx* ctx);
uint64_t ut30_stat_len_mismatch(const ut30_test_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30_SESSION_HARNESS_H_ */
