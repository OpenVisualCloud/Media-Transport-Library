/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_DEV_MT_FLOW_HARNESS_H
#define TESTS_UNIT_DEV_MT_FLOW_HARNESS_H

/*
 * C API for exercising the mt_flow.c rte_flow_create()/rte_flow_destroy()
 * retry loops without a NIC. Callers configure how many times the DPDK stub
 * should fail, invoke the static production function directly, then read
 * back call counts and outcome through this API.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_flow_ctx ut_flow_ctx;

ut_flow_ctx* ut_flow_create_ctx(void);
void ut_flow_destroy_ctx(ut_flow_ctx* ctx);

/* rte_flow_create: fail the first fail_count calls then succeed; -1 fails every call. */
void ut_flow_set_create_fail_count(ut_flow_ctx* ctx, int fail_count);
/* rte_flow_destroy: same shape as ut_flow_set_create_fail_count. */
void ut_flow_set_destroy_fail_count(ut_flow_ctx* ctx, int fail_count);

int ut_flow_create_calls(const ut_flow_ctx* ctx);
int ut_flow_destroy_calls(const ut_flow_ctx* ctx);
uint32_t ut_flow_create_call_group(const ut_flow_ctx* ctx, int call_index);

/* Calls the static rte_rx_flow_create() directly for a unicast flow. */
bool ut_flow_rx_flow_create(ut_flow_ctx* ctx, bool no_ip_flow);

/* Calls the static rx_flow_free() directly on a synthetic response with a flow set. */
int ut_flow_rx_flow_free(ut_flow_ctx* ctx);
bool ut_flow_last_free_flow_cleared(const ut_flow_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
