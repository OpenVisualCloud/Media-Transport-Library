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

/* Same as ut20_feed_frame_pkt() but with an explicit RTP sequence number,
 * for tests that need to drive seq independently of pkt_idx (e.g. wrap or
 * threshold tests). */
int ut20_feed_frame_pkt_seq(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq, uint32_t ts,
                            enum mtl_session_port port);

/* Feed every packet of one full frame on `port`, in pkt_idx order. */
void ut20_feed_full_frame(ut20_test_ctx* ctx, uint32_t ts, enum mtl_session_port port);

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

/* Session-wide counters. */
uint64_t ut20_stat_received(const ut20_test_ctx* ctx);
uint64_t ut20_stat_redundant(const ut20_test_ctx* ctx);
uint64_t ut20_stat_lost_pkts(const ut20_test_ctx* ctx);
uint64_t ut20_stat_no_slot(const ut20_test_ctx* ctx);
uint64_t ut20_stat_idx_oo_bitmap(const ut20_test_ctx* ctx);
uint64_t ut20_stat_frames_incomplete(const ut20_test_ctx* ctx);

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

#ifdef __cplusplus
}
#endif

#endif /* _ST20_SESSION_HARNESS_H_ */
