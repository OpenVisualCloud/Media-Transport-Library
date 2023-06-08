/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <unistd.h>

#ifndef _MT_UDP_PRELOAD_PLATFORM_H_
#define _MT_UDP_PRELOAD_PLATFORM_H_

#ifdef WINDOWSENV /* Windows */
#include <Winsock2.h>
#include <ws2tcpip.h>
#else /* Linux */
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#ifdef WINDOWSENV
typedef unsigned long int nfds_t;

static inline pid_t getpid() { return GetCurrentProcessId(); }
#endif

#endif
