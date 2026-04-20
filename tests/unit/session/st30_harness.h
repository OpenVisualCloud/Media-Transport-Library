/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST30 (audio) RX redundancy unit tests.
 * Opaque context — C++ never includes internal MTL headers.
 */

#ifndef _ST30_SESSION_HARNESS_H_
#define _ST30_SESSION_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut30_test_ctx ut30_test_ctx;

int ut30_init(void);

ut30_test_ctx* ut30_ctx_create(int num_port);
void ut30_ctx_destroy(ut30_test_ctx* ctx);

int ut30_feed_pkt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                  enum mtl_session_port port);
void ut30_feed_burst(ut30_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     enum mtl_session_port port);
int ut30_feed_pkt_pt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                     enum mtl_session_port port, uint8_t payload_type);
int ut30_feed_pkt_ssrc(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                       enum mtl_session_port port, uint32_t ssrc);
int ut30_feed_pkt_len(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                      enum mtl_session_port port, uint32_t payload_len);

void ut30_ctx_set_pt(ut30_test_ctx* ctx, uint8_t pt);
void ut30_ctx_set_ssrc(ut30_test_ctx* ctx, uint32_t ssrc);

uint64_t ut30_stat_unrecovered(const ut30_test_ctx* ctx);
uint64_t ut30_stat_redundant(const ut30_test_ctx* ctx);
uint64_t ut30_stat_received(const ut30_test_ctx* ctx);
uint64_t ut30_stat_lost_pkts(const ut30_test_ctx* ctx);
int ut30_session_seq_id(const ut30_test_ctx* ctx);
int ut30_frames_received(const ut30_test_ctx* ctx);
int ut30_pkts_per_frame(const ut30_test_ctx* ctx);

uint64_t ut30_stat_port_pkts(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_bytes(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_lost(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_reordered(const ut30_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut30_stat_port_duplicates(const ut30_test_ctx* ctx, enum mtl_session_port port);

uint64_t ut30_stat_wrong_pt(const ut30_test_ctx* ctx);
uint64_t ut30_stat_wrong_ssrc(const ut30_test_ctx* ctx);
uint64_t ut30_stat_len_mismatch(const ut30_test_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST30_SESSION_HARNESS_H_ */
