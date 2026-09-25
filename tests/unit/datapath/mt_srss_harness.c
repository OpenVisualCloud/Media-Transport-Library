/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_srss_harness.h"
#include "mt_main.h"

static uint16_t ut_rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
                                    struct rte_mbuf** rx_pkts, const uint16_t nb_pkts);

#define rte_eth_rx_burst ut_rte_eth_rx_burst
#include "datapath/mt_shared_rss.c"
#undef rte_eth_rx_burst

struct ut_srss_ctx {
  struct mtl_main_impl impl;
  struct mt_srss_impl srss;
  struct mt_srss_sch srss_sch;
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

void ut_srss_destroy_ctx(ut_srss_ctx* ctx) {
  if (!ctx) return;
  if (ut_active_ctx == ctx) ut_active_ctx = NULL;
  free(ctx);
}

int ut_srss_tasklet_handler(ut_srss_ctx* ctx) {
  ut_active_ctx = ctx;
  return srss_sch_tasklet_handler(&ctx->srss_sch);
}
