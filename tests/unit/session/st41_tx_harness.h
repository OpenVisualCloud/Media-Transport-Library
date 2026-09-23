/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-41 (fast metadata) TX build-packet unit tests.
 * Wraps the static tx_fastmetadata_session_build_packet() so its mbuf
 * capacity checks can be exercised directly, without a full TX
 * session/scheduler setup.
 */

#ifndef _ST41_TX_HARNESS_H_
#define _ST41_TX_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* _ST41_TX_HARNESS_H_ */
