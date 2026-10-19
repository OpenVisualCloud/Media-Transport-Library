/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "pipeline/st_plugin_harness.h"

#include <stdbool.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "st2110/pipeline/st_plugin.h"

struct ut_plugin_ctx {
  struct mtl_main_impl impl;
  st22_encoder_dev_handle encoder;
  st22_decoder_dev_handle decoder;
  st20_converter_dev_handle converter;
  bool fail_create;
  void* frame_in_create;
};

static void* ut_plugin_create_session(struct ut_plugin_ctx* ctx, enum ut_plugin_kind kind,
                                      void* session_p) {
  ctx->frame_in_create = ut_plugin_get_frame(kind, session_p);
  return ctx->fail_create ? NULL : ctx;
}

static st22_encode_priv ut_encoder_create_session(void* priv,
                                                  st22p_encode_session session_p,
                                                  struct st22_encoder_create_req* req) {
  req->max_codestream_size = UT_PLUGIN_RESP_MAX_SIZE;
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_plugin_create_session(priv, UT_PLUGIN_ENCODER, session_p);
}

static st22_decode_priv ut_decoder_create_session(void* priv,
                                                  st22p_decode_session session_p,
                                                  struct st22_decoder_create_req* req) {
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_plugin_create_session(priv, UT_PLUGIN_DECODER, session_p);
}

static st20_convert_priv ut_converter_create_session(
    void* priv, st20p_convert_session session_p, struct st20_converter_create_req* req) {
  (void)req;
  return ut_plugin_create_session(priv, UT_PLUGIN_CONVERTER, session_p);
}

static int ut_plugin_free_session(void* priv, void* session) {
  (void)priv;
  (void)session;
  return 0;
}

static int ut_converter_notify_frame_available(st20_convert_priv session) {
  (void)session;
  return 0;
}

static struct st22_encode_frame_meta* ut_encode_get_frame(void* priv) {
  return priv;
}

static struct st22_decode_frame_meta* ut_decode_get_frame(void* priv) {
  return priv;
}

static struct st20_convert_frame_meta* ut_convert_get_frame(void* priv) {
  return priv;
}

ut_plugin_ctx* ut_plugin_create(void) {
  if (ut_eal_init() < 0) return NULL;

  ut_plugin_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.inf[MTL_PORT_P].socket_id = (int)rte_socket_id();
  mt_pthread_mutex_init(&ctx->impl.plugin_mgr.lock, NULL);

  struct st22_encoder_dev e_dev = {
      .name = "ut_encoder",
      .priv = ctx,
      .input_fmt_caps = UINT64_MAX,
      .output_fmt_caps = UINT64_MAX,
      .create_session = ut_encoder_create_session,
      .free_session = ut_plugin_free_session,
  };
  struct st22_decoder_dev d_dev = {
      .name = "ut_decoder",
      .priv = ctx,
      .input_fmt_caps = UINT64_MAX,
      .output_fmt_caps = UINT64_MAX,
      .create_session = ut_decoder_create_session,
      .free_session = ut_plugin_free_session,
  };
  struct st20_converter_dev c_dev = {
      .name = "ut_converter",
      .priv = ctx,
      .input_fmt_caps = UINT64_MAX,
      .output_fmt_caps = UINT64_MAX,
      .create_session = ut_converter_create_session,
      .notify_frame_available = ut_converter_notify_frame_available,
      .free_session = ut_plugin_free_session,
  };
  ctx->encoder = st22_encoder_register(&ctx->impl, &e_dev);
  ctx->decoder = st22_decoder_register(&ctx->impl, &d_dev);
  ctx->converter = st20_converter_register(&ctx->impl, &c_dev);
  if (!ctx->encoder || !ctx->decoder || !ctx->converter) {
    ut_plugin_destroy(ctx);
    return NULL;
  }
  return ctx;
}

void ut_plugin_destroy(ut_plugin_ctx* ctx) {
  if (!ctx) return;
  if (ctx->converter) st20_converter_unregister(ctx->converter);
  if (ctx->decoder) st22_decoder_unregister(ctx->decoder);
  if (ctx->encoder) st22_encoder_unregister(ctx->encoder);
  mt_pthread_mutex_destroy(&ctx->impl.plugin_mgr.lock);
  free(ctx);
}

void ut_plugin_fail_create(ut_plugin_ctx* ctx) {
  ctx->fail_create = true;
}

void* ut_plugin_get(ut_plugin_ctx* ctx, enum ut_plugin_kind kind, void* frame) {
  switch (kind) {
    case UT_PLUGIN_ENCODER: {
      struct st22_get_encoder_request req = {.priv = frame,
                                             .get_frame = ut_encode_get_frame};
      return st22_get_encoder(&ctx->impl, &req);
    }
    case UT_PLUGIN_DECODER: {
      struct st22_get_decoder_request req = {.priv = frame,
                                             .get_frame = ut_decode_get_frame};
      return st22_get_decoder(&ctx->impl, &req);
    }
    case UT_PLUGIN_CONVERTER: {
      struct st20_get_converter_request req = {.priv = frame,
                                               .get_frame = ut_convert_get_frame};
      return st20_get_converter(&ctx->impl, &req);
    }
  }
  return NULL;
}

void ut_plugin_put(ut_plugin_ctx* ctx, enum ut_plugin_kind kind, void* session) {
  switch (kind) {
    case UT_PLUGIN_ENCODER:
      st22_put_encoder(&ctx->impl, session);
      break;
    case UT_PLUGIN_DECODER:
      st22_put_decoder(&ctx->impl, session);
      break;
    case UT_PLUGIN_CONVERTER:
      st20_put_converter(&ctx->impl, session);
      break;
  }
}

void* ut_plugin_get_frame(enum ut_plugin_kind kind, void* session) {
  switch (kind) {
    case UT_PLUGIN_ENCODER:
      return st22_encoder_get_frame(session);
    case UT_PLUGIN_DECODER:
      return st22_decoder_get_frame(session);
    case UT_PLUGIN_CONVERTER:
      return st20_converter_get_frame(session);
  }
  return NULL;
}

void* ut_plugin_frame_in_create(ut_plugin_ctx* ctx) {
  return ctx->frame_in_create;
}

size_t ut_plugin_encoder_max_size(void* session) {
  return ((struct st22_encode_session_impl*)session)->codestream_max_size;
}

uint32_t ut_plugin_encoder_resp_flag(void* session) {
  return ((struct st22_encode_session_impl*)session)->req.req.resp_flag;
}

uint32_t ut_plugin_decoder_resp_flag(void* session) {
  return ((struct st22_decode_session_impl*)session)->req.req.resp_flag;
}
