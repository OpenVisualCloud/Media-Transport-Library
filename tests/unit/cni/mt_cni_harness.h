/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the CNI RX dispatch unit tests: one port of a zeroed main handle,
 * whose frames end on the last byte before a PROT_NONE page.
 */

#ifndef TESTS_UNIT_CNI_MT_CNI_HARNESS_H
#define TESTS_UNIT_CNI_MT_CNI_HARNESS_H

#include <stdbool.h>
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

/* The port's address, and the peer whose ARP reply its table waits for. */
#define UT_CNI_SIP_ADDR {192, 168, 0, 2}
#define UT_CNI_ARP_PEER_ADDR {192, 168, 0, 1}

bool ut_cni_arp_resolved(ut_cni_ctx* ctx);

/* The port has seen an IGMP membership query. */
bool ut_cni_igmp_query_seen(ut_cni_ctx* ctx);

/* The PTP client of the port has taken the master of an announce. */
bool ut_cni_ptp_master_set(ut_cni_ctx* ctx);

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
