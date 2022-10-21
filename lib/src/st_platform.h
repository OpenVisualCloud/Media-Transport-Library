/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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

#ifdef ST_HAS_AVX512
#include <immintrin.h>
#endif

#ifndef ST_DISABLE_PCAPNG
/* pcapng only available from DPDK 21.11 */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#include <rte_pcapng.h>
#define ST_PCAPNG_ENABLED
#endif
#endif

#endif /* end of WINDOWSENV */

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

static inline int st_pthread_mutex_init(pthread_mutex_t* mutex,
                                        pthread_mutexattr_t* attr) {
  return pthread_mutex_init(mutex, attr);
}

static inline int st_pthread_mutex_lock(pthread_mutex_t* mutex) {
  return pthread_mutex_lock(mutex);
}

static inline int st_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  return pthread_mutex_unlock(mutex);
}

static inline int st_pthread_mutex_destroy(pthread_mutex_t* mutex) {
  return pthread_mutex_destroy(mutex);
}

static inline int st_pthread_cond_init(pthread_cond_t* cond,
                                       pthread_condattr_t* cond_attr) {
  return pthread_cond_init(cond, cond_attr);
}

static inline int st_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  return pthread_cond_wait(cond, mutex);
}

static inline int st_pthread_cond_destroy(pthread_cond_t* cond) {
  return pthread_cond_destroy(cond);
}

static inline int st_pthread_cond_signal(pthread_cond_t* cond) {
  return pthread_cond_signal(cond);
}

static inline bool st_socket_match(int cpu_socket, int dev_socket) {
#ifdef WINDOWSENV
  return true;  // windows cpu socket always 0
#else
  return (cpu_socket == dev_socket);
#endif
}
#endif
