/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstdint>

#ifndef SESSION_SKIP_PORT
#define SESSION_SKIP_PORT -1
#endif

#ifndef VIDEO_CLOCK_HZ
#define VIDEO_CLOCK_HZ 90000
#endif

/* Largest first-packet hardware/PCIe jitter nightly CI has recorded so far
 * (e810 only; e830/e835 have not yet corroborated -- revise if evidence does). */
constexpr int64_t kNoCtxEvidencedFirstPacketJitterNs = 6831;
