/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Includes the production mt_main.c so the static mt_calibrate_tsc() is visible, with
 * its clock reads and sleep renamed onto a simulated clock. mtl_init() and mtl_uninit()
 * are renamed too, as the ffmpeg harness defines its own.
 */

/* mt_main.c needs pthread_setaffinity_np(), but ut_common.h pulls system headers first */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "main/tsc_calibrate_harness.h"

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_queue.h"
#include "dev/mt_dev.h"
#include "mt_admin.h"
#include "mt_arp.h"
#include "mt_cni.h"
#include "mt_config.h"
#include "mt_dhcp.h"
#include "mt_dma.h"
#include "mt_flow.h"
#include "mt_instance.h"
#include "mt_log.h"
#include "mt_main.h"
#include "mt_mcast.h"
#include "mt_ptp.h"
#include "mt_sch.h"
#include "mt_socket.h"
#include "mt_stat.h"
#include "mt_util.h"
#include "st2110/pipeline/st_plugin.h"

static struct {
  uint64_t tsc_hz;
  uint64_t read_ns;
  uint64_t cold_read_ns;
  uint64_t now_ns;
  bool cold;
} ut_clock;

static void ut_clock_read_done(void) {
  ut_clock.now_ns += ut_clock.read_ns;
  if (ut_clock.cold) ut_clock.now_ns += ut_clock.cold_read_ns;
  ut_clock.cold = false;
}

static uint64_t ut_monotonic_time(void) {
  uint64_t ns = ut_clock.now_ns;
  ut_clock_read_done();
  return ns;
}

static uint64_t ut_tsc_cycles(void) {
  uint64_t cycles = (unsigned __int128)ut_clock.now_ns * ut_clock.tsc_hz / NS_PER_S;
  ut_clock_read_done();
  return cycles;
}

static void ut_sleep_ms(unsigned int ms) {
  ut_clock.now_ns += (uint64_t)ms * NS_PER_MS;
  ut_clock.cold = true;
}

mtl_handle ut_tsc_calibrate_mtl_init(struct mtl_init_params* p);
int ut_tsc_calibrate_mtl_uninit(mtl_handle mt);

#define mt_get_monotonic_time ut_monotonic_time
#define rte_get_tsc_cycles ut_tsc_cycles
#define mt_sleep_ms ut_sleep_ms
#define mtl_init ut_tsc_calibrate_mtl_init
#define mtl_uninit ut_tsc_calibrate_mtl_uninit
#include "mt_main.c"
#undef mtl_uninit
#undef mtl_init
#undef mt_sleep_ms
#undef rte_get_tsc_cycles
#undef mt_get_monotonic_time

uint64_t ut_tsc_calibrate(uint64_t tsc_hz, uint64_t read_ns, uint64_t cold_read_ns) {
  struct mtl_main_impl* impl = calloc(1, sizeof(*impl));
  uint64_t calibrated;

  if (!impl) return 0;
  ut_clock.tsc_hz = tsc_hz;
  ut_clock.read_ns = read_ns;
  ut_clock.cold_read_ns = cold_read_ns;
  ut_clock.now_ns = 0;
  ut_clock.cold = false;
  mt_calibrate_tsc(impl);
  calibrated = impl->tsc_hz;
  free(impl);
  return calibrated;
}
