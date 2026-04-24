/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Wrapper that pulls lib/src/st2110/st_rx_timing_parser.c into the
 * unit-test binary so the static-inline helper rv_tp_calculate_avg
 * becomes reachable for unit testing. mt_log is shimmed out via the
 * meson -include; libmtl/DPDK externs only used by init/uinit code
 * paths are stubbed in fakes_mtl.c (shared with tp_stat_aggregate).
 *
 * Bridge: the C++ test TU calls tp_calc_avg(cnt, sum) which forwards
 * directly to the static-inline production function. No state is
 * carried between calls.
 */

#include <stdint.h>

#include "../../../lib/src/st2110/st_rx_timing_parser.c"

float tp_calc_avg(uint32_t cnt, int64_t sum) {
  return rv_tp_calculate_avg(cnt, sum);
}
