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
  UT_DEV_EVENT_TX_RL_COMMIT,
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
/** Marks the port a started iavf VF at a 25G link, as mt_dev_free() finds it. */
void ut_dev_set_started_iavf_tx(ut_dev_ctx* ctx, bool rl_root_active,
                                enum st21_tx_pacing_way pacing_way);
void ut_dev_set_port_stopped(ut_dev_ctx* ctx);
int ut_dev_free_ports(ut_dev_ctx* ctx);
/* Runs dev_config_port() on an iavf (else ice) port with a user nb_rx_desc, the RX
 * timestamp offload on or off and rx_desc_lim.nb_max. Returns the RX ring size chosen. */
int ut_dev_config_port_nb_rx_desc(ut_dev_ctx* ctx, bool iavf, bool hw_timestamp,
                                  uint16_t nb_rx_desc, uint16_t nb_max);

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

int ut_dev_event_count(const ut_dev_ctx* ctx);
enum ut_dev_event ut_dev_event_at(const ut_dev_ctx* ctx, int index);
bool ut_dev_port_started(const ut_dev_ctx* ctx);
bool ut_dev_timesync_feature(const ut_dev_ctx* ctx);
/** peak.rate, in bytes/s, of the last shaper profile added. */
uint64_t ut_dev_last_shaper_rate(const ut_dev_ctx* ctx);

/* Port stats. The writer thread holds the port's stats_lock while it adds
 * UT_DEV_STATS_WRITER_STEP to user_stats_port rx_packets, sleeps 50 ms, then adds it to
 * tx_packets, so a copy taken under the lock always sees the two counters equal. */
#define UT_DEV_STATS_WRITER_STEP (100u)
/** Feeds the stats from the zeroed software counters instead of rte_eth_stats_get(). */
void ut_dev_use_sw_stats(ut_dev_ctx* ctx);
/** Returns once the writer holds the lock mid-update. */
int ut_dev_stats_writer_start(ut_dev_ctx* ctx);
/** Returns -ENOENT if no writer was started. */
int ut_dev_stats_writer_join(ut_dev_ctx* ctx);
/** Starts the writer, as ut_dev_stats_writer_start(), right after mt_dev.c next
 * releases the port's stats_lock. */
void ut_dev_stats_writer_start_on_unlock(ut_dev_ctx* ctx);
int ut_dev_get_port_stats(ut_dev_ctx* ctx, struct mtl_port_status* stats);
int ut_dev_reset_port_stats(ut_dev_ctx* ctx);
void ut_dev_user_port_stats(const ut_dev_ctx* ctx, struct mtl_port_status* stats);

#ifdef __cplusplus
}
#endif

#endif
