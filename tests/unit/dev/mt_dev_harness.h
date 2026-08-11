/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_DEV_MT_DEV_HARNESS_H
#define TESTS_UNIT_DEV_MT_DEV_HARNESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_dev_ctx ut_dev_ctx;

enum ut_dev_event {
  UT_DEV_EVENT_RX_QUEUE_SETUP,
  UT_DEV_EVENT_TX_QUEUE_SETUP,
  UT_DEV_EVENT_TIMESYNC_ENABLE,
  UT_DEV_EVENT_PORT_START,
  UT_DEV_EVENT_PORT_STOP,
};

ut_dev_ctx* ut_dev_create_ctx(void);
void ut_dev_destroy_ctx(ut_dev_ctx* ctx);
void ut_dev_fail_timesync_enable(ut_dev_ctx* ctx, int call, int error);
int ut_dev_start_port(ut_dev_ctx* ctx);
int ut_dev_create_ports(ut_dev_ctx* ctx);
int ut_dev_event_count(const ut_dev_ctx* ctx);
enum ut_dev_event ut_dev_event_at(const ut_dev_ctx* ctx, int index);
bool ut_dev_port_started(const ut_dev_ctx* ctx);
bool ut_dev_timesync_feature(const ut_dev_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif