/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the TSC calibration unit tests: mt_calibrate_tsc() runs against
 * a simulated TSC and monotonic clock, so no real time passes.
 */

#ifndef _UT_TSC_CALIBRATE_HARNESS_H_
#define _UT_TSC_CALIBRATE_HARNESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run mt_calibrate_tsc() on a simulated TSC ticking at tsc_hz, where each clock read
 * takes read_ns and the first read after a sleep takes cold_read_ns more. Returns the
 * tsc_hz it calibrated. */
uint64_t ut_tsc_calibrate(uint64_t tsc_hz, uint64_t read_ns, uint64_t cold_read_ns);

#ifdef __cplusplus
}
#endif

#endif
