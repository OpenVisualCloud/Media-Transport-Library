/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header shared between the C harness and C++ gtest file.
 * The test context is opaque — all internal struct access goes through
 * accessor functions so that C++ never includes internal MTL headers.
 */

#ifndef _ST40_RX_REDUNDANCY_HARNESS_H_
#define _ST40_RX_REDUNDANCY_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* opaque handle — defined only in the .c file */
typedef struct ut_test_ctx ut_test_ctx;

/** Initialise DPDK EAL, mempool, and ring. Call once before all tests. */
int ut_eal_init(void);

/** Allocate and initialise a test context.  num_port=2 for redundant. */
ut_test_ctx* ut_ctx_create(int num_port);

/** Free a test context. */
void ut_ctx_destroy(ut_test_ctx* ctx);

/** Drain all mbufs from the shared ring. */
void ut_drain_ring(void);

/** Feed one packet to the redundancy filter. */
int ut_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                enum mtl_session_port port);

/** Feed a burst of sequential packets (marker on last if last_marker). */
void ut_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                   int last_marker, enum mtl_session_port port);

/** Feed one packet with custom payload type. */
int ut_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                   enum mtl_session_port port, uint8_t payload_type);

/** Feed one packet with custom SSRC. */
int ut_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint32_t ssrc);

/** Feed one packet with custom F-bits (interlace field signaling). */
int ut_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port, uint8_t f_bits);

/** Set expected payload type on context (0 = disable check). */
void ut_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt);

/** Set expected SSRC on context (0 = disable check). */
void ut_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc);

/** Enable interlace auto-detect. */
void ut_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable);

/* ── stat accessors ───────────────────────────────────────────────── */

uint64_t ut_stat_unrecovered(const ut_test_ctx* ctx);
uint64_t ut_stat_redundant(const ut_test_ctx* ctx);
uint64_t ut_stat_received(const ut_test_ctx* ctx);
uint64_t ut_stat_out_of_order(const ut_test_ctx* ctx);
int ut_session_seq_id(const ut_test_ctx* ctx);

/* per-port stats */
uint64_t ut_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut_stat_port_ooo(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port);

/* validation stats */
uint64_t ut_stat_wrong_pt(const ut_test_ctx* ctx);
uint64_t ut_stat_wrong_ssrc(const ut_test_ctx* ctx);
uint64_t ut_stat_wrong_interlace(const ut_test_ctx* ctx);
uint64_t ut_stat_interlace_first(const ut_test_ctx* ctx);
uint64_t ut_stat_interlace_second(const ut_test_ctx* ctx);
uint64_t ut_stat_enqueue_fail(const ut_test_ctx* ctx);
int ut_frames_received(const ut_test_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST40_RX_REDUNDANCY_HARNESS_H_ */
