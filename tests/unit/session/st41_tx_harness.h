/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-41 (fast metadata) TX session unit tests.
 * Wraps the static tx_fastmetadata_session_build_packet() so its mbuf
 * capacity checks can be exercised directly, and drives the frame-level
 * tasklet with mocked PTP/TSC time for the timestamp tests.
 */

#ifndef _ST41_TX_HARNESS_H_
#define _ST41_TX_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

#include "st41_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rte_mbuf;
typedef struct ut41tx_ctx ut41tx_ctx;

/* Initialise the shared DPDK EAL and mempool. Idempotent. Returns 0 on success. */
int ut41tx_init(void);

/* Caller owns the returned pointer; free with ut41tx_ctx_destroy(). */
ut41tx_ctx* ut41tx_ctx_create(void);
void ut41tx_ctx_destroy(ut41tx_ctx* ctx);

/* Point the single st41 frame the session sources payload bytes from at
 * `len` bytes of `data`. Not copied: `data` must outlive the build. */
void ut41tx_ctx_set_payload(ut41tx_ctx* ctx, uint8_t* data, uint16_t len);

/* Allocate an mbuf from the shared pool with tailroom forced to exactly
 * `room` bytes (by adjusting data_off), or NULL if the pool's mbufs are
 * smaller. Caller frees with ut41tx_free_mbuf(). */
struct rte_mbuf* ut41tx_alloc_mbuf(size_t room);
void ut41tx_free_mbuf(struct rte_mbuf* m);

/* Call tx_fastmetadata_session_build_packet() directly. */
void ut41tx_build_packet(ut41tx_ctx* ctx, struct rte_mbuf* pkt);

uint32_t ut41tx_pkt_data_len(const struct rte_mbuf* pkt);
uint32_t ut41tx_pkt_pkt_len(const struct rte_mbuf* pkt);

/* sizeof(struct st41_fmd_hdr): eth + ipv4 + udp + rtp, the minimum capacity
 * required to build any fast-metadata packet at all. */
size_t ut41tx_fmd_hdr_len(void);

void ut41tx_set_ops_flags(ut41tx_ctx* ctx, uint32_t flags);
void ut41tx_set_mock_ptp_time(ut41tx_ctx* ctx, uint64_t ptp_ns);

/* Send one frame through the real frame-level tasklet: 1 ms epochs, a 90 kHz
 * media clock, the app handing over (tfmt, timestamp) in get_next_frame. The
 * epoch is reset from the mocked PTP time first, as at tasklet start, and the
 * mocked TSC is advanced to the packet's pacing time. Returns 0 if a packet
 * went out. The frame payload must have been set with ut41tx_ctx_set_payload(). */
int ut41tx_run_frame(ut41tx_ctx* ctx, enum st10_timestamp_fmt tfmt, uint64_t timestamp);

/* Meta the session passed to notify_frame_done for the last frame. */
const struct st41_tx_frame_meta* ut41tx_done_meta(const ut41tx_ctx* ctx);
/* RTP timestamp of the last packet the session sent, host order. */
uint32_t ut41tx_wire_rtp_timestamp(const ut41tx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST41_TX_HARNESS_H_ */
