/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-41 (fast metadata) TX build-packet unit tests.
 * Wraps the static tx_fastmetadata_session_build_packet() so its mbuf
 * capacity checks (tailroom vs. header/payload size) can be exercised
 * directly, without a full TX session/scheduler setup.
 */

#ifndef _ST41_TX_HARNESS_H_
#define _ST41_TX_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rte_mbuf;
typedef struct ut_test_ctx ut_test_ctx;

/* Initialise the shared DPDK EAL and mempool. Idempotent. Returns 0 on success. */
int ut41tx_init(void);

/* Caller owns the returned pointer; free with ut41tx_ctx_destroy(). */
ut_test_ctx* ut41tx_ctx_create(void);
void ut41tx_ctx_destroy(ut_test_ctx* ctx);

/* Configure the single st41 frame the session sources payload bytes from. */
void ut41tx_ctx_set_payload(ut_test_ctx* ctx, const uint8_t* data, uint16_t len);

/* Allocate an mbuf from the shared pool with tailroom forced to exactly
 * `room` bytes (by adjusting data_off). Caller frees with ut41tx_free_mbuf(). */
struct rte_mbuf* ut41tx_alloc_mbuf(size_t room);

/* Same as ut41tx_alloc_mbuf(), but also pre-sets data_len to `stale_data_len`
 * (via direct field write, before the header is built) while keeping the
 * real remaining capacity (tailroom) at `room`. Lets a test put data_len and
 * tailroom at odds: e.g. data_len already >= the required size (so a
 * data_len-based capacity check would wrongly let the build proceed) while
 * tailroom is still too small (so the correct tailroom-based check must
 * still reject it). Distinguishes the fix from the pre-fix data_len check. */
struct rte_mbuf* ut41tx_alloc_mbuf_stale_data_len(size_t room, size_t stale_data_len);
void ut41tx_free_mbuf(struct rte_mbuf* m);

/* Call tx_fastmetadata_session_build_packet() directly. */
void ut41tx_build_packet(ut_test_ctx* ctx, struct rte_mbuf* pkt);

uint32_t ut41tx_pkt_data_len(const struct rte_mbuf* pkt);
uint32_t ut41tx_pkt_pkt_len(const struct rte_mbuf* pkt);

/* sizeof(eth) + sizeof(ipv4) + sizeof(udp): written into pkt->data_len as
 * soon as the fmd-header capacity check passes, before the payload check. */
size_t ut41tx_l234_hdr_len(void);
/* sizeof(struct st41_fmd_hdr): eth + ipv4 + udp + rtp, the minimum capacity
 * required to build any fast-metadata packet at all. */
size_t ut41tx_fmd_hdr_len(void);

#ifdef __cplusplus
}
#endif

#endif /* _ST41_TX_HARNESS_H_ */
