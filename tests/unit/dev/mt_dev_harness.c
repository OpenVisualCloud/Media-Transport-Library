/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "dev/mt_dev_harness.h"
#include "mt_main.h"

/* Wider than MT_EAL_MAX_ARGS so an argc past that bound can be recorded, and compared
 * against it, without the harness overflowing in turn. */
#define UT_DEV_EAL_ARGV_MAX (256)

struct ut_dev_ctx {
  struct mtl_main_impl impl;
  struct mt_rx_queue rx_queue;
  struct mt_tx_queue tx_queue;
  struct mt_kport_info kport_info;
  char* lcores;
  int eal_init_calls;
  int eal_argc;
  char* eal_argv[UT_DEV_EAL_ARGV_MAX];
  enum ut_dev_event events[16];
  int event_count;
  int timesync_enable_calls;
  int fail_timesync_call;
  int fail_timesync_error;
  int timesync_read_calls;
  int fail_timesync_read_call;
  int fail_timesync_read_error;
  int fail_port_start_error;
};

static struct ut_dev_ctx* ut_active_ctx;

static int ut_rte_eth_rx_queue_setup(uint16_t port_id, uint16_t rx_queue_id,
                                     uint16_t nb_rx_desc, unsigned int socket_id,
                                     const struct rte_eth_rxconf* rx_conf,
                                     struct rte_mempool* mb_pool);
static int ut_rte_eth_tx_queue_setup(uint16_t port_id, uint16_t tx_queue_id,
                                     uint16_t nb_tx_desc, unsigned int socket_id,
                                     const struct rte_eth_txconf* tx_conf);
static const struct rte_eth_rxtx_callback* ut_rte_eth_add_tx_callback(
    uint16_t port_id, uint16_t queue_id, rte_tx_callback_fn fn, void* user_param);
static int ut_rte_eth_timesync_enable(uint16_t port_id);
static int ut_rte_eth_timesync_read_time(uint16_t port_id, struct timespec* time);
static int ut_rte_eth_dev_start(uint16_t port_id);
static int ut_rte_eth_dev_stop(uint16_t port_id);
static int ut_rte_eth_stats_reset(uint16_t port_id);
static int ut_rte_eth_promiscuous_enable(uint16_t port_id);
static int ut_rte_eal_init(int argc, char** argv);

#define rte_eth_rx_queue_setup ut_rte_eth_rx_queue_setup
#define rte_eth_tx_queue_setup ut_rte_eth_tx_queue_setup
#define rte_eth_add_tx_callback ut_rte_eth_add_tx_callback
#define rte_eth_timesync_enable ut_rte_eth_timesync_enable
#define rte_eth_timesync_read_time ut_rte_eth_timesync_read_time
#define rte_eth_dev_start ut_rte_eth_dev_start
#define rte_eth_dev_stop ut_rte_eth_dev_stop
#define rte_eth_stats_reset ut_rte_eth_stats_reset
#define rte_eth_promiscuous_enable ut_rte_eth_promiscuous_enable
#define rte_eal_init ut_rte_eal_init
#include "dev/mt_dev.c"
#undef rte_eal_init
#undef rte_eth_promiscuous_enable
#undef rte_eth_stats_reset
#undef rte_eth_dev_stop
#undef rte_eth_dev_start
#undef rte_eth_timesync_read_time
#undef rte_eth_timesync_enable
#undef rte_eth_add_tx_callback
#undef rte_eth_tx_queue_setup
#undef rte_eth_rx_queue_setup

static void ut_dev_record(enum ut_dev_event event) {
  if (ut_active_ctx && ut_active_ctx->event_count < (int)RTE_DIM(ut_active_ctx->events))
    ut_active_ctx->events[ut_active_ctx->event_count++] = event;
}

static int ut_rte_eth_rx_queue_setup(uint16_t port_id, uint16_t rx_queue_id,
                                     uint16_t nb_rx_desc, unsigned int socket_id,
                                     const struct rte_eth_rxconf* rx_conf,
                                     struct rte_mempool* mb_pool) {
  (void)port_id;
  (void)rx_queue_id;
  (void)nb_rx_desc;
  (void)socket_id;
  (void)rx_conf;
  (void)mb_pool;
  ut_dev_record(UT_DEV_EVENT_RX_QUEUE_SETUP);
  return 0;
}

static int ut_rte_eth_tx_queue_setup(uint16_t port_id, uint16_t tx_queue_id,
                                     uint16_t nb_tx_desc, unsigned int socket_id,
                                     const struct rte_eth_txconf* tx_conf) {
  (void)port_id;
  (void)tx_queue_id;
  (void)nb_tx_desc;
  (void)socket_id;
  (void)tx_conf;
  ut_dev_record(UT_DEV_EVENT_TX_QUEUE_SETUP);
  return 0;
}

