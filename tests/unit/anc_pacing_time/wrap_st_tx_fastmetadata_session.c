/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_tx_fastmetadata_session.c"

/* The fmd helper returns double; the production caller assigns the
 * result to a uint64_t (meta->timestamp). Mirror that conversion
 * here so the test asserts on the value actually stored. */
uint64_t test_fmd_pacing_time(double frame_time, uint64_t epochs) {
  struct st_tx_fastmetadata_session_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time = frame_time;
  return (uint64_t)tx_fastmetadata_pacing_time(&pacing, epochs);
}

uint32_t test_fmd_pacing_time_stamp(double frame_time_sampling, uint64_t epochs) {
  struct st_tx_fastmetadata_session_pacing pacing;
  memset(&pacing, 0, sizeof(pacing));
  pacing.frame_time_sampling = frame_time_sampling;
  return tx_fastmetadata_pacing_time_stamp(&pacing, epochs);
}
