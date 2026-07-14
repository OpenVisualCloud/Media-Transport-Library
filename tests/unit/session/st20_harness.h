/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness API for the ST 2110-20 (video) RX session unit tests.
 *
 * The harness wraps a single `st_rx_video_session_impl` configured for a
 * tiny synthetic geometry (16x2 YUV422-10bit, 2 packets per frame). All
 * MTL internal types are kept opaque so the C++ test layer never includes
 * libmtl headers.
 *
 * Two feeder families are exposed:
 *
 *   ut20_feed_*                — call the per-packet handler directly.
 *                                Use to assert on filter/state behaviour.
 *   ut20_feed_*_via_wrapper    — call the public `_handle_mbuf` wrapper
 *                                the production tasklet uses. Use whenever
 *                                the test asserts on per-port `err_packets`
 *                                or `packets` accounting.
 */

#ifndef _ST20_SESSION_HARNESS_H_
#define _ST20_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20_test_ctx ut20_test_ctx;

/* Initialise the shared DPDK EAL and mempool. Idempotent — safe to call
 * once per gtest fixture SetUp(). Returns 0 on success, < 0 on failure. */
int ut20_init(void);

/* Create a context with `num_port` enabled (1 = single-port, 2 = redundant).
 * Caller owns the returned pointer and must free it with ut20_ctx_destroy().
 * Returns NULL on allocation failure. */
ut20_test_ctx* ut20_ctx_create(int num_port);

/* Same as ut20_ctx_create() but lets the test pick how many packets make
 * up one full frame. The harness keeps width=16 fixed and varies height to
 * yield `pkts_per_frame` packets/frame (one packet covers exactly one row).
 * Valid range: 1..32. Returns NULL on out-of-range or allocation failure.
 *
 * Default `ut20_ctx_create(num_port)` == `ut20_ctx_create_geom(num_port, 2)`.
 * Use a larger value when the test needs to exercise gap detection,
 * intra-frame reorder, or any code path conditioned on more than 2 packets
 * per frame (e.g. `pkt_idx > slot->last_pkt_idx + 1`). */
ut20_test_ctx* ut20_ctx_create_geom(int num_port, int pkts_per_frame);

void ut20_ctx_destroy(ut20_test_ctx* ctx);

/* Build and feed one RFC 4175 video packet directly to the per-packet
 * handler. `seq` is the RTP sequence number, `ts` the RTP timestamp,
 * `line_num/line_offset/line_length` describe the row covered.
 * Returns the production handler's return code (0 = accepted, < 0 = error
 * reason; see `rv_handle_frame_pkt`). */
int ut20_feed_pkt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                  uint16_t line_offset, uint16_t line_length, enum mtl_session_port port);

/* Convenience: feed packet `pkt_idx` (0 .. UT20_PKTS_PER_FRAME-1) of a
 * frame at timestamp `ts`. Auto-derives line_num/offset/length and uses
 * `pkt_idx` as the RTP sequence number (no per-port sequence tracking). */
int ut20_feed_frame_pkt(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                        enum mtl_session_port port);

/* Same as ut20_feed_frame_pkt() but stamps the mbuf's HW RX-timestamp
 * dynfield with `hw_raw_ns` before feeding it. Requires a prior
 * ut20_ctx_enable_hw_timestamp(). */
int ut20_feed_frame_pkt_hw_ts(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                              enum mtl_session_port port, uint64_t hw_raw_ns);

/* Same as ut20_feed_frame_pkt() but with an explicit RTP sequence number,
 * for tests that need to drive seq independently of pkt_idx (e.g. wrap or
 * threshold tests). */
int ut20_feed_frame_pkt_seq(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq, uint32_t ts,
                            enum mtl_session_port port);

/* Feed every packet of one full frame on `port`, in pkt_idx order. */
void ut20_feed_full_frame(ut20_test_ctx* ctx, uint32_t ts, enum mtl_session_port port);

/* Switch the session into RTP-passthrough mode (ST20_TYPE_RTP_LEVEL): allocate
 * the rtps_ring, install a no-op notify_rtp_ready, and route packets through
 * rv_handle_rtp_pkt. Call once after ut20_ctx_create*() and before feeding. */
void ut20_ctx_enable_rtp(ut20_test_ctx* ctx);

/* Feed packet `pkt_idx` of an RTP-mode frame at timestamp `ts` with explicit
 * sequence `seq`, driving rv_handle_rtp_pkt directly. Requires a prior
 * ut20_ctx_enable_rtp(). */
int ut20_feed_rtp_pkt(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq, uint32_t ts,
                      enum mtl_session_port port);

/* Feed one packet with an overridden RTP payload type (`pt`) — for negative
 * tests that assert PT validation. All other fields as ut20_feed_pkt(). */
int ut20_feed_pkt_pt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                     uint16_t line_offset, uint16_t line_length,
                     enum mtl_session_port port, uint8_t pt);

/* Feed one packet with an overridden RTP SSRC — for SSRC-validation tests. */
int ut20_feed_pkt_ssrc(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                       uint16_t line_offset, uint16_t line_length,
                       enum mtl_session_port port, uint32_t ssrc);

/* Configure the session's expected RTP payload type / SSRC. Call after
 * ut20_ctx_create() and before feeding any packet. */
void ut20_ctx_set_pt(ut20_test_ctx* ctx, uint8_t pt);
void ut20_ctx_set_ssrc(ut20_test_ctx* ctx, uint32_t ssrc);

/* Force a session port's physical link up/down. A link-down port is a dead
 * wire: it never receives data and is not charged per-port loss at frame
 * finalisation (see rv_slot_account_per_port_loss). */
