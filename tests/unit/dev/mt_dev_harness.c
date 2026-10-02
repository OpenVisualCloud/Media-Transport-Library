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
#define UT_DEV_TX_QUEUES_MAX (8)
#define UT_DEV_TM_PROFILES_MAX (MT_MAX_RL_ITEMS + 2)
#define UT_DEV_TM_NONLEAF_MAX (8)
/* iavf_status.h codes iavf_hierarchy_commit() returns */
#define UT_IAVF_ERR_PARAM (-5)
#define UT_IAVF_ERR_NOT_READY (-63)

struct ut_dev_ctx {
  struct mtl_main_impl impl;
  struct mt_rx_queue rx_queue;
  struct mt_tx_queue tx_queues[UT_DEV_TX_QUEUES_MAX];
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
  bool tm_vf_without_qos;
  int tm_queue_node_adds;
  int tm_fail_queue_node_add_call;
  int tm_fail_queue_node_add_error;
  int tm_fail_commit_error;
  int tm_calls;
  int tm_calls_needing_qos;
  bool tm_profile[UT_DEV_TM_PROFILES_MAX];
  uint64_t tm_profile_rate[UT_DEV_TM_PROFILES_MAX];
  bool tm_nonleaf[UT_DEV_TM_NONLEAF_MAX];
  bool tm_leaf[UT_DEV_TX_QUEUES_MAX];
  uint32_t tm_leaf_profile_id[UT_DEV_TX_QUEUES_MAX];
  bool tm_committed[UT_DEV_TX_QUEUES_MAX];
  uint64_t tm_committed_rate[UT_DEV_TX_QUEUES_MAX];
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
static int ut_rte_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q, uint16_t nb_tx_q,
                                    const struct rte_eth_conf* conf);
static int ut_rte_eth_dev_adjust_nb_rx_tx_desc(uint16_t port_id, uint16_t* nb_rx_desc,
                                               uint16_t* nb_tx_desc);
static int ut_rte_eth_dev_get_supported_ptypes(uint16_t port_id, uint32_t ptype_mask,
                                               uint32_t* ptypes, int num);
static int ut_rte_tm_shaper_profile_add(uint16_t port_id, uint32_t shaper_profile_id,
                                        const struct rte_tm_shaper_params* profile,
                                        struct rte_tm_error* error);
static int ut_rte_tm_node_add(uint16_t port_id, uint32_t node_id, uint32_t parent_node_id,
                              uint32_t priority, uint32_t weight, uint32_t level_id,
                              const struct rte_tm_node_params* params,
                              struct rte_tm_error* error);
static int ut_rte_tm_node_delete(uint16_t port_id, uint32_t node_id,
                                 struct rte_tm_error* error);
static int ut_rte_tm_hierarchy_commit(uint16_t port_id, int clear_on_fail,
                                      struct rte_tm_error* error);

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
#define rte_eth_dev_configure ut_rte_eth_dev_configure
#define rte_eth_dev_adjust_nb_rx_tx_desc ut_rte_eth_dev_adjust_nb_rx_tx_desc
#define rte_eth_dev_get_supported_ptypes ut_rte_eth_dev_get_supported_ptypes
#define rte_tm_shaper_profile_add ut_rte_tm_shaper_profile_add
#define rte_tm_node_add ut_rte_tm_node_add
#define rte_tm_node_delete ut_rte_tm_node_delete
#define rte_tm_hierarchy_commit ut_rte_tm_hierarchy_commit
#include "dev/mt_dev.c"
#undef rte_tm_hierarchy_commit
#undef rte_tm_node_delete
#undef rte_tm_node_add
#undef rte_tm_shaper_profile_add
#undef rte_eth_dev_get_supported_ptypes
#undef rte_eth_dev_adjust_nb_rx_tx_desc
#undef rte_eth_dev_configure
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

static int ut_rte_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q, uint16_t nb_tx_q,
                                    const struct rte_eth_conf* conf) {
  (void)port_id;
  (void)nb_rx_q;
  (void)nb_tx_q;
  (void)conf;
  return 0;
}

/* Clamps to rx_desc_lim.nb_max, as the real one does. */
static int ut_rte_eth_dev_adjust_nb_rx_tx_desc(uint16_t port_id, uint16_t* nb_rx_desc,
                                               uint16_t* nb_tx_desc) {
  (void)port_id;
  (void)nb_tx_desc;
  *nb_rx_desc = RTE_MIN(*nb_rx_desc,
                        ut_active_ctx->impl.inf[MTL_PORT_P].dev_info.rx_desc_lim.nb_max);
  return 0;
}

static int ut_rte_eth_dev_get_supported_ptypes(uint16_t port_id, uint32_t ptype_mask,
                                               uint32_t* ptypes, int num) {
  (void)port_id;
  (void)ptype_mask;
  (void)ptypes;
  (void)num;
  return 0;
}

