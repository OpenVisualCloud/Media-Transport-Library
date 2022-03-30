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

#ifndef _ST_LIB_PLATFORM_HEAD_H_
#define _ST_LIB_PLATFORM_HEAD_H_

#ifdef WINDOWSENV /* Windows */
#include "win_posix.h"
#else /* Linux */
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <numa.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>

/* pcapng only available from DPDK 21.11 */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#include <rte_pcapng.h>
#define ST_PCAPNG_ENABLED
#endif

#endif

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#ifdef WINDOWSENV
#define ST_FLOCK_PATH "c:/temp/kahawai_lcore.lock"
#else
#define ST_FLOCK_PATH "/tmp/kahawai_lcore.lock"
#endif

#endif
