/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Free-function overrides for rte_eth_timesync_adjust_{time,freq} that forward
 * into the active TimesyncMock. See timesync_mock.h for the contract.
 */

#include "ptp/timesync_mock.h"

#include <cassert>

namespace {
TimesyncMock* g_timesync_mock = nullptr;
}

TimesyncMock::TimesyncMock() {
  /* a leftover installed mock means a previous instance outlived its scope */
  assert(g_timesync_mock == nullptr);
  g_timesync_mock = this;
}

TimesyncMock::~TimesyncMock() {
  g_timesync_mock = nullptr;
}

extern "C" int rte_eth_timesync_adjust_time(uint16_t port_id, int64_t delta) {
  return g_timesync_mock ? g_timesync_mock->adjust_time(port_id, delta) : 0;
}

extern "C" int rte_eth_timesync_adjust_freq(uint16_t port_id, int64_t ppm) {
  return g_timesync_mock ? g_timesync_mock->adjust_freq(port_id, ppm) : 0;
}
