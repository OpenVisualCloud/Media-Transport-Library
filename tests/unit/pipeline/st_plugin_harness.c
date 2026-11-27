/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#undef MTL_HAS_USDT
#include "pipeline/st_plugin_harness.h"

#include "common/ut_common.h"
#include "st2110/pipeline/st_plugin.c"

static struct st22_encode_dev_impl ut_encoder;
static struct st22_decode_dev_impl ut_decoder;
static struct st20_convert_dev_impl ut_converter;
static bool ut_fail_create;
static void* ut_frame_in_create;

static void* ut_create_session(enum ut_plugin_kind kind, void* session_p) {
  ut_frame_in_create = ut_plugin_get_frame(kind, session_p);
  return ut_fail_create ? NULL : session_p;
}

static st22_encode_priv ut_encoder_create_session(void* priv,
                                                  st22p_encode_session session_p,
                                                  struct st22_encoder_create_req* req) {
  (void)priv;
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_create_session(UT_PLUGIN_ENCODER, session_p);
}

static st22_decode_priv ut_decoder_create_session(void* priv,
                                                  st22p_decode_session session_p,
                                                  struct st22_decoder_create_req* req) {
  (void)priv;
  req->resp_flag = UT_PLUGIN_RESP_FLAG;
  return ut_create_session(UT_PLUGIN_DECODER, session_p);
}

static st20_convert_priv ut_converter_create_session(
    void* priv, st20p_convert_session session_p, struct st20_converter_create_req* req) {
  (void)priv;
  (void)req;
  return ut_create_session(UT_PLUGIN_CONVERTER, session_p);
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

void ut_plugin_reset(void) {
  memset(&ut_encoder, 0, sizeof(ut_encoder));
  memset(&ut_decoder, 0, sizeof(ut_decoder));
  memset(&ut_converter, 0, sizeof(ut_converter));
  ut_encoder.dev.name = "ut_encoder";
  ut_encoder.dev.create_session = ut_encoder_create_session;
  ut_decoder.dev.name = "ut_decoder";
  ut_decoder.dev.create_session = ut_decoder_create_session;
  ut_converter.dev.name = "ut_converter";
  ut_converter.dev.create_session = ut_converter_create_session;
  ut_fail_create = false;
  ut_frame_in_create = NULL;
}

void ut_plugin_fail_create(void) {
  ut_fail_create = true;
}

void* ut_plugin_get(enum ut_plugin_kind kind, void* frame) {
  switch (kind) {
    case UT_PLUGIN_ENCODER: {
      struct st22_get_encoder_request req = {.priv = frame,
                                             .get_frame = ut_encode_get_frame};
      return st22_get_encoder_session(&ut_encoder, &req);
    }
    case UT_PLUGIN_DECODER: {
      struct st22_get_decoder_request req = {.priv = frame,
                                             .get_frame = ut_decode_get_frame};
      return st22_get_decoder_session(&ut_decoder, &req);
    }
    case UT_PLUGIN_CONVERTER: {
      struct st20_get_converter_request req = {.priv = frame,
                                               .get_frame = ut_convert_get_frame};
      return st20_get_converter_session(&ut_converter, &req);
    }
  }
  return NULL;
}

void ut_plugin_put(enum ut_plugin_kind kind, void* session) {
  switch (kind) {
    case UT_PLUGIN_ENCODER:
      ((struct st22_encode_session_impl*)session)->session = NULL;
      break;
    case UT_PLUGIN_DECODER:
      ((struct st22_decode_session_impl*)session)->session = NULL;
      break;
    case UT_PLUGIN_CONVERTER:
      ((struct st20_convert_session_impl*)session)->session = NULL;
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

void* ut_plugin_frame_in_create(void) {
  return ut_frame_in_create;
}

unsigned int ut_plugin_resp_flag(enum ut_plugin_kind kind, void* session) {
  if (kind == UT_PLUGIN_ENCODER)
    return ((struct st22_encode_session_impl*)session)->req.req.resp_flag;
  return ((struct st22_decode_session_impl*)session)->req.req.resp_flag;
}

static struct mtl_main_impl ut_impl;
static struct st_dl_plugin_impl ut_slot_taken;

void* ut_plugin_mgr_init(void) {
  ut_eal_init();
  memset(&ut_impl, 0, sizeof(ut_impl));
  ut_impl.type = MT_HANDLE_MAIN;
  mt_pthread_mutex_init(&ut_impl.plugin_mgr.lock, NULL);
  mt_pthread_mutex_init(&ut_impl.plugin_mgr.plugins_lock, NULL);
  return &ut_impl;
}

const char* ut_plugin_so_path(enum ut_plugin_so so) {
  switch (so) {
    case UT_SO_PLAIN:
      return UT_PLUGIN_SO;
    case UT_SO_KEEP_DEV:
      return UT_PLUGIN_KEEP_DEV_SO;
    case UT_SO_FAIL_CREATE:
      return UT_PLUGIN_FAIL_CREATE_SO;
    case UT_SO_BUSY:
      return UT_PLUGIN_BUSY_SO;
    case UT_SO_FAIL_CREATE_BUSY:
      return UT_PLUGIN_FAIL_CREATE_BUSY_SO;
  }
  return NULL;
}

int ut_plugin_mgr_encoders(void) {
  int cnt = 0;

  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++)
    if (ut_impl.plugin_mgr.encode_devs[i]) cnt++;
  return cnt;
}

void* ut_plugin_mgr_get_encoder(void) {
  struct st22_get_encoder_request req = {
      .device = ST_PLUGIN_DEVICE_AUTO,
      .req = {.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
              .output_fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM},
  };

  return st22_get_encoder(&ut_impl, &req);
}

void ut_plugin_mgr_put_encoder(void* session) {
  st22_put_encoder(&ut_impl, session);
}

int ut_plugin_mgr_uinit(void) {
  return st_plugins_uinit(&ut_impl);
}

void ut_plugin_mgr_hold_encoder(void) {
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    if (!ut_impl.plugin_mgr.encode_devs[i]) continue;
    rte_atomic32_inc(&ut_impl.plugin_mgr.encode_devs[i]->ref_cnt);
    return;
  }
}

void ut_plugin_mgr_fill_slots(bool full) {
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++)
    ut_impl.plugin_mgr.plugins[i] = full ? &ut_slot_taken : NULL;
}
