/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_srss_harness.h"
#include "mt_main.h"
#include "mt_sch.h"
#include "mt_stat.h"

static uint16_t ut_rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
                                    struct rte_mbuf** rx_pkts, const uint16_t nb_pkts);
static struct mtl_sch_impl* ut_mt_sch_get(struct mtl_main_impl* impl, int quota_mbs,
                                          enum mt_sch_type type, mt_sch_mask_t mask);
static int ut_mt_sch_put(struct mtl_sch_impl* sch, int quota_mbs);
static mtl_tasklet_handle ut_mtl_sch_register_tasklet(
    struct mtl_sch_impl* sch, struct mtl_tasklet_ops* tasklet_ops);
static int ut_mtl_sch_unregister_tasklet(mtl_tasklet_handle tasklet);
static int ut_mt_stat_register(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv,
                               char* name);
static int ut_mt_stat_unregister(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv);
static int ut_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                             void* (*start_routine)(void*), void* arg);

#define rte_eth_rx_burst ut_rte_eth_rx_burst
#define mt_sch_get ut_mt_sch_get
#define mt_sch_put ut_mt_sch_put
#define mtl_sch_register_tasklet ut_mtl_sch_register_tasklet
#define mtl_sch_unregister_tasklet ut_mtl_sch_unregister_tasklet
#define mt_stat_register ut_mt_stat_register
#define mt_stat_unregister ut_mt_stat_unregister
#define pthread_create ut_pthread_create
#include "datapath/mt_shared_rss.c"
#undef pthread_create
#undef mt_stat_unregister
#undef mt_stat_register
#undef mtl_sch_unregister_tasklet
#undef mtl_sch_register_tasklet
#undef mt_sch_put
#undef mt_sch_get
#undef rte_eth_rx_burst

struct ut_srss_ctx {
  struct mtl_main_impl impl;
  struct mt_srss_impl srss;
  struct mt_srss_sch srss_sch;
  struct mt_sch_tasklet_impl tasklet;
  int requested_quota_mbs;
  uint16_t nb_pkts;
};

static struct ut_srss_ctx* ut_active_ctx;

static uint16_t ut_rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
                                    struct rte_mbuf** rx_pkts, const uint16_t nb_pkts) {
  (void)port_id;
  if (queue_id) return 0;
  uint16_t n = RTE_MIN(ut_active_ctx->nb_pkts, nb_pkts);

  if (rte_pktmbuf_alloc_bulk(ut_pool(), rx_pkts, n) < 0) return 0;
  /* ether_type 0 is non-IP, so the handler frees it: the ctx has no cni_entry */
  for (uint16_t i = 0; i < n; i++)
    memset(rte_pktmbuf_append(rx_pkts[i], sizeof(struct mt_udp_hdr)), 0,
           sizeof(struct mt_udp_hdr));
  return n;
}

static struct mtl_sch_impl* ut_mt_sch_get(struct mtl_main_impl* impl, int quota_mbs,
                                          enum mt_sch_type type, mt_sch_mask_t mask) {
  (void)type;
  (void)mask;
  ut_active_ctx->requested_quota_mbs = quota_mbs;
  return impl->main_sch;
}

static int ut_mt_sch_put(struct mtl_sch_impl* sch, int quota_mbs) {
  (void)sch;
  (void)quota_mbs;
  return 0;
}

static mtl_tasklet_handle ut_mtl_sch_register_tasklet(
    struct mtl_sch_impl* sch, struct mtl_tasklet_ops* tasklet_ops) {
  (void)sch;
  ut_active_ctx->tasklet.ops = *tasklet_ops;
  return &ut_active_ctx->tasklet;
}

static int ut_mtl_sch_unregister_tasklet(mtl_tasklet_handle tasklet) {
  (void)tasklet;
  return 0;
}

static int ut_mt_stat_register(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv,
                               char* name) {
  (void)impl;
  (void)cb;
  (void)priv;
  (void)name;
  return 0;
}

static int ut_mt_stat_unregister(struct mtl_main_impl* impl, mt_stat_cb_t cb,
                                 void* priv) {
  (void)impl;
  (void)cb;
  (void)priv;
  return 0;
}

/* Leaves srss->tid 0, so srss_traffic_thread_stop() skips pthread_join(). */
static int ut_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                             void* (*start_routine)(void*), void* arg) {
  (void)thread;
  (void)attr;
  (void)start_routine;
  (void)arg;
  return 0;
}

int ut_srss_init(void) {
  return ut_eal_init();
}

ut_srss_ctx* ut_srss_create_ctx(uint16_t nb_queues, uint16_t nb_pkts) {
  struct ut_srss_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->srss.parent = &ctx->impl;
  ctx->srss.port = MTL_PORT_P;
  ctx->srss_sch.parent = &ctx->srss;
  ctx->srss_sch.q_end = nb_queues;
  ctx->nb_pkts = nb_pkts;
  ut_active_ctx = ctx;
  return ctx;
}

ut_srss_ctx* ut_srss_init_port(uint32_t link_speed_mbps) {
  struct ut_srss_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.user_para.num_ports = 1;
  ctx->impl.main_sch = mt_sch_instance(&ctx->impl, 0);
  ctx->impl.main_sch->data_quota_mbs_limit =
      ST_QUOTA_TX1080P_PER_SCH * st20_1080p59_yuv422_10bit_bandwidth_mps();
  struct mt_interface* inf = mt_if(&ctx->impl, MTL_PORT_P);
  inf->rss_mode = MTL_RSS_MODE_L3_L4;
  inf->nb_rx_q = 1;
  inf->link_speed = link_speed_mbps;
  ut_active_ctx = ctx;
  if (mt_srss_init(&ctx->impl) < 0) {
    ut_srss_destroy_ctx(ctx);
    return NULL;
  }
  return ctx;
}

void ut_srss_destroy_ctx(ut_srss_ctx* ctx) {
  if (!ctx) return;
  mt_srss_uinit(&ctx->impl);
  if (ut_active_ctx == ctx) ut_active_ctx = NULL;
  free(ctx);
}

int ut_srss_tasklet_handler(ut_srss_ctx* ctx) {
  ut_active_ctx = ctx;
  return srss_sch_tasklet_handler(&ctx->srss_sch);
}

uint64_t ut_srss_registered_advice_sleep_us(const ut_srss_ctx* ctx) {
  return ctx->tasklet.ops.advice_sleep_us;
}

int ut_srss_requested_quota_mbs(const ut_srss_ctx* ctx) {
  return ctx->requested_quota_mbs;
}

int ut_srss_main_sch_quota_limit_mbs(const ut_srss_ctx* ctx) {
  return ctx->impl.main_sch->data_quota_mbs_limit;
}

uint16_t ut_srss_burst_size(void) {
  return MT_SRSS_BURST_SIZE;
}
