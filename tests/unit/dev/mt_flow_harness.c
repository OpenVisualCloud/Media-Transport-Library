/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * Harness for the mt_flow.c retry loops (SDBQ-3703 style transient VF flow
 * timeouts): compiles the real mt_flow.c with rte_flow_validate/create/destroy
 * and rte_free replaced by controllable stubs, so the retry logic runs against
 * synthetic failures instead of a real NIC.
 */

#include <errno.h>
#include <rte_flow.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "dev/mt_flow_harness.h"

/*
 * ut_flow_ctx is defined after mt_flow.c is included below, once mt_main.h's
 * types are visible. The rte_free shadow only takes effect on the mt_rte_free()
 * body inside mt_mem.h if that header has not been included yet, so mt_main.h
 * must not be pulled in before the #define block.
 */
struct ut_flow_ctx;
static struct ut_flow_ctx* ut_active_ctx;

static int ut_rte_flow_validate(uint16_t port_id, const struct rte_flow_attr* attr,
                                const struct rte_flow_item pattern[],
                                const struct rte_flow_action actions[],
                                struct rte_flow_error* error);
static struct rte_flow* ut_rte_flow_create(uint16_t port_id,
                                           const struct rte_flow_attr* attr,
                                           const struct rte_flow_item pattern[],
                                           const struct rte_flow_action actions[],
                                           struct rte_flow_error* error);
static int ut_rte_flow_destroy(uint16_t port_id, struct rte_flow* flow,
                               struct rte_flow_error* error);
static void ut_rte_free(void* ptr);

#define rte_flow_validate ut_rte_flow_validate
#define rte_flow_create ut_rte_flow_create
#define rte_flow_destroy ut_rte_flow_destroy
#define rte_free ut_rte_free
#include "mt_flow.c"
#undef rte_free
#undef rte_flow_destroy
#undef rte_flow_create
#undef rte_flow_validate

struct ut_flow_ctx {
  struct mtl_main_impl impl;
  /* rte_rx_flow_create() indexes inf->rx_queues[q] via mt_if_hdr_split_pool()
   * before the retry loop even runs; must be a real, zeroed queue so that
   * path resolves a NULL pool and skips the (untested) hdr-split branch. */
  struct mt_rx_queue rx_queue;
  int create_calls;
  /* -1 fails every rte_flow_create() call; N fails the first N calls then
   * lets call N+1 succeed. */
  int create_fail_count;
  /* attr.group observed on each rte_flow_create() call, used to confirm the
   * e810 group-2 fallback attempt. */
  uint32_t create_call_group[8];
  int destroy_calls;
  /* Same shape as create_fail_count, for rte_flow_destroy(). */
  int destroy_fail_count;
  bool last_free_flow_cleared;
};

static int ut_rte_flow_validate(uint16_t port_id, const struct rte_flow_attr* attr,
                                const struct rte_flow_item pattern[],
                                const struct rte_flow_action actions[],
                                struct rte_flow_error* error) {
  (void)port_id;
  (void)attr;
  (void)pattern;
  (void)actions;
  memset(error, 0, sizeof(*error));
  return 0;
}

static struct rte_flow* ut_rte_flow_create(uint16_t port_id,
                                           const struct rte_flow_attr* attr,
                                           const struct rte_flow_item pattern[],
                                           const struct rte_flow_action actions[],
                                           struct rte_flow_error* error) {
  (void)port_id;
  (void)pattern;
  (void)actions;
  struct ut_flow_ctx* ctx = ut_active_ctx;

  if ((size_t)ctx->create_calls < RTE_DIM(ctx->create_call_group))
    ctx->create_call_group[ctx->create_calls] = attr->group;
  ctx->create_calls++;

  memset(error, 0, sizeof(*error));
  bool fail =
      (ctx->create_fail_count < 0) || (ctx->create_calls <= ctx->create_fail_count);
  if (fail) {
    error->message = "ut simulated create failure";
    return NULL;
  }
  return (struct rte_flow*)(uintptr_t)0x1;
}

