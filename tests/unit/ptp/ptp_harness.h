/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the generic RX hw-timestamp (mt_mbuf_time_stamp) unit tests.
 *
 * Keeps DPDK mbuf and MTL internal types opaque so the C++ test layer only
 * deals with plain values.
 */

#ifndef _PTP_HARNESS_H_
#define _PTP_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_ptp_test_ctx ut_ptp_test_ctx;

/* Initialise the shared DPDK EAL/mempool and register the generic RX
 * hw-timestamp dynfield. Idempotent — safe to call once per fixture SetUp().
 * Returns 0 on success, < 0 on failure. */
int ut_ptp_init(void);

/* Create a context with one PTP instance on MTL_PORT_P, RX_OFFLOAD_TIMESTAMP
 * enabled, and an identity ptp_correct_ts() (coefficient=1.0, last_sync_ts=0,
 * ptp_delta=0). Caller owns the pointer; free with ut_ptp_ctx_destroy(). */
ut_ptp_test_ctx* ut_ptp_ctx_create(void);

/* Same as ut_ptp_ctx_create() but without MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP,
 * exercising mt_mbuf_time_stamp()'s fallback path. Caller owns the pointer;
 * free with ut_ptp_ctx_destroy(). */
ut_ptp_test_ctx* ut_ptp_ctx_create_no_offload_timestamp(void);

void ut_ptp_ctx_destroy(ut_ptp_test_ctx* ctx);

/* Allocate an mbuf from the shared pool with packet_type/timesync/ol_flags
 * all cleared (none of the ice IEEE1588-specific markers set) and the
 * generic hw-timestamp dynfield set to `ns`. Returns NULL on pool
 * exhaustion. */
void* ut_ptp_alloc_mbuf(ut_ptp_test_ctx* ctx, uint64_t ns);

void ut_ptp_free_mbuf(void* mbuf);

/* True iff the mbuf carries none of the ice IEEE1588-specific markers:
 * packet_type != RTE_PTYPE_L2_ETHER_TIMESYNC, timesync == 0, and neither
 * RX_IEEE1588_PTP nor RX_IEEE1588_TMST is set in ol_flags. */
bool ut_ptp_mbuf_is_unpatched_ice_state(const void* mbuf);

/* Wraps mt_mbuf_time_stamp(impl, mbuf, MTL_PORT_P). */
uint64_t ut_ptp_mbuf_time_stamp(ut_ptp_test_ctx* ctx, void* mbuf);

#ifdef __cplusplus
}
#endif

#endif /* _PTP_HARNESS_H_ */
