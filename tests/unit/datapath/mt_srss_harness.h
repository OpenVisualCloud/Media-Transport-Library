/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_DATAPATH_MT_SRSS_HARNESS_H
#define TESTS_UNIT_DATAPATH_MT_SRSS_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_srss_ctx ut_srss_ctx;

int ut_srss_init(void);

/* A dispatcher over nb_queues queues; only queue 0 returns nb_pkts non-IP packets. */
ut_srss_ctx* ut_srss_create_ctx(uint16_t nb_queues, uint16_t nb_pkts);
/* One shared RSS port set up by the real mt_srss_init(); NULL if it fails. */
ut_srss_ctx* ut_srss_init_port(uint32_t link_speed_mbps);
void ut_srss_destroy_ctx(ut_srss_ctx* ctx);

/* One pass of the static srss_sch_tasklet_handler(), returning its result. */
int ut_srss_tasklet_handler(ut_srss_ctx* ctx);

/* advice_sleep_us of the tasklet mt_srss_init() registered. */
uint64_t ut_srss_registered_advice_sleep_us(const ut_srss_ctx* ctx);

/* MT_SRSS_BURST_SIZE, the most packets one rx burst takes from a queue. */
uint16_t ut_srss_burst_size(void);

#ifdef __cplusplus
}
#endif

#endif
