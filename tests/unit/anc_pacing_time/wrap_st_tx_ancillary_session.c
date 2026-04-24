/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bridges for the two pacing-time helpers that should behave the
 * same way but historically diverge: ancillary uses nextafter() to
 * round up before the cast to uint64_t; fast-metadata returns a raw
 * double from the multiply.
 *
 * Each .c file is dragged in via its own wrapper TU. --gc-sections
 * drops the rest. Bridges return uint64_t so the C++ test can assert
 * monotonicity on the value the production callers ultimately store
 * in meta->timestamp (which IS a uint64_t).
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_tx_ancillary_session.c"

uint64_t test_anc_pacing_time(double frame_time, uint64_t epochs) {
  struct st_tx_ancillary_session_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time = frame_time;
  return tx_ancillary_pacing_time(&pacing, epochs);
}

uint32_t test_anc_pacing_time_stamp(double frame_time_sampling, uint64_t epochs) {
  struct st_tx_ancillary_session_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time_sampling = frame_time_sampling;
  return tx_ancillary_pacing_time_stamp(&pacing, epochs);
}
