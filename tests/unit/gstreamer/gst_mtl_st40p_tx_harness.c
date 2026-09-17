/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Drives gst_mtl_st40p_tx_start() with the production source compiled in place.
 * start() is where the ST 2110-40 sink decides whether the element may run, so the
 * seams are the four calls it makes into something the unit tier has no business
 * reaching, all renamed by #define so they stay in this translation unit:
 *   - gst_mtl_common_init_handle(), which would mtl_init() a real device;
 *   - st40p_tx_create(), which would need a NIC;
 *   - gst_base_sink_set_async_enabled() and gst_element_set_state(), which need a
 *     GObject the type system made. The sink here is a plain struct, so
 *     G_DISABLE_CAST_CHECKS (see meson.build) keeps GST_MTL_ST40P_TX() a cast.
 * Counting the set_state() calls is how a test tells "start() failed" from "start()
 * failed but pushed the element to PLAYING anyway".
 *
 * No gst_init(): see the note in gst_mtl_st20p_tx_harness.c.
 */

#include "gstreamer/gst_mtl_st40p_tx_harness.h"

#include <gst/base/gstbasesink.h>
#include <mtl/st40_pipeline_api.h>
#include <stdint.h>
#include <string.h>

#include "gst_mtl_common.h"

static enum ut_gst_st40p_tx_start_case ut_st40p_case;
static int ut_st40p_set_state_calls;
static int ut_st40p_create_calls;

/* Declared before the renames below so the plugin's calls land here, and defined
 * after them so this file still reaches the real functions. */
static mtl_handle ut_gst_mtl_common_init_handle(GeneralArgs* args, gboolean force_new);
static st40p_tx_handle ut_st40p_tx_create(mtl_handle mt, struct st40p_tx_ops* ops);
static void ut_gst_base_sink_set_async_enabled(GstBaseSink* sink, gboolean enabled);
static GstStateChangeReturn ut_gst_element_set_state(GstElement* element, GstState state);

#define gst_mtl_common_init_handle ut_gst_mtl_common_init_handle
#define st40p_tx_create ut_st40p_tx_create
#define gst_base_sink_set_async_enabled ut_gst_base_sink_set_async_enabled
#define gst_element_set_state ut_gst_element_set_state
#include "../../../ecosystem/gstreamer_plugin/gst_mtl_st40p_tx.c"
#undef gst_element_set_state
#undef gst_base_sink_set_async_enabled
#undef st40p_tx_create
#undef gst_mtl_common_init_handle

static mtl_handle ut_gst_mtl_common_init_handle(GeneralArgs* args, gboolean force_new) {
  (void)args;
  (void)force_new;
  if (ut_st40p_case == UT_GST_ST40P_TX_START_NO_MTL_HANDLE) return NULL;
  return (mtl_handle)(uintptr_t)0x2110; /* never dereferenced */
}

static st40p_tx_handle ut_st40p_tx_create(mtl_handle mt, struct st40p_tx_ops* ops) {
  (void)mt;
  (void)ops;
  ut_st40p_create_calls++;
  if (ut_st40p_case == UT_GST_ST40P_TX_START_CREATE_FAILS) return NULL;
  return (st40p_tx_handle)(uintptr_t)0x5740; /* never dereferenced */
}

static void ut_gst_base_sink_set_async_enabled(GstBaseSink* sink, gboolean enabled) {
  (void)sink;
  (void)enabled;
}

static GstStateChangeReturn ut_gst_element_set_state(GstElement* element,
                                                     GstState state) {
  (void)element;
  if (state == GST_STATE_PLAYING) ut_st40p_set_state_calls++;
  return GST_STATE_CHANGE_SUCCESS;
}

void ut_gst_st40p_tx_start(enum ut_gst_st40p_tx_start_case c,
                           struct ut_gst_st40p_tx_start_result* out) {
  Gst_Mtl_St40p_Tx sink;

  ut_st40p_case = c;
  ut_st40p_set_state_calls = 0;
  ut_st40p_create_calls = 0;

  memset(&sink, 0, sizeof(sink));
  sink.input_format = GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW;
  sink.fps_d = 1;
  sink.fps_n = (c == UT_GST_ST40P_TX_START_FPS_OFF_TABLE)
                   ? 48 /* outside every st_fps_timings window */
                   : 60;
  /* What a second READY -> PAUSED sees: nothing frees tx_handle before finalize. */
  if (c == UT_GST_ST40P_TX_START_RESTART)
    sink.tx_handle = (st40p_tx_handle)(uintptr_t)0x5740;

  if (c != UT_GST_ST40P_TX_START_PORT_UNSET) {
    strncpy(sink.generalArgs.port[MTL_PORT_P], "0000:af:01.0", MTL_PORT_MAX_LEN - 1);
    strncpy(sink.portArgs.session_ip_string[MTL_PORT_P], "239.168.85.20",
            MTL_PORT_MAX_LEN - 1);
    sink.portArgs.udp_port[MTL_PORT_P] = 20000;
    sink.portArgs.payload_type = PAYLOAD_TYPE_ANCILLARY;
  }

  out->started = gst_mtl_st40p_tx_start((GstBaseSink*)&sink) ? true : false;
  out->handle_set = sink.tx_handle != NULL;
  out->set_state_calls = ut_st40p_set_state_calls;
  out->create_calls = ut_st40p_create_calls;
}
