/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for st22_get_encoder_session(), st22_get_decoder_session() and
 * st20_get_converter_session(): one fake dev of each, whose create_session calls the
 * matching *_get_frame() on its session_p, as a worker started there may.
 */

#ifndef TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H
#define TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ut_plugin_kind {
  UT_PLUGIN_ENCODER,
  UT_PLUGIN_DECODER,
  UT_PLUGIN_CONVERTER,
};

/* What the fake encoder and decoder write back into the create request. */
#define UT_PLUGIN_RESP_FLAG 0x5a5a0001u

/* Empties the fake devs; their create_session succeeds. */
void ut_plugin_reset(void);
/* Every following create_session fails. */
void ut_plugin_fail_create(void);
/* A session for a request whose get_frame returns frame; NULL if none is granted. */
void* ut_plugin_get(enum ut_plugin_kind kind, void* frame);
/* Frees the session's slot, as the put does. */
void ut_plugin_put(enum ut_plugin_kind kind, void* session);
void* ut_plugin_get_frame(enum ut_plugin_kind kind, void* session);
/* What the get_frame inside the last create_session returned. */
void* ut_plugin_frame_in_create(void);
/* The resp_flag in the encoder or decoder session's create request. */
unsigned int ut_plugin_resp_flag(enum ut_plugin_kind kind, void* session);

/* Empties the plugin manager of a fake instance and returns that instance. */
void* ut_plugin_mgr_init(void);
/* The plugin .so copies, see ut_st22_plugin.c. */
enum ut_plugin_so {
  UT_SO_PLAIN,
  UT_SO_KEEP_DEV,
  UT_SO_FAIL_CREATE,
  UT_SO_BUSY,
  UT_SO_FAIL_CREATE_BUSY,
};

const char* ut_plugin_so_path(enum ut_plugin_so so);
int ut_plugin_mgr_encoders(void);
/* A session on the .so's encoder dev, as st22p does; NULL if none is granted. */
void* ut_plugin_mgr_get_encoder(void);
void ut_plugin_mgr_put_encoder(void* session);
int ut_plugin_mgr_uinit(void);
/* Counts a session on the first encoder dev, as a get that raced its plugin's failed
 * create. */
void ut_plugin_mgr_hold_encoder(void);
/* Occupies (or frees) every plugin slot of the fake instance. */
void ut_plugin_mgr_fill_slots(bool full);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_PIPELINE_ST_PLUGIN_HARNESS_H */
