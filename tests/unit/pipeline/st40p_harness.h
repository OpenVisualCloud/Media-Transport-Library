/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST40 pipeline-layer unit tests.
 * Tests the frame-assembly logic in rx_st40p_rtp_ready().
 */

#ifndef _ST40P_PIPELINE_HARNESS_H_
#define _ST40P_PIPELINE_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"
#include "st40_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut40p_ctx ut40p_ctx;

int ut40p_init(void);

/** Create a pipeline test context.
 *  num_port: 1 or 2 (redundant).
 *  framebuff_cnt: number of frame buffers (3 is typical). */
ut40p_ctx* ut40p_ctx_create(int num_port, int framebuff_cnt);
void ut40p_ctx_destroy(ut40p_ctx* ctx);

/** Enqueue one ANC RTP packet into the mock transport ring.
 *  The mbuf is built with L2+L3+L4 headers followed by an RFC 8331 RTP header
 *  with anc_count=1 and empty UDW payload (zero user data words).
 *  port: MTL_SESSION_PORT_P or MTL_SESSION_PORT_R. */
int ut40p_enqueue_pkt(ut40p_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port);

/** Enqueue one ANC RTP packet with a custom DPDK port_id (for unmapped port tests). */
int ut40p_enqueue_pkt_port_id(ut40p_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                              uint16_t dpdk_port_id);

/** Enqueue a burst of sequential packets. marker on last if last_marker. */
void ut40p_enqueue_burst(ut40p_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                         int last_marker, enum mtl_session_port port);

/** Call rx_st40p_rtp_ready() once — processes one mbuf from the ring. */
int ut40p_process(ut40p_ctx* ctx);

/** Call rx_st40p_rtp_ready() in a loop until the ring is empty. */
void ut40p_process_all(ut40p_ctx* ctx);

/** Get next READY frame (wraps st40p_rx_get_frame). Returns NULL if none. */
struct st40_frame_info* ut40p_get_frame(ut40p_ctx* ctx);

/** Return a frame to the pipeline (wraps st40p_rx_put_frame). */
int ut40p_put_frame(ut40p_ctx* ctx, struct st40_frame_info* frame);

/* ── stat accessors ───────────────────────────────────────────────── */

uint32_t ut40p_stat_busy(const ut40p_ctx* ctx);
uint32_t ut40p_stat_drop_frame(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_received(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_dropped(const ut40p_ctx* ctx);
uint64_t ut40p_stat_frames_corrupted(const ut40p_ctx* ctx);

/* ── public-API wrappers ─────────────────────────────────────────── */

/** Wraps st40p_rx_get_session_stats() — exercises the atomic overlay path. */
int ut40p_get_session_stats(ut40p_ctx* ctx, struct st40_rx_user_stats* stats);

/** Wraps st40p_rx_reset_session_stats() — exercises the atomic reset path. */
int ut40p_reset_session_stats(ut40p_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST40P_PIPELINE_HARNESS_H_ */
