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

/* mt_rx_flow_create() of a multicast flow to dst_port on queue 0 of port P: the id of its
 * response, -1 on failure. mt_rx_flow_free() of it; its rte_flow handle (NULL if none).
 */
int ut_flow_create(ut_flow_ctx* ctx, uint16_t dst_port);
int ut_flow_free(ut_flow_ctx* ctx, int id);
const void* ut_flow_handle(const ut_flow_ctx* ctx, int id);
/* responses on port P's flow list; MT_DRV_F_RX_NO_FLOW on port P */
int ut_flow_listed_count(const ut_flow_ctx* ctx);
void ut_flow_set_no_flow(ut_flow_ctx* ctx, bool no_flow);
/* UDP destination port and queue of rte_flow_create() call call_index; the handle
 * rte_flow_destroy() got last */
uint16_t ut_flow_create_dst_port(const ut_flow_ctx* ctx, int call_index);
uint16_t ut_flow_create_queue(const ut_flow_ctx* ctx, int call_index);
const void* ut_flow_last_destroyed(const ut_flow_ctx* ctx);
/* the handle rte_flow_create() returned last */
const void* ut_flow_last_created(const ut_flow_ctx* ctx);

/* whether mt_flow.c has the iavf VF reset callbacks (DPDK 26.03+); without, the calls
 * below do nothing */
bool ut_flow_has_reset_cb(void);

/* the iavf VF reset callbacks, as the PMD's event thread calls them */
void ut_flow_iavf_pre(ut_flow_ctx* ctx);
void ut_flow_iavf_post(ut_flow_ctx* ctx, int reset_state);
/* mt_flow_reset_cb_register() / _unregister() on port P, against register stubs that
 * return -EBUSY for the next busy calls. Their log: P / p post / pre set, X / x cleared.
 */
void ut_flow_set_reg_busy(ut_flow_ctx* ctx, int busy);
void ut_flow_reset_cb_register(ut_flow_ctx* ctx);
void ut_flow_reset_cb_unregister(ut_flow_ctx* ctx);
const char* ut_flow_reg_log(const ut_flow_ctx* ctx);
bool ut_flow_reset_cb_on(const ut_flow_ctx* ctx);
/* mt_flow_uinit(); how often it has freed port P's flow state */
int ut_flow_uinit_frees(ut_flow_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
