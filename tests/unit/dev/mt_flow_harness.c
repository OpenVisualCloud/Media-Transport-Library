/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * Harness for mt_flow.c without a NIC: compiles the real mt_flow.c with
 * rte_flow_validate/create/destroy, rte_zmalloc_socket and rte_free replaced by
 * controllable stubs. It drives the retry loops (transient VF flow timeouts) and the
 * iavf VF reset handling: the flow list, the reset callbacks and their registration
 * against stub register functions (with DPDK 26.03+, MT_FLOW_IAVF_RESET_CB).
 */

#define _GNU_SOURCE /* as mt_main.h, before any system header */
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
/* noipa: a .constprop clone would lose the rte_free pairing rte_malloc.h gives it
 * (-Wmismatched-dealloc) */
static void* ut_rte_zmalloc_socket(const char* type, size_t size, unsigned int align,
                                   int socket) __attribute__((noipa));

#define rte_flow_validate ut_rte_flow_validate
#define rte_flow_create ut_rte_flow_create
#define rte_flow_destroy ut_rte_flow_destroy
#define rte_free ut_rte_free
#define rte_zmalloc_socket ut_rte_zmalloc_socket
/* the iavf callback register retries: 3 tries 1 ms apart, not 600 x 100 ms */
#define MT_FLOW_IAVF_CB_TRIES (3)
#define MT_FLOW_IAVF_CB_RETRY_MS (1)
#include "mt_flow.c"
#undef rte_zmalloc_socket
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
  struct rte_flow* last_destroyed;
  struct rte_flow* last_created;
  /* UDP destination port and queue index of each rte_flow_create() call */
  uint16_t create_dst_port[8];
  uint16_t create_queue[8];
  /* port P's flow state, and the responses rte_zmalloc_socket() hands out */
  struct mt_flow_impl flow_impl;
  int flow_impl_frees;
  struct mt_rx_flow_rsp pool[8];
  int pool_used;
  /* the iavf register stubs: -EBUSY for the next reg_busy calls, then a log of the
   * changes, P / p post / pre set, X / x post / pre cleared */
  int reg_busy;
  char reg_log[16];
  int reg_n;
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
  struct ut_flow_ctx* ctx = ut_active_ctx;

  if ((size_t)ctx->create_calls < RTE_DIM(ctx->create_call_group))
    ctx->create_call_group[ctx->create_calls] = attr->group;
  ctx->create_calls++;

  memset(error, 0, sizeof(*error));
  bool fail =
      (ctx->create_fail_count < 0) || (ctx->create_calls <= ctx->create_fail_count);
  if ((size_t)(ctx->create_calls - 1) < RTE_DIM(ctx->create_dst_port)) {
    int n = ctx->create_calls - 1;

    for (const struct rte_flow_item* it = pattern; it->type != RTE_FLOW_ITEM_TYPE_END;
         it++)
      if (it->type == RTE_FLOW_ITEM_TYPE_UDP && it->spec)
        ctx->create_dst_port[n] =
            ntohs(((const struct rte_flow_item_udp*)it->spec)->hdr.dst_port);
    for (const struct rte_flow_action* a = actions; a->type != RTE_FLOW_ACTION_TYPE_END;
         a++)
      if (a->type == RTE_FLOW_ACTION_TYPE_QUEUE && a->conf)
        ctx->create_queue[n] = ((const struct rte_flow_action_queue*)a->conf)->index;
  }
  if (fail) {
    error->message = "ut simulated create failure";
    return NULL;
  }
  /* a handle of its own per call, as a PMD gives */
  ctx->last_created = (struct rte_flow*)(uintptr_t)(0x1000 + ctx->create_calls);
  return ctx->last_created;
}

static int ut_rte_flow_destroy(uint16_t port_id, struct rte_flow* flow,
                               struct rte_flow_error* error) {
  (void)port_id;
  (void)flow;
  struct ut_flow_ctx* ctx = ut_active_ctx;

  ctx->destroy_calls++;
  ctx->last_destroyed = flow;
  memset(error, 0, sizeof(*error));
  bool fail =
      (ctx->destroy_fail_count < 0) || (ctx->destroy_calls <= ctx->destroy_fail_count);
  return fail ? -EAGAIN : 0;
}

