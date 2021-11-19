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

#ifndef _ST_APP_PLATFORM_HEAD_H_
#define _ST_APP_PLATFORM_HEAD_H_

#ifdef WINDOWSENV /* Windows */
#include <Winsock2.h>
#include <ws2tcpip.h>

#include "win_posix.h"
#else /* Linux */
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <numa.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#endif

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#endif