static const struct rte_eth_rxtx_callback* ut_rte_eth_add_tx_callback(
    uint16_t port_id, uint16_t queue_id, rte_tx_callback_fn fn, void* user_param) {
  (void)port_id;
  (void)queue_id;
  (void)fn;
  (void)user_param;
  return (const struct rte_eth_rxtx_callback*)(uintptr_t)1;
}

static int ut_rte_eth_timesync_enable(uint16_t port_id) {
  (void)port_id;
  ut_dev_record(UT_DEV_EVENT_TIMESYNC_ENABLE);
  ut_active_ctx->timesync_enable_calls++;
  if (ut_active_ctx->timesync_enable_calls == ut_active_ctx->fail_timesync_call)
    return ut_active_ctx->fail_timesync_error;
  return 0;
}

static int ut_rte_eth_timesync_read_time(uint16_t port_id, struct timespec* time) {
  (void)port_id;
  ut_dev_record(UT_DEV_EVENT_TIMESYNC_READ);
  ut_active_ctx->timesync_read_calls++;
  if (ut_active_ctx->timesync_read_calls == ut_active_ctx->fail_timesync_read_call)
    return ut_active_ctx->fail_timesync_read_error;
  time->tv_sec = 1;
  time->tv_nsec = 0;
  return 0;
}

static int ut_rte_eth_dev_start(uint16_t port_id) {
  (void)port_id;
  ut_dev_record(UT_DEV_EVENT_PORT_START);
  return ut_active_ctx->fail_port_start_error;
}

static int ut_rte_eth_dev_stop(uint16_t port_id) {
  (void)port_id;
  ut_dev_record(UT_DEV_EVENT_PORT_STOP);
  return 0;
}

static int ut_rte_eth_stats_reset(uint16_t port_id) {
  (void)port_id;
  return 0;
}

static int ut_rte_eth_promiscuous_enable(uint16_t port_id) {
  (void)port_id;
  return 0;
}

static int ut_rte_eal_init(int argc, char** argv) {
  ut_dev_ctx* ctx = ut_active_ctx;

  ctx->eal_init_calls++;
  ctx->eal_argc = argc;
  /* argv entries point at dev_eal_init()'s stack locals, so copy them out. */
  for (int i = 0; i < argc && i < UT_DEV_EAL_ARGV_MAX; i++) {
    ctx->eal_argv[i] = strdup(argv[i]);
    if (!ctx->eal_argv[i]) return -ENOMEM;
  }
  /* Fail so dev_eal_init() returns before latching its one-shot eal_initted guard. */
  return -1;
}

ut_dev_ctx* ut_dev_create_ctx(void) {
  ut_dev_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.user_para.num_ports = 1;
  ctx->impl.user_para.flags = MTL_FLAG_PTP_ENABLE;
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  inf->parent = &ctx->impl;
  inf->port = MTL_PORT_P;
  inf->port_id = 0;
  inf->drv_info.drv_type = MT_DRV_IGC;
  inf->drv_info.port_type = MT_PORT_PF;
  inf->nb_rx_q = 1;
  inf->nb_tx_q = 1;
  inf->nb_rx_desc = 128;
  inf->nb_tx_desc = 128;
  inf->rx_queues = &ctx->rx_queue;
  inf->tx_queues = &ctx->tx_queue;
  inf->rx_mbuf_pool = (struct rte_mempool*)(uintptr_t)1;
  ut_active_ctx = ctx;
  return ctx;
}

void ut_dev_destroy_ctx(ut_dev_ctx* ctx) {
  if (ut_active_ctx == ctx) ut_active_ctx = NULL;
  for (int i = 0; i < UT_DEV_EAL_ARGV_MAX; i++) free(ctx->eal_argv[i]);
  free(ctx->lcores);
  free(ctx);
}

void ut_dev_fail_timesync_enable(ut_dev_ctx* ctx, int call, int error) {
  ctx->fail_timesync_call = call;
  ctx->fail_timesync_error = error;
}

void ut_dev_fail_timesync_read(ut_dev_ctx* ctx, int call, int error) {
  ctx->fail_timesync_read_call = call;
  ctx->fail_timesync_read_error = error;
}

void ut_dev_fail_port_start(ut_dev_ctx* ctx, int error) {
  ctx->fail_port_start_error = error;
}

void ut_dev_use_non_igc_driver(ut_dev_ctx* ctx) {
  ctx->impl.inf[MTL_PORT_P].drv_info.drv_type = MT_DRV_ICE;
}

void ut_dev_set_ptp_enabled(ut_dev_ctx* ctx, bool enabled) {
  if (enabled)
    ctx->impl.user_para.flags |= MTL_FLAG_PTP_ENABLE;
  else
    ctx->impl.user_para.flags &= ~MTL_FLAG_PTP_ENABLE;
}

void ut_dev_set_port(ut_dev_ctx* ctx, enum mtl_port port, const char* bdf,
                     uint32_t rl_burst_size) {
  struct mtl_init_params* p = &ctx->impl.user_para;

  snprintf(p->port[port], MTL_PORT_MAX_LEN, "%s", bdf);
  p->port_params[port].rl_burst_size = rl_burst_size;
}

size_t ut_dev_pci_devarg_size(void) {
  return MT_EAL_PORT_ARG_MAX_LEN;
}

void ut_dev_build_pci_devarg(ut_dev_ctx* ctx, enum mtl_port port, char* out, size_t len) {
  dev_build_pci_devarg(&ctx->impl.user_para, port, out, len);
}

int ut_dev_start_port(ut_dev_ctx* ctx) {
  ut_active_ctx = ctx;
  return dev_start_port(&ctx->impl.inf[MTL_PORT_P]);
}

int ut_dev_create_ports(ut_dev_ctx* ctx) {
  ut_active_ctx = ctx;
  return mt_dev_create(&ctx->impl);
}

int ut_dev_event_count(const ut_dev_ctx* ctx) {
  return ctx->event_count;
}

enum ut_dev_event ut_dev_event_at(const ut_dev_ctx* ctx, int index) {
  return ctx->events[index];
}

bool ut_dev_port_started(const ut_dev_ctx* ctx) {
  return ctx->impl.inf[MTL_PORT_P].status & MT_IF_STAT_PORT_STARTED;
}

bool ut_dev_timesync_feature(const ut_dev_ctx* ctx) {
  return ctx->impl.inf[MTL_PORT_P].feature & MT_IF_FEATURE_TIMESYNC;
}

int ut_dev_eal_max_args(void) {
  return MT_EAL_MAX_ARGS;
}

int ut_dev_eal_lcores_max_len(void) {
  return MT_EAL_LCORES_MAX_LEN;
}

int ut_dev_eal_init(ut_dev_ctx* ctx) {
  ut_active_ctx = ctx;
  for (int i = 0; i < UT_DEV_EAL_ARGV_MAX; i++) {
    free(ctx->eal_argv[i]);
    ctx->eal_argv[i] = NULL;
  }
  ctx->eal_argc = 0;

  enum mtl_log_level level = mt_get_log_global_level();
  int ret = dev_eal_init(&ctx->impl.user_para, &ctx->kport_info);
  /* dev_eal_init() applies p->log_level process wide; keep it off the rest of the suite.
   */
  mt_set_log_global_level(level);
  return ret;
}

int ut_dev_eal_argc(const ut_dev_ctx* ctx) {
  return ctx->eal_argc;
}

const char* ut_dev_eal_argv(const ut_dev_ctx* ctx, int index) {
  if (index < 0 || index >= ctx->eal_argc || index >= UT_DEV_EAL_ARGV_MAX) return NULL;
  return ctx->eal_argv[index];
}

int ut_dev_eal_init_calls(const ut_dev_ctx* ctx) {
  return ctx->eal_init_calls;
}

void ut_dev_set_num_ports(ut_dev_ctx* ctx, int num_ports) {
  ctx->impl.user_para.num_ports = num_ports;
}

void ut_dev_set_dma_dev_ports(ut_dev_ctx* ctx, uint8_t num) {
  struct mtl_init_params* p = &ctx->impl.user_para;

  p->num_dma_dev_port = num;
  for (uint8_t i = 0; i < num && i < MTL_DMA_DEV_MAX; i++)
    snprintf(p->dma_dev_port[i], MTL_PORT_MAX_LEN, "0000:00:01.%u", i);
}

void ut_dev_set_lcores(ut_dev_ctx* ctx, uint32_t main_lcore, const char* lcores) {
  struct mtl_init_params* p = &ctx->impl.user_para;

  p->main_lcore = main_lcore;
  free(ctx->lcores);
  ctx->lcores = lcores ? strdup(lcores) : NULL;
  p->lcores = ctx->lcores;
}

void ut_dev_set_iova_mode(ut_dev_ctx* ctx, enum mtl_iova_mode mode) {
  ctx->impl.user_para.iova_mode = mode;
}

void ut_dev_set_log_level(ut_dev_ctx* ctx, enum mtl_log_level level) {
  ctx->impl.user_para.log_level = level;
}

void ut_dev_enable_rxtx_simd_512(ut_dev_ctx* ctx) {
  ctx->impl.user_para.flags |= MTL_FLAG_RXTX_SIMD_512;
}
