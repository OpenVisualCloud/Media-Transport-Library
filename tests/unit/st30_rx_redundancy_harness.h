/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST30 (audio) RX redundancy unit tests.
 * Opaque context — C++ never includes internal MTL headers.
 */

#ifndef _ST30_RX_REDUNDANCY_HARNESS_H_
#define _ST30_RX_REDUNDANCY_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut30_test_ctx ut30_test_ctx;

/** Initialise DPDK EAL (shared with ST40 harness — idempotent). */
int ut30_eal_init(void);

/** Allocate and initialise a test context. num_port=2 for redundant. */
ut30_test_ctx* ut30_ctx_create(int num_port);

/** Free a test context. */
void ut30_ctx_destroy(ut30_test_ctx* ctx);

/** Feed one audio packet. payload_len bytes of zeroes appended as audio data. */
int ut30_feed_pkt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                  enum mtl_session_port port);

/** Feed a burst of sequential packets. */
void ut30_feed_burst(ut30_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     enum mtl_session_port port);

/** Feed one audio packet with custom payload type. */
int ut30_feed_pkt_pt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                     enum mtl_session_port port, uint8_t payload_type);

/** Feed one audio packet with custom SSRC. */
int ut30_feed_pkt_ssrc(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                       enum mtl_session_port port, uint32_t ssrc);

/** Feed one audio packet with custom payload length (wrong size test). */
int ut30_feed_pkt_len(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                      enum mtl_session_port port, uint32_t payload_len);

/** Set expected payload type (0 = disable check). */
void ut30_ctx_set_pt(ut30_test_ctx* ctx, uint8_t pt);

/** Set expected SSRC (0 = disable check). */
void ut30_ctx_set_ssrc(ut30_test_ctx* ctx, uint32_t ssrc);

/* ── stat accessors ───────────────────────────────────────────────── */

uint64_t ut30_stat_unrecovered(const ut30_test_ctx* ctx);
uint64_t ut30_stat_redundant(const ut30_test_ctx* ctx);
uint64_t ut30_stat_received(const ut30_test_ctx* ctx);
uint64_t ut30_stat_out_of_order(const ut30_test_ctx* ctx);
int ut30_session_seq_id(const ut30_test_ctx* ctx);
int ut30_frames_received(const ut30_test_ctx* ctx);

/** How many packets make up one audio frame. */
int ut30_pkts_per_frame(const ut30_test_ctx* ctx);

/* per-port stats */
uint64_t ut30_stat_port_pkts(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_bytes(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_ooo(const ut30_test_ctx* ctx, enum mtl_session_port port);

/* validation stats */
uint64_t ut30_stat_wrong_pt(const ut30_test_ctx* ctx);
uint64_t ut30_stat_wrong_ssrc(const ut30_test_ctx* ctx);
uint64_t ut30_stat_len_mismatch(const ut30_test_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30_RX_REDUNDANCY_HARNESS_H_ */
