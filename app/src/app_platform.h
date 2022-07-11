/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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
#include <unistd.h>
#endif

#include <fcntl.h>
#include <stdio.h>

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

enum st_tx_frame_status {
  ST_TX_FRAME_FREE = 0,
  ST_TX_FRAME_READY,
  ST_TX_FRAME_IN_TRANSMITTING,
  ST_TX_FRAME_STATUS_MAX,
};

struct st_tx_frame {
  enum st_tx_frame_status stat;
  size_t size;
  bool second_field;    /* for interlaced mode */
  bool slice_trigger;   /* for slice */
  uint16_t lines_ready; /* for slice */
};

struct st_rx_frame {
  void* frame;
  size_t size;
};

static inline int st_pthread_mutex_init(pthread_mutex_t* mutex,
                                        pthread_mutexattr_t* attr) {
  return pthread_mutex_init(mutex, attr);
}

static inline int st_pthread_mutex_trylock(pthread_mutex_t* mutex) {
  return pthread_mutex_trylock(mutex);
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

static inline int st_open(const char* path, int flags) { return open(path, flags); }

static inline int st_open_mode(const char* path, int flags, mode_t mode) {
  return open(path, flags, mode);
}

static inline FILE* st_fopen(const char* path, const char* mode) {
  return fopen(path, mode);
}

static inline void st_pause(void) {
#ifdef WINDOWSENV
  system("pause");
#else
  pause();
#endif
}

#endif
