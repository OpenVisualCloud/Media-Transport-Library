/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST40 session-layer unit tests.
 * The test context is opaque — all access goes through accessors.
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

int ut40_init(void);
ut_test_ctx* ut40_ctx_create(int num_port);
void ut40_ctx_destroy(ut_test_ctx* ctx);
void ut40_drain_ring(void);

int ut40_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                  enum mtl_session_port port);
void ut40_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     int last_marker, enum mtl_session_port port);
int ut40_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint8_t payload_type);
int ut40_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port, uint32_t ssrc);
int ut40_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                        enum mtl_session_port port, uint8_t f_bits);

void ut40_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt);
void ut40_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc);
void ut40_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable);

uint64_t ut40_stat_unrecovered(const ut_test_ctx* ctx);
uint64_t ut40_stat_redundant(const ut_test_ctx* ctx);
uint64_t ut40_stat_received(const ut_test_ctx* ctx);
uint64_t ut40_stat_out_of_order(const ut_test_ctx* ctx);
int ut40_session_seq_id(const ut_test_ctx* ctx);

uint64_t ut40_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_ooo(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port);

uint64_t ut40_stat_wrong_pt(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_ssrc(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_interlace(const ut_test_ctx* ctx);
uint64_t ut40_stat_interlace_first(const ut_test_ctx* ctx);
uint64_t ut40_stat_interlace_second(const ut_test_ctx* ctx);
uint64_t ut40_stat_enqueue_fail(const ut_test_ctx* ctx);
int ut40_frames_received(const ut_test_ctx* ctx);

void ut40_set_skip_drain(bool skip);
int ut40_ring_dequeue_markers(int* out_count, bool* out_has_marker);

#ifdef __cplusplus
}
#endif

#endif /* _ST40_SESSION_HARNESS_H_ */
