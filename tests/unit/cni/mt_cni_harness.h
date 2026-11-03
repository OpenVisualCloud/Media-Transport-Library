/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the CNI RX dispatch unit tests: one port of a zeroed main handle,
 * whose frames end on the last byte before a PROT_NONE page.
 */

#ifndef TESTS_UNIT_CNI_MT_CNI_HARNESS_H
#define TESTS_UNIT_CNI_MT_CNI_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_cni_ctx ut_cni_ctx;

ut_cni_ctx* ut_cni_create(void);
void ut_cni_destroy(ut_cni_ctx* ctx);

/* Hand len bytes of an Ethernet frame to the CNI RX handler; its return value. */
int ut_cni_rx(ut_cni_ctx* ctx, const uint8_t* frame, size_t len);

/* The DHCP client of the port starts out discovering, in this transaction. */
#define UT_CNI_DHCP_XID 0x4d544c31u
/* Mirror of enum mt_dhcp_status; the harness asserts they match. */
#define UT_CNI_DHCP_DISCOVERING 1
#define UT_CNI_DHCP_REQUESTING 2
#define UT_CNI_DHCP_BOUND 3

int ut_cni_dhcp_status(ut_cni_ctx* ctx);
const uint8_t* ut_cni_dhcp_server_ip(ut_cni_ctx* ctx);
const uint8_t* ut_cni_dhcp_netmask(ut_cni_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_CNI_MT_CNI_HARNESS_H */
