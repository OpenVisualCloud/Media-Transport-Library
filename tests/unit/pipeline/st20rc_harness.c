/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST20 redundant-combined RX (st20rc) unit tests. Includes
 * the production .c so static rx_st20rc_frame_ready() is reachable; its
 * st20_rx_* calls resolve to the session compiled in by session/st20_harness.c.
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "st2110/experimental/st20_redundant_combined_rx.c"

struct ut20rc_ctx {
  struct st20rc_rx_ctx rc;
  struct st20rc_rx_transport transport;
};

#include "pipeline/st20rc_harness.h"

ut20rc_ctx* ut20rc_ctx_create(st20_rx_handle handle) {
  ut20rc_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->rc.type = MT_HANDLE_RX_VIDEO_R;
  ctx->rc.ready = false;
  mt_pthread_mutex_init(&ctx->rc.lock, NULL);
  ctx->transport.port = MTL_SESSION_PORT_P;
  ctx->transport.parent = &ctx->rc;
  rte_spinlock_init(&ctx->transport.pending_put_lock);
  ctx->transport.handle = handle;
  ctx->rc.transport[MTL_SESSION_PORT_P] = &ctx->transport;
  return ctx;
}

void ut20rc_ctx_destroy(ut20rc_ctx* ctx) {
  if (!ctx) return;
  mt_pthread_mutex_destroy(&ctx->rc.lock);
  free(ctx);
}

int ut20rc_frame_ready(ut20rc_ctx* ctx, void* frame, struct st20_rx_frame_meta* meta) {
  return rx_st20rc_frame_ready(&ctx->transport, frame, meta);
}

void ut20rc_set_handle(ut20rc_ctx* ctx, st20_rx_handle handle) {
  rx_st20rc_set_handle(&ctx->transport, handle);
}