/*
 * mt_rte_free(): the responses come from the context's pool and the retry tests pass
 * a stack-local response they read back after rx_flow_free(), so nothing is freed;
 * frees of the flow state are counted for mt_flow_uinit().
 */
static void ut_rte_free(void* ptr) {
  if (ut_active_ctx && ptr == &ut_active_ctx->flow_impl) ut_active_ctx->flow_impl_frees++;
}

/* the responses of mt_rx_flow_create(), from the context's pool */
static void* ut_rte_zmalloc_socket(const char* type, size_t size, unsigned int align,
                                   int socket) {
  struct ut_flow_ctx* ctx = ut_active_ctx;
  void* p;

  (void)type;
  (void)align;
  (void)socket;
  if (!ctx || size > sizeof(ctx->pool[0]) || ctx->pool_used >= (int)RTE_DIM(ctx->pool))
    return NULL;
  p = &ctx->pool[ctx->pool_used++];
  memset(p, 0, size);
  return p;
}

#ifdef MT_FLOW_IAVF_RESET_CB
static int ut_reg_change(char set, char clear, bool has_cb) {
  struct ut_flow_ctx* ctx = ut_active_ctx;

  if (ctx->reg_busy > 0) {
    ctx->reg_busy--;
    return -EBUSY;
  }
  if (ctx->reg_n < (int)sizeof(ctx->reg_log) - 1)
    ctx->reg_log[ctx->reg_n++] = (char)(has_cb ? set : clear);
  return 0;
}

static int ut_reg_pre(uint16_t port, iavf_pre_reset_cb_t cb, void* arg) {
  (void)port;
  (void)arg;
  return ut_reg_change('p', 'x', cb != NULL);
}

static int ut_reg_post(uint16_t port, iavf_post_reset_cb_t cb, void* arg) {
  (void)port;
  (void)arg;
  return ut_reg_change('P', 'X', cb != NULL);
}
#endif

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
  ctx->impl.user_para.num_ports = 1;
  ctx->flow_impl.parent = &ctx->impl;
  ctx->flow_impl.port = MTL_PORT_P;
  mt_pthread_mutex_init(&ctx->flow_impl.mutex, NULL);
  MT_TAILQ_INIT(&ctx->flow_impl.rte_flows);
  ctx->impl.flow[MTL_PORT_P] = &ctx->flow_impl;
#ifdef MT_FLOW_IAVF_RESET_CB
  flow_iavf_reg_pre = ut_reg_pre;
  flow_iavf_reg_post = ut_reg_post;
#endif
  ut_active_ctx = ctx;
  return ctx;
}

