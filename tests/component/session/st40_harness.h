/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness API for the ST 2110-40 (ancillary) RX session unit tests.
 *
 * The harness wraps a single `st_rx_ancillary_session_impl`. Accepted
 * packets are placed onto a real DPDK ring (so the production frame-assembly
 * code can dequeue them); the ring is drained between feeds by default and
 * can be inspected directly via the `ut40_drain_paused` RAII guard plus
 * `ut40_ring_dequeue_markers()`.
 *
 * Two feeder families are exposed:
 *
 *   ut40_feed_*                — call the per-packet handler directly.
 *                                Use to assert on filter/state behaviour.
 *   ut40_feed_*_via_wrapper    — call the public `_handle_mbuf` wrapper
 *                                the production tasklet uses. Use whenever
 *                                the test asserts on per-port `err_packets`
 *                                or `packets` accounting.
 */

#ifndef _ST40_SESSION_HARNESS_H_
#define _ST40_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_test_ctx ut_test_ctx;

/* Initialise the shared DPDK EAL, mempool, and ANC ring. Idempotent —
 * safe to call once per gtest fixture SetUp(). Returns 0 on success. */
int ut40_init(void);

/* Create a context with `num_port` enabled (1 = single-port, 2 = redundant).
 * Caller owns the returned pointer and must free it with ut40_ctx_destroy(). */
ut_test_ctx* ut40_ctx_create(int num_port);
void ut40_ctx_destroy(ut_test_ctx* ctx);

/* Drain every accepted packet currently on the ANC ring. The harness
 * normally drains automatically inside each feeder; call this explicitly
 * after `ut40_drain_paused` scope ends, or in TearDown() to reset shared
 * state between tests. */
void ut40_drain_ring(void);

/* Build and feed one RFC 8331 ancillary packet directly to the per-packet
 * handler. `marker` sets the RTP marker bit (1 = end of frame, 0 = mid).
 * Returns the production handler's return code (0 = accepted, < 0 = error
 * reason; e.g. -EIO for redundancy filtering). */
int ut40_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                  enum mtl_session_port port);

/* Convenience: feed `count` consecutive packets starting at `seq_start`,
 * all with the same `ts`. The marker bit is set only on the last packet
 * if `last_marker` is non-zero. */
void ut40_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     int last_marker, enum mtl_session_port port);

/* Negative-test feeders: override one RTP / payload field while keeping
 * the others at session defaults. */
int ut40_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint8_t payload_type);
int ut40_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port, uint32_t ssrc);
int ut40_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                        enum mtl_session_port port, uint8_t f_bits);

/* Designated-initializer-friendly packet description for ut40_feed_spec().
 * Fields are in the order tests overwhelmingly use; defaults (0) match the
 * thin wrappers above so callers only set what they want to override. */
struct ut40_pkt_spec {
  uint16_t seq;
  uint32_t ts;
  int marker;
  enum mtl_session_port port;
  uint8_t payload_type; /* 0 = use session default */
  uint32_t ssrc;        /* 0 = use session default */
  uint8_t f_bits;       /* 0 = progressive */
};

/* Feed a packet described by `spec` directly to the per-packet handler.
 * Equivalent to the typed feeders above but lets a test override several
 * fields in one call without an explosion of parameters. */
int ut40_feed_spec(ut_test_ctx* ctx, struct ut40_pkt_spec spec);

/* Configure the session's expected RTP fields. Call after ut40_ctx_create()
 * and before feeding any packet. */
void ut40_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt);
void ut40_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc);

/* Toggle interlace auto-detection. When enabled the session derives the
 * field bit from the first interlaced packet seen; when disabled the
 * session enforces its configured value. */
void ut40_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable);

/* Wrapper feeder — drives the production `_handle_mbuf` wrapper instead of
 * the per-packet handler. Use this whenever the test asserts on per-port
 * `err_packets` or per-port `packets`. */
int ut40_feed_spec_via_wrapper(ut_test_ctx* ctx, struct ut40_pkt_spec spec);

/* Per-port counter accessors. */
uint64_t ut40_stat_port_err_packets(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_lost(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_reordered(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_duplicates(const ut_test_ctx* ctx, enum mtl_session_port port);

/* Session-wide counters. `unrecovered` counts gaps the redundancy filter
 * could not heal; `redundant` counts cross-port duplicates dropped by the
 * filter; `received` counts accepted packets after filtering. */
uint64_t ut40_stat_unrecovered(const ut_test_ctx* ctx);
uint64_t ut40_stat_redundant(const ut_test_ctx* ctx);
uint64_t ut40_stat_received(const ut_test_ctx* ctx);
uint64_t ut40_stat_lost_pkts(const ut_test_ctx* ctx);

/* Last accepted RTP sequence number (a.k.a. session "watermark"). */
int ut40_session_seq_id(const ut_test_ctx* ctx);

/* Internal field-bit mismatch counter, bumped when ports disagree on
 * the F-bit value within the same timestamp. */
uint64_t ut40_stat_field_bit_mismatch(const ut_test_ctx* ctx);

/* Per-reason drop counters. */
uint64_t ut40_stat_wrong_pt(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_ssrc(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_interlace(const ut_test_ctx* ctx);

/* Per-field interlace counters (incremented when the session accepts a
 * packet for the corresponding field of an interlaced frame). */
uint64_t ut40_stat_interlace_first(const ut_test_ctx* ctx);
uint64_t ut40_stat_interlace_second(const ut_test_ctx* ctx);

/* Counts ring-enqueue failures (e.g. ring full) — useful when validating
 * back-pressure paths. */
uint64_t ut40_stat_enqueue_fail(const ut_test_ctx* ctx);

/* Number of frames delivered since session reset. */
int ut40_frames_received(const ut_test_ctx* ctx);

/* Pause or resume the auto-drain that runs inside every feeder. While
 * paused, accepted packets accumulate on the ring so the test can inspect
 * them with ut40_ring_dequeue_markers(). Prefer the `ut40_drain_paused`
 * RAII guard below over calling this directly. */
void ut40_set_skip_drain(bool skip);

/* Reset the session's per-packet state (sequence watermark, prev_tmstamp,
 * threshold counters) without destroying the context. Use to start a fresh
 * sub-scenario inside a test. */
void ut40_session_reset(ut_test_ctx* ctx);

/* Dequeue every packet currently on the ANC ring, counting them and
 * reporting whether any of them carried the RTP marker bit.
 *   *out_count       — total number of packets dequeued.
 *   *out_has_marker  — true iff at least one of them had marker == 1.
 * Returns 0 on success, < 0 if the ring is not initialised.
 *
 * Typical use:
 *   {
 *     ut40_drain_paused guard;
 *     // ... feed packets ...
 *     int n; bool m;
 *     ut40_ring_dequeue_markers(&n, &m);
 *     EXPECT_TRUE(m);
 *   }
 */
int ut40_ring_dequeue_markers(int* out_count, bool* out_has_marker);

#ifdef __cplusplus
}

/* RAII guard: pause ring drain for the lifetime of the guard. Use in tests
 * that need to inspect queued packets directly. Restores the previous state
 * on scope exit (including via gtest assertion failures). */
class ut40_drain_paused {
 public:
  ut40_drain_paused() {
    ut40_set_skip_drain(true);
  }
  ~ut40_drain_paused() {
    ut40_set_skip_drain(false);
  }
  ut40_drain_paused(const ut40_drain_paused&) = delete;
  ut40_drain_paused& operator=(const ut40_drain_paused&) = delete;
};

extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ST40_SESSION_HARNESS_H_ */
