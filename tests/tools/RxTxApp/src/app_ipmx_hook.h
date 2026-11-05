/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _APP_IPMX_HOOK_HEAD_H_
#define _APP_IPMX_HOOK_HEAD_H_

#ifdef APP_HAS_IPMX
#include "ipmx/app_ipmx.h"
#else

#include "app_base.h"
#include "log.h"

struct app_ipmx_tx;

static inline int app_ipmx_no_support(const char* opt) {
  err("%s needs RxTxApp built against an IPMX enabled MTL\n", opt);
  return -ENOTSUP;
}

static inline int app_ipmx_set_sdp_dir(struct st_app_context* ctx, const char* dir) {
  MTL_MAY_UNUSED(ctx);
  MTL_MAY_UNUSED(dir);
  return app_ipmx_no_support("--ipmx_sdp_dir");
}

static inline int app_ipmx_set_ts_refclk(struct st_app_context* ctx,
                                         const char* ts_refclk) {
  MTL_MAY_UNUSED(ctx);
  MTL_MAY_UNUSED(ts_refclk);
  return app_ipmx_no_support("--ipmx_ts_refclk");
}

static inline int app_ipmx_tx_st20p_init(struct st_app_context* ctx,
                                         st_json_st20p_session_t* st20p,
                                         struct st_app_tx_st20p_session* s,
                                         const char* name) {
  MTL_MAY_UNUSED(ctx);
  MTL_MAY_UNUSED(st20p);
  MTL_MAY_UNUSED(s);
  MTL_MAY_UNUSED(name);
  return 0;
}

static inline int app_ipmx_tx_st22p_init(struct st_app_context* ctx,
                                         st_json_st22p_session_t* st22p,
                                         struct st_app_tx_st22p_session* s,
                                         const char* name) {
  MTL_MAY_UNUSED(ctx);
  MTL_MAY_UNUSED(st22p);
  MTL_MAY_UNUSED(s);
  MTL_MAY_UNUSED(name);
  return 0;
}

static inline void app_ipmx_tx_uinit(struct app_ipmx_tx** ipmx) {
  MTL_MAY_UNUSED(ipmx);
}

static inline void app_ipmx_tx_poll(struct st_app_context* ctx) {
  MTL_MAY_UNUSED(ctx);
}

static inline void app_ipmx_rx_st20p_result(struct st_app_rx_st20p_session* s) {
  MTL_MAY_UNUSED(s);
}

static inline void app_ipmx_rx_st22p_result(struct st_app_rx_st22p_session* s) {
  MTL_MAY_UNUSED(s);
}

#endif

#endif