static int ut_rte_flow_destroy(uint16_t port_id, struct rte_flow* flow,
                               struct rte_flow_error* error) {
  (void)port_id;
  (void)flow;
  struct ut_flow_ctx* ctx = ut_active_ctx;

  ctx->destroy_calls++;
  memset(error, 0, sizeof(*error));
  bool fail =
      (ctx->destroy_fail_count < 0) || (ctx->destroy_calls <= ctx->destroy_fail_count);
  return fail ? -EAGAIN : 0;
}

/*
 * rx_flow_free() unconditionally calls mt_rte_free(rsp) at the end. Real DPDK
 * rte_free() on a stack-allocated struct would corrupt the allocator, so this
 * is a no-op: the caller passes a stack-local struct mt_rx_flow_rsp it reads
 * back after the call, which a real free would make a use-after-free.
 */
static void ut_rte_free(void* ptr) {
  (void)ptr;
}

ut_flow_ctx* ut_flow_create_ctx(void) {
  ut_flow_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  struct mt_interface* inf = &ctx->impl.inf[MTL_PORT_P];
  inf->parent = &ctx->impl;
  inf->port = MTL_PORT_P;
  inf->port_id = 0;
  inf->nb_rx_q = 1;
  inf->rx_queues = &ctx->rx_queue;
  mt_pthread_mutex_init(&inf->vf_cmd_mutex, NULL);
  ut_active_ctx = ctx;
  return ctx;
}

void ut_flow_destroy_ctx(ut_flow_ctx* ctx) {
  if (!ctx) return;
  if (ut_active_ctx == ctx) ut_active_ctx = NULL;
  mt_pthread_mutex_destroy(&ctx->impl.inf[MTL_PORT_P].vf_cmd_mutex);
  free(ctx);
}

void ut_flow_set_create_fail_count(ut_flow_ctx* ctx, int fail_count) {
  ctx->create_fail_count = fail_count;
}

void ut_flow_set_destroy_fail_count(ut_flow_ctx* ctx, int fail_count) {
  ctx->destroy_fail_count = fail_count;
}

int ut_flow_create_calls(const ut_flow_ctx* ctx) {
  return ctx->create_calls;
}

int ut_flow_destroy_calls(const ut_flow_ctx* ctx) {
  return ctx->destroy_calls;
}

uint32_t ut_flow_create_call_group(const ut_flow_ctx* ctx, int call_index) {
  return ctx->create_call_group[call_index];
}

bool ut_flow_rx_flow_create(ut_flow_ctx* ctx, bool no_ip_flow) {
  ut_active_ctx = ctx;

  struct mt_rxq_flow flow;
  memset(&flow, 0, sizeof(flow));
  flow.dip_addr[0] = 192;
  flow.dip_addr[1] = 168;
  flow.dip_addr[2] = 1;
  flow.dip_addr[3] = 100;
  flow.sip_addr[0] = 192;
  flow.sip_addr[1] = 168;
  flow.sip_addr[2] = 1;
  flow.sip_addr[3] = 1;
  flow.dst_port = 20000;
  if (no_ip_flow) flow.flags |= MT_RXQ_FLOW_F_NO_IP;

  struct rte_flow* result = rte_rx_flow_create(&ctx->impl.inf[MTL_PORT_P], 0, &flow);
  return result != NULL;
}

int ut_flow_rx_flow_free(ut_flow_ctx* ctx) {
  ut_active_ctx = ctx;

  struct mt_rx_flow_rsp rsp;
  memset(&rsp, 0, sizeof(rsp));
  rsp.flow = (struct rte_flow*)(uintptr_t)0x1;
  rsp.flow_id = -1;
  rsp.queue_id = 0;
  rsp.dst_port = 20000;

  int ret = rx_flow_free(&ctx->impl.inf[MTL_PORT_P], &rsp);
  ctx->last_free_flow_cleared = (rsp.flow == NULL);
  return ret;
}

bool ut_flow_last_free_flow_cleared(const ut_flow_ctx* ctx) {
  return ctx->last_free_flow_cleared;
}
