/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_GSTREAMER_GST_MTL_COMMON_HARNESS_H
#define TESTS_UNIT_GSTREAMER_GST_MTL_COMMON_HARNESS_H

#include <mtl/mtl_api.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Replaces clock_gettime() inside the plugin source; the test defines it. */
int ut_gst_clock_gettime(clockid_t clock_id, struct timespec* ts);

/**
 * Run the plugin's dev-argument parser on one valid port, keeping the GStreamer and GLib
 * headers out of the C++ test.
 *
 * @param enable_onboard_ptp Value of the enable-ptp property.
 * @param out Zeroed as gst_mtl_common_init_handle() zeroes it, then filled by the parser.
 * @return Parser verdict: true on success.
 */
bool ut_gst_parse_general_arguments(bool enable_onboard_ptp, struct mtl_init_params* out);

#ifdef __cplusplus
}
#endif

#endif
