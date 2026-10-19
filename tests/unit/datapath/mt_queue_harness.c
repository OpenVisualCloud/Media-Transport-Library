/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "datapath/mt_queue_harness.h"

#include <stdlib.h>

#include "common/ut_common.h"
#include "datapath/mt_queue.h"

struct ut_rxq_ctx {
  struct mtl_main_impl impl;
  struct mt_rx_queue rx_queue;
  struct mt_rxq_entry* entry;
};

int ut_rxq_init(void) {
  return ut_eal_init();
}

ut_rxq_ctx* ut_rxq_create(bool kernel_socket) {
  ut_rxq_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.user_para.pmd[MTL_PORT_P] =
      kernel_socket ? MTL_PMD_KERNEL_SOCKET : MTL_PMD_DPDK_USER;
  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  inf->parent = &ctx->impl;
  inf->port = MTL_PORT_P;
  inf->socket_id = rte_socket_id();
  if (kernel_socket) inf->drv_info.flags |= MT_DRV_F_KERNEL_BASED;
  inf->nb_rx_q = 1;
  inf->rx_queues = &ctx->rx_queue;
  ctx->rx_queue.port = MTL_PORT_P;
  ctx->rx_queue.queue_id = 0;
  mt_pthread_mutex_init(&inf->rx_queues_mutex, NULL);
  return ctx;
}

void ut_rxq_destroy(ut_rxq_ctx* ctx) {
  if (!ctx) return;
  if (ctx->entry) mt_rxq_put(ctx->entry);
  mt_pthread_mutex_destroy(&ctx->impl.inf[MTL_PORT_P].rx_queues_mutex);
  free(ctx);
}

int ut_rxq_get_without_flow(ut_rxq_ctx* ctx) {
  ctx->entry = mt_rxq_get(&ctx->impl, MTL_PORT_P, NULL);
  return ctx->entry ? 0 : -EIO;
}

int ut_rxq_put(ut_rxq_ctx* ctx) {
  int ret = mt_rxq_put(ctx->entry);
  ctx->entry = NULL;
  return ret;
}

bool ut_rxq_queue_active(const ut_rxq_ctx* ctx) {
  return ctx->rx_queue.active;
}

bool ut_rxq_queue_has_flow(const ut_rxq_ctx* ctx) {
  return ctx->rx_queue.flow_rsp != NULL;
}
