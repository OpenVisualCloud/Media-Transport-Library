/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared infrastructure for unit test harnesses.
 * Provides idempotent EAL init, mempool, ring factory, and PTP stubs.
 *
 * This header is safe to include from any C translation unit — it does
 * NOT include internal MTL headers.
 */

#ifndef _UT_COMMON_H_
#define _UT_COMMON_H_

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UT_POOL_SIZE 2048
#define UT_RING_SIZE 512

/** Initialise DPDK EAL (idempotent — safe to call from multiple harnesses). */
int ut_eal_init(void);

/** Return the shared mempool (created on first call after ut_eal_init). */
struct rte_mempool* ut_pool(void);

/** Create an SP/SC ring with the given name and size.  Caller owns the ring. */
struct rte_ring* ut_ring_create(const char* name, unsigned int size);

/** Drain all mbufs from a ring and free them. */
void ut_ring_drain(struct rte_ring* ring);

#ifdef __cplusplus
}
#endif

#endif /* _UT_COMMON_H_ */
