/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#pragma once

#ifdef WINDOWSENV /* Windows */
#include <Winsock2.h>
#include <ws2tcpip.h>
#ifndef sleep
#define sleep(x) Sleep(1000 * x)
#include <unistd.h>
#endif
#else /* Linux */
#include <arpa/inet.h>
#endif

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif
