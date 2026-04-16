/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST20 (video) RX redundancy unit tests.
 * Opaque context — C++ never includes internal MTL headers.
 */

#ifndef _ST20_RX_REDUNDANCY_HARNESS_H_
#define _ST20_RX_REDUNDANCY_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20_test_ctx ut20_test_ctx;

/** Initialise DPDK EAL (shared with other harnesses — idempotent). */
int ut20_eal_init(void);

/** Allocate and initialise a test context. num_port=2 for redundant. */
ut20_test_ctx* ut20_ctx_create(int num_port);

/** Free a test context. */
void ut20_ctx_destroy(ut20_test_ctx* ctx);

/**
 * Feed one video packet with full control over RTP fields.
 * seq: 32-bit sequence id (base + ext).
 * ts:  RTP timestamp.
 * line_num, line_offset, line_length: RFC 4175 row header fields.
 */
int ut20_feed_pkt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                  uint16_t line_offset, uint16_t line_length, enum mtl_session_port port);

/**
 * Feed one packet by logical packet index within a frame.
 * For the test geometry (16x2 YUV422-10bit, 2 pkts/frame):
 *   pkt_idx 0 → line 0, offset 0, length 40
 *   pkt_idx 1 → line 1, offset 0, length 40
 * seq is auto-assigned: seq_base + pkt_idx, where seq_base = ts * 2 (arbitrary).
 */
int ut20_feed_frame_pkt(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                        enum mtl_session_port port);

/**
 * Feed one packet by pkt_idx with an explicit 32-bit seq id.
 * line/offset/length are computed from pkt_idx as in ut20_feed_frame_pkt.
 */
int ut20_feed_frame_pkt_seq(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq, uint32_t ts,
                            enum mtl_session_port port);

/** Feed a complete frame (all pkts) from one port with auto-assigned seq ids. */
void ut20_feed_full_frame(ut20_test_ctx* ctx, uint32_t ts, enum mtl_session_port port);

/** Feed one video packet with custom payload type. */
int ut20_feed_pkt_pt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                     uint16_t line_offset, uint16_t line_length,
                     enum mtl_session_port port, uint8_t pt);

/** Feed one video packet with custom SSRC. */
int ut20_feed_pkt_ssrc(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                       uint16_t line_offset, uint16_t line_length,
                       enum mtl_session_port port, uint32_t ssrc);

/** Set expected payload type (0 = disable check). */
void ut20_ctx_set_pt(ut20_test_ctx* ctx, uint8_t pt);

/** Set expected SSRC (0 = disable check). */
void ut20_ctx_set_ssrc(ut20_test_ctx* ctx, uint32_t ssrc);

/* ── stat accessors ───────────────────────────────────────────────── */

uint64_t ut20_stat_received(const ut20_test_ctx* ctx);
uint64_t ut20_stat_redundant(const ut20_test_ctx* ctx);
uint64_t ut20_stat_out_of_order(const ut20_test_ctx* ctx);
uint64_t ut20_stat_no_slot(const ut20_test_ctx* ctx);
uint64_t ut20_stat_idx_oo_bitmap(const ut20_test_ctx* ctx);
uint64_t ut20_stat_frames_dropped(const ut20_test_ctx* ctx);
int ut20_frames_received(const ut20_test_ctx* ctx);

/* validation stats */
uint64_t ut20_stat_wrong_pt(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_ssrc(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_interlace(const ut20_test_ctx* ctx);
uint64_t ut20_stat_offset_dropped(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_len(const ut20_test_ctx* ctx);

/** Total packets per frame in the test geometry. */
int ut20_total_frame_pkts(void);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_RX_REDUNDANCY_HARNESS_H_ */