/* Without PF-granted QoS, iavf commit fails cleanly; the others crash or cannot tell. */
static bool ut_tm_vf_crashes(ut_dev_ctx* ctx) {
  ctx->tm_calls++;
  if (!ctx->tm_vf_without_qos) return false;
  ctx->tm_calls_needing_qos++;
  return true;
}

static bool ut_tm_is_nonleaf(uint32_t node_id) {
  return node_id >= ST_ROOT_NODE_ID && node_id < ST_ROOT_NODE_ID + UT_DEV_TM_NONLEAF_MAX;
}

static int ut_rte_tm_shaper_profile_add(uint16_t port_id, uint32_t shaper_profile_id,
                                        const struct rte_tm_shaper_params* profile,
                                        struct rte_tm_error* error) {
  ut_dev_ctx* ctx = ut_active_ctx;
  (void)port_id;
  (void)error;

  if (ut_tm_vf_crashes(ctx)) return -EIO;
  if (shaper_profile_id >= UT_DEV_TM_PROFILES_MAX) return -ENOSPC;
  if (ctx->tm_profile[shaper_profile_id]) return -EINVAL;
  ctx->tm_profile[shaper_profile_id] = true;
  ctx->tm_profile_rate[shaper_profile_id] = profile->peak.rate;
  return 0;
}

static int ut_rte_tm_node_add(uint16_t port_id, uint32_t node_id, uint32_t parent_node_id,
                              uint32_t priority, uint32_t weight, uint32_t level_id,
                              const struct rte_tm_node_params* params,
                              struct rte_tm_error* error) {
  ut_dev_ctx* ctx = ut_active_ctx;
  (void)port_id;
  (void)priority;
  (void)weight;
  (void)level_id;
  (void)error;

  if (ut_tm_vf_crashes(ctx)) return -EIO;
  if (parent_node_id != RTE_TM_NODE_ID_NULL &&
      (!ut_tm_is_nonleaf(parent_node_id) ||
       !ctx->tm_nonleaf[parent_node_id - ST_ROOT_NODE_ID]))
    return -EINVAL;
  if (ut_tm_is_nonleaf(node_id)) {
    if (ctx->tm_nonleaf[node_id - ST_ROOT_NODE_ID]) return -EEXIST;
    ctx->tm_nonleaf[node_id - ST_ROOT_NODE_ID] = true;
    return 0;
  }
  if (node_id >= UT_DEV_TX_QUEUES_MAX) return -EINVAL;
  ctx->tm_queue_node_adds++;
  if (ctx->tm_queue_node_adds == ctx->tm_fail_queue_node_add_call)
    return ctx->tm_fail_queue_node_add_error;
  if (ctx->tm_leaf[node_id]) return -EEXIST;
  if (params->shaper_profile_id >= UT_DEV_TM_PROFILES_MAX ||
      !ctx->tm_profile[params->shaper_profile_id])
    return -EINVAL;
  ctx->tm_leaf[node_id] = true;
  ctx->tm_leaf_profile_id[node_id] = params->shaper_profile_id;
  return 0;
}

static int ut_rte_tm_node_delete(uint16_t port_id, uint32_t node_id,
                                 struct rte_tm_error* error) {
  ut_dev_ctx* ctx = ut_active_ctx;
  (void)port_id;
  (void)error;

  if (ut_tm_vf_crashes(ctx)) return -EIO;
  if (node_id < UT_DEV_TX_QUEUES_MAX) {
    if (!ctx->tm_leaf[node_id]) return -EINVAL;
    ctx->tm_leaf[node_id] = false;
    return 0;
  }
  if (!ut_tm_is_nonleaf(node_id) || !ctx->tm_nonleaf[node_id - ST_ROOT_NODE_ID])
    return -EINVAL;
  for (uint32_t i = node_id - ST_ROOT_NODE_ID + 1; i < UT_DEV_TM_NONLEAF_MAX; i++) {
    if (ctx->tm_nonleaf[i]) return -EBUSY;
  }
  for (int q = 0; q < UT_DEV_TX_QUEUES_MAX; q++) {
    if (ctx->tm_leaf[q]) return -EBUSY;
  }
  ctx->tm_nonleaf[node_id - ST_ROOT_NODE_ID] = false;
  return 0;
}

static int ut_tm_commit_fail(ut_dev_ctx* ctx, int clear_on_fail, int error) {
  if (!clear_on_fail) return error;
  memset(ctx->tm_profile, 0, sizeof(ctx->tm_profile));
  memset(ctx->tm_nonleaf, 0, sizeof(ctx->tm_nonleaf));
  memset(ctx->tm_leaf, 0, sizeof(ctx->tm_leaf));
  return error;
}

