/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "gstreamer/gst_mtl_common_harness.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The plugin's meson.build turns these off too (plugin_c_args). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#define clock_gettime ut_gst_clock_gettime
#include "../../../ecosystem/gstreamer_plugin/gst_mtl_common.c"
#undef clock_gettime
#pragma GCC diagnostic pop

bool ut_gst_parse_general_arguments(bool enable_onboard_ptp,
                                    struct mtl_init_params* out) {
  GeneralArgs args;

  memset(&args, 0, sizeof(args));
  memset(out, 0, sizeof(*out));
  (void)snprintf(args.port[MTL_PORT_P], sizeof(args.port[MTL_PORT_P]), "0000:01:00.0");
  (void)snprintf(args.local_ip_string[MTL_PORT_P],
                 sizeof(args.local_ip_string[MTL_PORT_P]), "192.0.2.1");
  args.enable_onboard_ptp = enable_onboard_ptp;

  return gst_mtl_common_parse_general_arguments(out, &args) != FALSE;
}
