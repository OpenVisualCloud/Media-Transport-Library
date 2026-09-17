/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_GSTREAMER_GST_MTL_ST20P_TX_HARNESS_H
#define TESTS_UNIT_GSTREAMER_GST_MTL_ST20P_TX_HARNESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One case per exit of gst_mtl_st20p_tx_session_create(), named after what makes it
 * take that exit. The harness owns the GstVideoInfo the plugin sees, so the cases
 * before PORT_UNSET are properties of that info. */
enum ut_gst_st20p_tx_case {
  UT_GST_ST20P_TX_OK,
  UT_GST_ST20P_TX_NO_MTL_HANDLE,
  UT_GST_ST20P_TX_INFO_NULL,
  UT_GST_ST20P_TX_INTERLACE_UNSUPPORTED,
  UT_GST_ST20P_TX_FORMAT_UNSUPPORTED,
  UT_GST_ST20P_TX_FPS_OFF_TABLE,
  UT_GST_ST20P_TX_FPS_DEN_ZERO,
  UT_GST_ST20P_TX_PORT_UNSET,
  UT_GST_ST20P_TX_CREATE_FAILS,
};

struct ut_gst_st20p_tx_result {
  bool created;    /* what gst_mtl_st20p_tx_session_create() returned */
  int info_allocs; /* GstVideoInfo the harness handed to the plugin */
  int info_frees;  /* GstVideoInfo the plugin handed back */
  bool handle_set; /* sink->tx_handle after the call */
};

void ut_gst_st20p_tx_session_create(enum ut_gst_st20p_tx_case c,
                                    struct ut_gst_st20p_tx_result* out);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_GSTREAMER_GST_MTL_ST20P_TX_HARNESS_H */