static int ut_rte_tm_hierarchy_commit(uint16_t port_id, int clear_on_fail,
                                      struct rte_tm_error* error) {
  ut_dev_ctx* ctx = ut_active_ctx;
  uint16_t nb_tx_q = ctx->impl.inf[MTL_PORT_P].nb_tx_q;
  int fail_error = ctx->tm_fail_commit_error;
  (void)port_id;
  (void)error;

  ctx->tm_calls++;
  if (ctx->tm_vf_without_qos)
    return ut_tm_commit_fail(ctx, clear_on_fail, MT_IAVF_TM_NOT_SUPPORTED);
  /* one tc node allows a commit on a started port */
  if (!ctx->tm_nonleaf[ST_TM_NONLEAF_NODES_NUM_VF - 1]) return UT_IAVF_ERR_NOT_READY;
  for (uint16_t q = 0; q < nb_tx_q; q++) {
    if (!ctx->tm_leaf[q]) return ut_tm_commit_fail(ctx, clear_on_fail, UT_IAVF_ERR_PARAM);
  }
  if (fail_error) {
    ctx->tm_fail_commit_error = 0;
    return ut_tm_commit_fail(ctx, clear_on_fail, fail_error);
  }

  for (int q = 0; q < UT_DEV_TX_QUEUES_MAX; q++) {
    ctx->tm_committed[q] = ctx->tm_leaf[q];
    if (ctx->tm_leaf[q])
      ctx->tm_committed_rate[q] = ctx->tm_profile_rate[ctx->tm_leaf_profile_id[q]];
  }
  return 0;
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
  inf->tx_queues = ctx->tx_queues;
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

int ut_dev_config_port_nb_rx_desc(ut_dev_ctx* ctx, bool iavf, bool hw_timestamp,
                                  uint16_t nb_rx_desc, uint16_t nb_max) {
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  int ret;

  inf->drv_info.drv_type = iavf ? MT_DRV_IAVF : MT_DRV_ICE;
  if (hw_timestamp) inf->feature |= MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;
  inf->dev_info.rx_desc_lim.nb_max = nb_max;
  ctx->impl.user_para.nb_rx_desc = nb_rx_desc;
  ut_active_ctx = ctx;
  ret = dev_config_port(inf);
  return ret < 0 ? ret : inf->nb_rx_desc;
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

void ut_dev_set_driver(ut_dev_ctx* ctx, const char* driver_name) {
  parse_driver_info(driver_name, &ctx->impl.inf[MTL_PORT_P].drv_info);
}

void ut_dev_enable_shared_tx_queue(ut_dev_ctx* ctx) {
  ctx->impl.user_para.flags |= MTL_FLAG_SHARED_TX_QUEUE;
}

void ut_dev_tm_set_vf_without_qos(ut_dev_ctx* ctx) {
  ctx->tm_vf_without_qos = true;
}

void ut_dev_tm_fail_queue_node_add(ut_dev_ctx* ctx, int call, int error) {
  ctx->tm_fail_queue_node_add_call = call;
  ctx->tm_fail_queue_node_add_error = error;
}

void ut_dev_tm_fail_commit_once(ut_dev_ctx* ctx, int error) {
  ctx->tm_fail_commit_error = error;
}

int ut_dev_init_pacing(ut_dev_ctx* ctx, enum st21_tx_pacing_way pacing_way,
                       uint16_t nb_tx_q) {
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];

  if (nb_tx_q > UT_DEV_TX_QUEUES_MAX) return -EINVAL;
  inf->nb_tx_q = nb_tx_q;
  for (uint16_t q = 0; q < nb_tx_q; q++) {
    inf->tx_queues[q].queue_id = q;
    inf->tx_queues[q].rl_shapers_mapping = -1;
  }
  inf->tx_pacing_way = pacing_way;
  ut_active_ctx = ctx;
  return dev_if_init_pacing(inf);
}

enum st21_tx_pacing_way ut_dev_tx_pacing_way(const ut_dev_ctx* ctx) {
  return ctx->impl.inf[MTL_PORT_P].tx_pacing_way;
}

int ut_dev_tm_calls(const ut_dev_ctx* ctx) {
  return ctx->tm_calls;
}

int ut_dev_tm_calls_needing_qos(const ut_dev_ctx* ctx) {
  return ctx->tm_calls_needing_qos;
}

bool ut_dev_tm_committed_rate(const ut_dev_ctx* ctx, uint16_t queue,
                              uint64_t* bytes_per_sec) {
  if (queue >= UT_DEV_TX_QUEUES_MAX || !ctx->tm_committed[queue]) return false;
  *bytes_per_sec = ctx->tm_committed_rate[queue];
  return true;
}

bool ut_dev_dpdk_iavf_commit_checks_qos_first(void) {
  return MT_IAVF_TM_QOS_PROBE;
}

uint64_t ut_dev_default_rl_bps(void) {
  return ST_DEFAULT_RL_BPS;
}
