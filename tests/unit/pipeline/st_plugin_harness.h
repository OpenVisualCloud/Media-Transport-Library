/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the st22_get_encoder()/st22_get_decoder()/st20_get_converter() unit
 * tests: one fake encoder, decoder and converter dev, whose create_session calls the
 * matching *_get_frame() on its session_p, as a worker started there may.
 */

#ifndef TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H
#define TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_plugin_ctx ut_plugin_ctx;

enum ut_plugin_kind {
  UT_PLUGIN_ENCODER,
  UT_PLUGIN_DECODER,
  UT_PLUGIN_CONVERTER,
};

/* What the fake encoder and decoder write back into the create request. */
#define UT_PLUGIN_RESP_MAX_SIZE ((size_t)0x2a2a)
#define UT_PLUGIN_RESP_FLAG ((uint32_t)0x5a5a0001)

ut_plugin_ctx* ut_plugin_create(void);
void ut_plugin_destroy(ut_plugin_ctx* ctx);
/* Every following create_session fails. */
void ut_plugin_fail_create(ut_plugin_ctx* ctx);

/* A session for a request whose get_frame returns frame; NULL if none is granted. */
void* ut_plugin_get(ut_plugin_ctx* ctx, enum ut_plugin_kind kind, void* frame);
void ut_plugin_put(ut_plugin_ctx* ctx, enum ut_plugin_kind kind, void* session);
void* ut_plugin_get_frame(enum ut_plugin_kind kind, void* session);
/* What the get_frame inside the last create_session returned. */
void* ut_plugin_frame_in_create(ut_plugin_ctx* ctx);

size_t ut_plugin_encoder_max_size(void* session);
uint32_t ut_plugin_encoder_resp_flag(void* session);
uint32_t ut_plugin_decoder_resp_flag(void* session);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H */