void ut_flow_destroy_ctx(ut_flow_ctx* ctx) {
  if (!ctx) return;
  if (ut_active_ctx == ctx) ut_active_ctx = NULL;
  /* mt_flow_uinit() destroyed it, or keeps it for the callbacks */
  if (ctx->impl.flow[MTL_PORT_P]) mt_pthread_mutex_destroy(&ctx->flow_impl.mutex);
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

int ut_flow_create(ut_flow_ctx* ctx, uint16_t dst_port) {
  struct mt_rxq_flow flow;
  struct mt_rx_flow_rsp* rsp;

  ut_active_ctx = ctx;
  memset(&flow, 0, sizeof(flow));
  flow.dip_addr[0] = 239;
  flow.dip_addr[1] = 0;
  flow.dip_addr[2] = 0;
  flow.dip_addr[3] = 1;
  flow.dst_port = dst_port;
  rsp = mt_rx_flow_create(&ctx->impl, MTL_PORT_P, 0, &flow);
  return rsp ? (int)(rsp - ctx->pool) : -1;
}

int ut_flow_free(ut_flow_ctx* ctx, int id) {
  ut_active_ctx = ctx;
  return mt_rx_flow_free(&ctx->impl, MTL_PORT_P, &ctx->pool[id]);
}

const void* ut_flow_handle(const ut_flow_ctx* ctx, int id) {
  return ctx->pool[id].flow;
}

int ut_flow_listed_count(const ut_flow_ctx* ctx) {
  const struct mt_rx_flow_rsp* rsp;
  int n = 0;

  MT_TAILQ_FOREACH(rsp, &ctx->flow_impl.rte_flows, next) n++;
  return n;
}

void ut_flow_set_no_flow(ut_flow_ctx* ctx, bool no_flow) {
  if (no_flow)
    ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_RX_NO_FLOW;
  else
    ctx->impl.inf[MTL_PORT_P].drv_info.flags &= ~MT_DRV_F_RX_NO_FLOW;
}

uint16_t ut_flow_create_dst_port(const ut_flow_ctx* ctx, int call_index) {
  return ctx->create_dst_port[call_index];
}

uint16_t ut_flow_create_queue(const ut_flow_ctx* ctx, int call_index) {
  return ctx->create_queue[call_index];
}

const void* ut_flow_last_destroyed(const ut_flow_ctx* ctx) {
  return ctx->last_destroyed;
}

const void* ut_flow_last_created(const ut_flow_ctx* ctx) {
  return ctx->last_created;
}

bool ut_flow_has_reset_cb(void) {
#ifdef MT_FLOW_IAVF_RESET_CB
  return true;
#else
  return false;
#endif
}

#ifdef MT_FLOW_IAVF_RESET_CB
void ut_flow_iavf_pre(ut_flow_ctx* ctx) {
  ut_active_ctx = ctx;
  flow_iavf_pre_reset(0, &ctx->flow_impl);
}

void ut_flow_iavf_post(ut_flow_ctx* ctx, int reset_state) {
  ut_active_ctx = ctx;
  flow_iavf_post_reset(0, reset_state, &ctx->flow_impl);
}

void ut_flow_set_reg_busy(ut_flow_ctx* ctx, int busy) {
  ctx->reg_busy = busy;
}

void ut_flow_reset_cb_register(ut_flow_ctx* ctx) {
  ut_active_ctx = ctx;
  mt_flow_reset_cb_register(&ctx->impl, MTL_PORT_P);
}

void ut_flow_reset_cb_unregister(ut_flow_ctx* ctx) {
  ut_active_ctx = ctx;
  mt_flow_reset_cb_unregister(&ctx->impl, MTL_PORT_P);
}

const char* ut_flow_reg_log(const ut_flow_ctx* ctx) {
  return ctx->reg_log;
}

bool ut_flow_reset_cb_on(const ut_flow_ctx* ctx) {
  return ctx->flow_impl.reset_cb;
}
#else /* the iavf reset callbacks need DPDK 26.03: the tests skip */
void ut_flow_iavf_pre(ut_flow_ctx* ctx) {
  (void)ctx;
}

void ut_flow_iavf_post(ut_flow_ctx* ctx, int reset_state) {
  (void)ctx;
  (void)reset_state;
}

void ut_flow_set_reg_busy(ut_flow_ctx* ctx, int busy) {
  ctx->reg_busy = busy;
}

void ut_flow_reset_cb_register(ut_flow_ctx* ctx) {
  (void)ctx;
}

void ut_flow_reset_cb_unregister(ut_flow_ctx* ctx) {
  (void)ctx;
}

const char* ut_flow_reg_log(const ut_flow_ctx* ctx) {
  return ctx->reg_log;
}

bool ut_flow_reset_cb_on(const ut_flow_ctx* ctx) {
  return ctx->flow_impl.reset_cb;
}
#endif

int ut_flow_uinit_frees(ut_flow_ctx* ctx) {
  ut_active_ctx = ctx;
  mt_flow_uinit(&ctx->impl);
  return ctx->flow_impl_frees;
}
