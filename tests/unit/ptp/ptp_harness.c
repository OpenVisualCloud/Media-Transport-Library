/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the generic RX hw-timestamp (mt_mbuf_time_stamp) unit tests.
 *
 * tests/unit/pipeline/st40p_harness.c defines its own mt_mbuf_time_stamp()
 * stub (needed to isolate st40p pipeline tests from the PTP stack). Since
 * both harnesses link into the single UnitTest binary, the executable's own
 * copy of that symbol takes precedence over libmtl.so's real definition for
 * every caller in the process (standard ELF symbol interposition). This
 * harness resolves the real symbol directly from libmtl.so via dlsym() on
 * its own handle, bypassing that interposition, so it exercises the actual
 * production implementation rather than the st40p stub.
 */

#include "ptp/ptp_harness.h"

#include <dlfcn.h>
#include <rte_mbuf_dyn.h>
#include <stdlib.h>

#include "common/ut_common.h"
#include "mt_ptp.h"

struct ut_ptp_test_ctx {
  struct mtl_main_impl impl;
  struct mt_ptp_impl ptp;
};

typedef uint64_t (*mt_mbuf_time_stamp_fn)(struct mtl_main_impl* impl,
                                          struct rte_mbuf* mbuf, enum mtl_port port);

static bool g_dynfield_ready;
static int g_dynfield_offset = -1;
static mt_mbuf_time_stamp_fn g_real_mt_mbuf_time_stamp;

int ut_ptp_init(void) {
  int ret = ut_eal_init();
  if (ret < 0) return ret;

  if (!g_dynfield_ready) {
    ret = rte_mbuf_dyn_rx_timestamp_register(&g_dynfield_offset, NULL);
    if (ret < 0) return ret;

    void* handle = dlopen("libmtl.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return -1;
    g_real_mt_mbuf_time_stamp =
        (mt_mbuf_time_stamp_fn)dlsym(handle, "mt_mbuf_time_stamp");
    if (!g_real_mt_mbuf_time_stamp) return -1;

    g_dynfield_ready = true;
  }

  return 0;
}

static ut_ptp_test_ctx* ut_ptp_ctx_create_internal(bool enable_offload_timestamp) {
  if (!g_dynfield_ready) return NULL;

  ut_ptp_test_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.dynfield_offset = g_dynfield_offset;
  ctx->impl.ptp[MTL_PORT_P] = &ctx->ptp;
  if (enable_offload_timestamp)
    ctx->impl.inf[MTL_PORT_P].feature |= MT_IF_FEATURE_RX_OFFLOAD_TIMESTAMP;

  ctx->ptp.impl = &ctx->impl;
  ctx->ptp.port = MTL_PORT_P;
  ctx->ptp.coefficient = 1.0;
  ctx->ptp.last_sync_ts = 0;
  ctx->ptp.ptp_delta = 0;

  return ctx;
}

ut_ptp_test_ctx* ut_ptp_ctx_create(void) {
  return ut_ptp_ctx_create_internal(true);
}

ut_ptp_test_ctx* ut_ptp_ctx_create_no_offload_timestamp(void) {
  return ut_ptp_ctx_create_internal(false);
}

void ut_ptp_ctx_destroy(ut_ptp_test_ctx* ctx) {
  free(ctx);
}

void* ut_ptp_alloc_mbuf(ut_ptp_test_ctx* ctx, uint64_t ns) {
  struct rte_mbuf* mbuf = rte_pktmbuf_alloc(ut_pool());
  if (!mbuf) return NULL;

  mbuf->packet_type = 0;
  mbuf->timesync = 0;
  mbuf->ol_flags &= ~(RTE_MBUF_F_RX_IEEE1588_PTP | RTE_MBUF_F_RX_IEEE1588_TMST);
  *RTE_MBUF_DYNFIELD(mbuf, ctx->impl.dynfield_offset, rte_mbuf_timestamp_t*) = ns;

  return mbuf;
}

void ut_ptp_free_mbuf(void* mbuf) {
  rte_pktmbuf_free((struct rte_mbuf*)mbuf);
}

bool ut_ptp_mbuf_is_unpatched_ice_state(const void* mbuf_ptr) {
  const struct rte_mbuf* mbuf = mbuf_ptr;

  if ((mbuf->packet_type & RTE_PTYPE_L2_MASK) == RTE_PTYPE_L2_ETHER_TIMESYNC)
    return false;
  if (mbuf->timesync != 0) return false;
  if (mbuf->ol_flags & (RTE_MBUF_F_RX_IEEE1588_PTP | RTE_MBUF_F_RX_IEEE1588_TMST))
    return false;

  return true;
}

uint64_t ut_ptp_mbuf_time_stamp(ut_ptp_test_ctx* ctx, void* mbuf) {
  return g_real_mt_mbuf_time_stamp(&ctx->impl, (struct rte_mbuf*)mbuf, MTL_PORT_P);
}
