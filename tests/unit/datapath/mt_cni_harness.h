/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the CNI RX dispatch unit tests: feeds one packet to the
 * production cni_rx_handle() on MTL_PORT_R, with or without a PTP instance on
 * that port, and counts the mt_ptp_parse() calls it makes.
 */

#ifndef _UT_MT_CNI_HARNESS_H_
#define _UT_MT_CNI_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_cni_ctx ut_cni_ctx;

int ut_cni_eal_init(void);
ut_cni_ctx* ut_cni_create(bool ptp_instance);
void ut_cni_destroy(ut_cni_ctx* ctx);

/* Hand one L2 (ethertype 0x88F7) PTP Sync pkt to cni_rx_handle(). */
int ut_cni_rx_l2_ptp(ut_cni_ctx* ctx, bool vlan);

int ut_cni_ptp_parse_calls(void);
uint64_t ut_cni_eth_rx_bytes(const ut_cni_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _UT_MT_CNI_HARNESS_H_ */
