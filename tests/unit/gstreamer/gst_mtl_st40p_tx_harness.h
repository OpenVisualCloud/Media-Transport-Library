/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_GSTREAMER_GST_MTL_ST40P_TX_HARNESS_H
#define TESTS_UNIT_GSTREAMER_GST_MTL_ST40P_TX_HARNESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OK and RESTART, plus one case per rejection gst_mtl_st40p_tx_start() can hit with a
 * value its properties admit: the MTL handle it takes itself, and three of the seven
 * exits of the session create it calls. Of the other four, its MTL handle exit is
 * unreachable through start(), which checks the handle first; "tx_handle already
 * initialized" is RESTART, which start() treats as success; and the DID and SDID range
 * checks sit behind properties GObject clamps to 0xff. */
enum ut_gst_st40p_tx_start_case {
  UT_GST_ST40P_TX_START_OK,
  UT_GST_ST40P_TX_START_RESTART,
  UT_GST_ST40P_TX_START_NO_MTL_HANDLE,
  UT_GST_ST40P_TX_START_PORT_UNSET,
  UT_GST_ST40P_TX_START_FPS_OFF_TABLE,
  UT_GST_ST40P_TX_START_CREATE_FAILS,
};

struct ut_gst_st40p_tx_start_result {
  bool started;        /* what gst_mtl_st40p_tx_start() returned */
  bool handle_set;     /* sink->tx_handle after the call */
  int set_state_calls; /* gst_element_set_state(..., GST_STATE_PLAYING) calls */
  int create_calls;    /* st40p_tx_create() calls */
};

void ut_gst_st40p_tx_start(enum ut_gst_st40p_tx_start_case c,
                           struct ut_gst_st40p_tx_start_result* out);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_GSTREAMER_GST_MTL_ST40P_TX_HARNESS_H */
