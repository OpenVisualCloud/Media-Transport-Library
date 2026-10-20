/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the mt_pcap unit tests: one main handle on a net_null port whose PTP
 * clock reads a fixed time.
 */

#ifndef TESTS_UNIT_MAIN_MT_PCAP_HARNESS_H
#define TESTS_UNIT_MAIN_MT_PCAP_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_pcap_ctx ut_pcap_ctx;

/* A main handle whose PTP clock always reads ptp_ns; NULL on failure. */
ut_pcap_ctx* ut_pcap_create(uint64_t ptp_ns);
void ut_pcap_destroy(ut_pcap_ctx* ctx);

/* mt_pcap_open() on a temporary file; 0 on success. */
int ut_pcap_open(ut_pcap_ctx* ctx);
/* mt_pcap_dump() of one synthetic packet; returns the packet count it wrote. */
int ut_pcap_dump_one(ut_pcap_ctx* ctx);
/* mt_pcap_close(), then read the first packet block timestamp; 0 on success. */
int ut_pcap_close_and_read_ts(ut_pcap_ctx* ctx, uint64_t* ts);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_MAIN_MT_PCAP_HARNESS_H */
