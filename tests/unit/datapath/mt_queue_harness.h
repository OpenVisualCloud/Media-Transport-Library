/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for mt_rxq_get(): a port with one free DPDK rx queue and no NIC
 * behind it, so queue selection runs without a device.
 */

#ifndef _MT_QUEUE_HARNESS_H_
#define _MT_QUEUE_HARNESS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_rxq_ctx ut_rxq_ctx;

int ut_rxq_init(void);

/* Port P uses the DPDK user PMD, or the kernel socket backend when `kernel_socket`. */
ut_rxq_ctx* ut_rxq_create(bool kernel_socket);
void ut_rxq_destroy(ut_rxq_ctx* ctx);

/* mt_rxq_get() with no flow, as ST20_RX_FLAG_DATA_PATH_ONLY requests it. Returns 0
 * when a queue was handed out, < 0 otherwise. */
int ut_rxq_get_without_flow(ut_rxq_ctx* ctx);
int ut_rxq_put(ut_rxq_ctx* ctx);
bool ut_rxq_queue_active(const ut_rxq_ctx* ctx);
bool ut_rxq_queue_has_flow(const ut_rxq_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _MT_QUEUE_HARNESS_H_ */
