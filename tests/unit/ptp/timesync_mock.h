/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * GoogleMock seam for the DPDK ethdev PHC-discipline calls that
 * `ptp_adjust_delta` reaches when no_timesync=false. gmock cannot intercept
 * a C free function on its own, so the matching `extern "C"` definitions in
 * timesync_mock.cpp forward into the currently-installed TimesyncMock (the
 * gmock "Mocking Free Functions" idiom). The executable-local definitions
 * win over libdpdk via -Wl,--allow-multiple-definition.
 *
 * Construct a TimesyncMock in a test to make it the active target; its ctor
 * installs it and its dtor uninstalls and verifies expectations. Only one
 * instance may be live at a time (the tests are single-threaded).
 */

#ifndef _UT_TIMESYNC_MOCK_H_
#define _UT_TIMESYNC_MOCK_H_

#include <gmock/gmock.h>

#include <cstdint>

class TimesyncMock {
 public:
  TimesyncMock();
  ~TimesyncMock();

  MOCK_METHOD(int, adjust_time, (uint16_t port_id, int64_t delta));
  MOCK_METHOD(int, adjust_freq, (uint16_t port_id, int64_t ppm));
};

#endif /* _UT_TIMESYNC_MOCK_H_ */
