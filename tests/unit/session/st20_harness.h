/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST20 (video) RX redundancy unit tests.
 * Opaque context — C++ never includes internal MTL headers.
 */

#ifndef _ST20_SESSION_HARNESS_H_
#define _ST20_SESSION_HARNESS_H_

#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20_test_ctx ut20_test_ctx;

int ut20_init(void);

ut20_test_ctx* ut20_ctx_create(int num_port);
void ut20_ctx_destroy(ut20_test_ctx* ctx);

int ut20_feed_pkt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                  uint16_t line_offset, uint16_t line_length, enum mtl_session_port port);

int ut20_feed_frame_pkt(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                        enum mtl_session_port port);

int ut20_feed_frame_pkt_seq(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq, uint32_t ts,
                            enum mtl_session_port port);

void ut20_feed_full_frame(ut20_test_ctx* ctx, uint32_t ts, enum mtl_session_port port);

int ut20_feed_pkt_pt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                     uint16_t line_offset, uint16_t line_length,
                     enum mtl_session_port port, uint8_t pt);

int ut20_feed_pkt_ssrc(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts, uint16_t line_num,
                       uint16_t line_offset, uint16_t line_length,
                       enum mtl_session_port port, uint32_t ssrc);

void ut20_ctx_set_pt(ut20_test_ctx* ctx, uint8_t pt);
void ut20_ctx_set_ssrc(ut20_test_ctx* ctx, uint32_t ssrc);

uint64_t ut20_stat_received(const ut20_test_ctx* ctx);
uint64_t ut20_stat_redundant(const ut20_test_ctx* ctx);
uint64_t ut20_stat_lost_pkts(const ut20_test_ctx* ctx);
uint64_t ut20_stat_port_reordered(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_port_lost(const ut20_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut20_stat_no_slot(const ut20_test_ctx* ctx);
uint64_t ut20_stat_idx_oo_bitmap(const ut20_test_ctx* ctx);
uint64_t ut20_stat_frames_dropped(const ut20_test_ctx* ctx);
int ut20_frames_received(const ut20_test_ctx* ctx);

uint64_t ut20_stat_wrong_pt(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_ssrc(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_interlace(const ut20_test_ctx* ctx);
uint64_t ut20_stat_offset_dropped(const ut20_test_ctx* ctx);
uint64_t ut20_stat_wrong_len(const ut20_test_ctx* ctx);

int ut20_total_frame_pkts(void);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_SESSION_HARNESS_H_ */
