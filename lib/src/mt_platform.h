/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_PLATFORM_HEAD_H_
#define _MT_LIB_PLATFORM_HEAD_H_

#ifdef WINDOWSENV /* Windows */
#include "win_posix.h"
#else /* Linux */
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/udp.h>
#include <numa.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>

#ifdef MTL_HAS_AVX512
#include <immintrin.h>
#endif

#ifndef MTL_DISABLE_PCAPNG
/* pcapng only available from DPDK 21.11 */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#include <rte_pcapng.h>
#define ST_PCAPNG_ENABLED
#endif
#endif

#endif /* end of WINDOWSENV */

#ifdef CLOCK_MONOTONIC_RAW
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#ifdef WINDOWSENV
typedef unsigned long int nfds_t;
#endif

#ifndef POLLIN /* For windows */
/* There is data to read */
#define POLLIN 0x001
#endif

#ifndef MSG_DONTWAIT /* For windows */
#define MSG_DONTWAIT (0x40)
#endif

#ifdef WINDOWSENV
#define MT_THREAD_TIMEDWAIT_CLOCK_ID CLOCK_REALTIME
#else
/* use CLOCK_MONOTONIC for mt_pthread_cond_timedwait */
#define MT_THREAD_TIMEDWAIT_CLOCK_ID CLOCK_MONOTONIC
#endif

#ifdef WINDOWSENV
#define MT_FLOCK_PATH "c:/temp/kahawai_lcore.lock"
#else
#define MT_FLOCK_PATH "/tmp/kahawai_lcore.lock"
#endif

static inline int mt_pthread_mutex_init(pthread_mutex_t* mutex,
                                        pthread_mutexattr_t* attr) {
  return pthread_mutex_init(mutex, attr);
}

static inline int mt_pthread_mutex_lock(pthread_mutex_t* mutex) {
  return pthread_mutex_lock(mutex);
}

static inline int mt_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  return pthread_mutex_unlock(mutex);
}

static inline int mt_pthread_mutex_destroy(pthread_mutex_t* mutex) {
  return pthread_mutex_destroy(mutex);
}

static inline int mt_pthread_cond_init(pthread_cond_t* cond,
                                       pthread_condattr_t* cond_attr) {
  return pthread_cond_init(cond, cond_attr);
}

static inline int mt_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  return pthread_cond_wait(cond, mutex);
}

static inline int mt_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                            const struct timespec* time) {
  return pthread_cond_timedwait(cond, mutex, time);
}

static inline int mt_pthread_cond_destroy(pthread_cond_t* cond) {
  return pthread_cond_destroy(cond);
}

static inline int mt_pthread_cond_signal(pthread_cond_t* cond) {
  return pthread_cond_signal(cond);
}

static inline bool mt_socket_match(int cpu_socket, int dev_socket) {
#ifdef WINDOWSENV
  return true;  // windows cpu socket always 0
#else
  return (cpu_socket == dev_socket);
#endif
}
#endif