void ut20_set_port_down(ut20_test_ctx* ctx, enum mtl_session_port port, bool down);

/* Enable the HW RX-timestamp offload path on `port`: registers the DPDK
 * dynfield, installs an identity-mapped PTP correction, and sets
 * MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP so mt_mbuf_time_stamp() reads the
 * mbuf dynfield instead of falling back to the software PTP clock. */
void ut20_ctx_enable_hw_timestamp(ut20_test_ctx* ctx, enum mtl_session_port port);

/* timestamp_first_pkt captured off the most recent delivered frame's meta. */
uint64_t ut20_last_timestamp_first_pkt(const ut20_test_ctx* ctx);

/* Wrapper feeders — drive the production `_handle_mbuf` wrapper instead
 * of the per-packet handler. Use these (not the direct feeders above)
 * whenever the test asserts on per-port `err_packets` or per-port
 * `packets`, since those counters live in the wrapper and not the
 * per-packet handler.
 * `pt`/`ssrc` are sent on the wire (use the session-configured values
 * to test the happy path; pass mismatched values to test error paths). */
int ut20_feed_pkt_via_wrapper(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts,
                              uint16_t line_num, uint16_t line_offset,
                              uint16_t line_length, enum mtl_session_port port,
                              uint8_t pt, uint32_t ssrc);
int ut20_feed_frame_pkt_via_wrapper(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                                    enum mtl_session_port port);

/* Per-port counter accessors (live inside `port_user_stats.common.port[]`). */
uint64_t ut20_stat_port_err_packets(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_port_packets(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_port_reordered(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_port_lost(const ut20_test_ctx* ctx, enum mtl_session_port port);

/* Per-port frame accounting (frame-mode ST20 only).
 *
 *   port[i].frames        — incremented per-port iff that port delivered every
 *                           packet of the completed frame on its own (i.e.
 *                           `pkts_recv_per_port[i] >= pkts_received`). With
 *                           cross-port reconstruction NEITHER port is
 *                           credited; the session-wide `stat_frames_received`
 *                           still bumps. See `rv_frame_notify` in
 *                           `lib/src/st2110/st_rx_video_session.c`.
 *   frames_partial[i]     — incremented per-port iff the completed frame
 *                           needed packets from the OTHER port to fill in.
 *                           Together with `port[i].frames` it sums to
 *                           `stat_frames_received` per redundant session.
 */
uint64_t ut20_stat_port_frames(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_frames_partial(const ut20_test_ctx* ctx, enum mtl_session_port port);

/* Session-wide counters. */
uint64_t ut20_stat_received(const ut20_test_ctx* ctx);
uint64_t ut20_stat_redundant(const ut20_test_ctx* ctx);
uint64_t ut20_stat_lost_pkts(const ut20_test_ctx* ctx);
uint64_t ut20_stat_no_slot(const ut20_test_ctx* ctx);
uint64_t ut20_stat_slot_get_frame_fail(const ut20_test_ctx* ctx);

/* Suppress the harness refcnt-dec in notify_frame_ready to simulate an
 * application that stops calling st20_rx_put_framebuff. The
 * hold→release transition drains the withheld refcnts. */
void ut20_set_hold_frames(ut20_test_ctx* ctx, bool hold);

/* Bump stat_pkts_no_slot by `n` without touching stat_pkts_pool_empty —
 * simulates a non-back-pressure no_slot bump path (e.g. past-tmstamp
 * drop, DMA-busy drop) so tests can prove the warn line's pkts number
 * is driven by stat_pkts_pool_empty alone, not the wider no_slot total. */
void ut20_bump_pkts_no_slot_past_ts(ut20_test_ctx* ctx, uint64_t n);

/* Live value of the segregated pool-empty packet counter that drives the
 * back-pressure warn line's pkts number. */
uint64_t ut20_stat_pkts_pool_empty(const ut20_test_ctx* ctx);
uint64_t ut20_stat_idx_oo_bitmap(const ut20_test_ctx* ctx);
uint64_t ut20_stat_frames_incomplete(const ut20_test_ctx* ctx);
uint64_t ut20_stat_pkts_unrecovered(const ut20_test_ctx* ctx);

/* Invoke the production rv_stat() against the harness session without
 * standing up an mt_stat thread. Tests assert on its real log output. */
void ut20_invoke_rv_stat(ut20_test_ctx* ctx);

/* Number of frames the production handler has marked completed/delivered
 * since session reset. Returns the live value of `stat_frames_received`. */
int ut20_frames_received(const ut20_test_ctx* ctx);

/* Per-reason drop counters (each rejection bumps exactly one of these). */
uint64_t ut20_stat_wrong_pt(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_ssrc(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_interlace(const ut20_test_ctx* ctx);
uint64_t ut20_stat_offset_dropped(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_len(const ut20_test_ctx* ctx);

/* Fixed geometry constant: number of packets that make up one full frame
 * in the harness's synthetic 16x2 YUV422-10bit configuration. Tests use it
 * to derive expected counts without hard-coding the value. */
int ut20_total_frame_pkts(void);

/* Live geometry: number of packets per frame for THIS context. Returns
 * the value passed to ut20_ctx_create_geom() (or 2 for ut20_ctx_create()). */
int ut20_pkts_per_frame(const ut20_test_ctx* ctx);

/* Drive the production detach-time flush of any slot still holding a deferred
 * per-port deficit (test-only). Exercises `rv_flush_pending_loss`, the path the
 * production `rv_detach` runs before its final stat dump. */
void ut20_session_detach(ut20_test_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_SESSION_HARNESS_H_ */
