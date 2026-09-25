/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_DEV_MT_DEV_HARNESS_H
#define TESTS_UNIT_DEV_MT_DEV_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_dev_ctx ut_dev_ctx;

enum ut_dev_event {
  UT_DEV_EVENT_RX_QUEUE_SETUP,
  UT_DEV_EVENT_TX_QUEUE_SETUP,
  UT_DEV_EVENT_TIMESYNC_ENABLE,
  UT_DEV_EVENT_TIMESYNC_READ,
  UT_DEV_EVENT_PORT_START,
  UT_DEV_EVENT_PORT_STOP,
};

struct ut_dev_tm_node {
  uint32_t node_id;
  uint32_t parent_node_id;
  uint32_t shaper_profile_id;
};

ut_dev_ctx* ut_dev_create_ctx(void);
void ut_dev_destroy_ctx(ut_dev_ctx* ctx);
void ut_dev_fail_timesync_enable(ut_dev_ctx* ctx, int call, int error);
void ut_dev_fail_timesync_read(ut_dev_ctx* ctx, int call, int error);
/** Injects the rte_eth_dev_start() return; 0 keeps the mocked start successful. */
void ut_dev_fail_port_start(ut_dev_ctx* ctx, int error);
void ut_dev_use_non_igc_driver(ut_dev_ctx* ctx);
void ut_dev_set_ptp_enabled(ut_dev_ctx* ctx, bool enabled);
void ut_dev_set_port(ut_dev_ctx* ctx, enum mtl_port port, const char* bdf,
                     uint32_t rl_burst_size);
/** Width of the devarg buffer dev_eal_init() passes, so tests cannot pick a wider one. */
size_t ut_dev_pci_devarg_size(void);
void ut_dev_build_pci_devarg(ut_dev_ctx* ctx, enum mtl_port port, char* out, size_t len);
int ut_dev_start_port(ut_dev_ctx* ctx);
int ut_dev_create_ports(ut_dev_ctx* ctx);

/* EAL argv builder. rte_eal_init() is stubbed out and fails, which keeps dev_eal_init()'s
 * one-shot guard unlatched, so these may be called repeatedly in one process. */

/** MT_EAL_MAX_ARGS. dev/mt_dev.h includes mt_main.h, which a C++ TU cannot parse
 * (st2110/st_header.h uses _Atomic), so the macros come through here. */
int ut_dev_eal_max_args(void);
/** MT_EAL_LCORES_MAX_LEN, the width of the "-l" argument dev_eal_init() builds. */
int ut_dev_eal_lcores_max_len(void);
/** Runs dev_eal_init(); returns its ret, always negative because the stub fails. */
int ut_dev_eal_init(ut_dev_ctx* ctx);
int ut_dev_eal_argc(const ut_dev_ctx* ctx);
const char* ut_dev_eal_argv(const ut_dev_ctx* ctx, int index);
/** Times rte_eal_init() was reached, telling a rejected argv from a completed one. */
int ut_dev_eal_init_calls(const ut_dev_ctx* ctx);
void ut_dev_set_num_ports(ut_dev_ctx* ctx, int num_ports);
/** Fills the first num dma_dev_port[] entries with synthetic BDFs. */
void ut_dev_set_dma_dev_ports(ut_dev_ctx* ctx, uint8_t num);
/** Copies lcores, so the caller need not keep it alive. */
void ut_dev_set_lcores(ut_dev_ctx* ctx, uint32_t main_lcore, const char* lcores);
void ut_dev_set_iova_mode(ut_dev_ctx* ctx, enum mtl_iova_mode mode);
void ut_dev_set_log_level(ut_dev_ctx* ctx, enum mtl_log_level level);
void ut_dev_enable_rxtx_simd_512(ut_dev_ctx* ctx);

/* Pacing init. The rte_tm_* calls of mt_dev.c go to fakes that record each node add. */

void ut_dev_set_pacing_port(ut_dev_ctx* ctx, bool iavf, enum st21_tx_pacing_way pacing);
void ut_dev_set_shared_txq(ut_dev_ctx* ctx);
/** Injects the rte_tm_capabilities_get() return; 0 keeps the port TM capable. */
void ut_dev_fail_tm_capabilities(ut_dev_ctx* ctx, int error);
/** Injects the rte_tm_node_add() return; 0 keeps the fake checking parent and id. */
void ut_dev_fail_tm_node_add(ut_dev_ctx* ctx, int error);
/** Marks the TM root as built, as an rl attempt earlier in this process leaves it. */
void ut_dev_set_rl_root_active(ut_dev_ctx* ctx);
/** Injects the rte_tm_hierarchy_commit() return; 0 keeps the commit successful. */
void ut_dev_fail_tm_commit(ut_dev_ctx* ctx, int error);
int ut_dev_init_pacing(ut_dev_ctx* ctx);
enum st21_tx_pacing_way ut_dev_pacing_way(const ut_dev_ctx* ctx);
int ut_dev_nb_tx_queues(void);
/** Every rte_tm_* call, of any kind. */
int ut_dev_tm_call_count(const ut_dev_ctx* ctx);
int ut_dev_tm_commit_count(const ut_dev_ctx* ctx);
int ut_dev_tm_node_count(const ut_dev_ctx* ctx);
struct ut_dev_tm_node ut_dev_tm_node_at(const ut_dev_ctx* ctx, int index);
/** RTE_TM_SHAPER_PROFILE_ID_NONE, so the tests need no DPDK header. */
uint32_t ut_dev_tm_shaper_none(void);
int ut_dev_tx_queue_rl_mapping(const ut_dev_ctx* ctx, int queue);
uint64_t ut_dev_tx_queue_bps(const ut_dev_ctx* ctx, int queue);

int ut_dev_event_count(const ut_dev_ctx* ctx);
enum ut_dev_event ut_dev_event_at(const ut_dev_ctx* ctx, int index);
bool ut_dev_port_started(const ut_dev_ctx* ctx);
bool ut_dev_timesync_feature(const ut_dev_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
