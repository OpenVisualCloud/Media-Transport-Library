/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bridge for video_trs_burst_fail (st_video_transmitter.c). The
 * helper detects sustained TX burst failure and, once the hang
 * threshold is exceeded, escalates via st20_tx_queue_fatal_error
 * and returns nb_pkts so the caller drops the batch instead of
 * spinning forever on a wedged queue.
 *
 * The bridge fakes mt_get_tsc to a controllable counter and counts
 * st20_tx_queue_fatal_error invocations so the test asserts both
 * the return-value contract and the "fatal error escalated" side
 * effect across the threshold boundary.
 */

#include <stdint.h>
#include <string.h>

/* Pull mt_main.h first so its `static inline mt_get_tsc()` definition
 * is parsed unaltered. THEN install a macro that intercepts every
 * textual call inside the production .c we drag in below. The static
 * inline body itself never calls mt_get_tsc, so leaving it intact is
 * harmless; the linker drops it via --gc-sections. */
#include "../../../lib/src/mt_main.h"

static uint64_t test_get_tsc_ns(struct mtl_main_impl* impl);
#define mt_get_tsc(impl) test_get_tsc_ns(impl)

#include "../../../lib/src/st2110/st_video_transmitter.c"

#undef mt_get_tsc

static uint64_t g_fake_tsc_ns;
static int g_fatal_calls;

static uint64_t test_get_tsc_ns(struct mtl_main_impl* impl) {
  (void)impl;
  return g_fake_tsc_ns;
}

int st20_tx_queue_fatal_error(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port) {
  (void)impl;
  (void)s;
  (void)s_port;
  g_fatal_calls++;
  return 0;
}

void test_burst_fail_reset(void) {
  g_fake_tsc_ns = 0;
  g_fatal_calls = 0;
}

void test_burst_fail_set_tsc(uint64_t tsc) {
  g_fake_tsc_ns = tsc;
}

int test_burst_fail_fatal_calls(void) {
  return g_fatal_calls;
}

uint16_t test_burst_fail_run(uint64_t last_succ_tsc, uint64_t threshold_ns,
                             uint16_t nb_pkts, uint64_t* out_last_succ_after) {
  struct st_tx_video_session_impl s;
  memset(&s, 0, sizeof(s));
  s.idx = 0;
  s.last_burst_succ_time_tsc[MTL_SESSION_PORT_P] = last_succ_tsc;
  s.tx_hang_detect_time_thresh = threshold_ns;
  uint16_t r = video_trs_burst_fail(NULL, &s, MTL_SESSION_PORT_P, nb_pkts);
  *out_last_succ_after = s.last_burst_succ_time_tsc[MTL_SESSION_PORT_P];
  return r;
}
