/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "main/mt_instance_harness.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include "mt_instance.h"
#include "mt_main.h"

struct ut_instance_ctx {
  struct mtl_main_impl impl;
};

ut_instance_ctx* ut_instance_create_orphan(void) {
  int fds[2];
  ut_instance_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
    free(ctx);
    return NULL;
  }
  close(fds[1]);
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.instance_fd = fds[0];
  return ctx;
}

void ut_instance_destroy(ut_instance_ctx* ctx) {
  if (!ctx) return;
  close(ctx->impl.instance_fd);
  free(ctx);
}

int ut_instance_put_lcore(ut_instance_ctx* ctx, unsigned int lcore) {
  return mt_instance_put_lcore(&ctx->impl, lcore);
}

int ut_instance_request_xsks_map_fd(ut_instance_ctx* ctx, unsigned int ifindex) {
  return mt_instance_request_xsks_map_fd(&ctx->impl, ifindex);
}
