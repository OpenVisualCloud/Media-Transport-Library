/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_APP_PLATFORM_HEAD_H_
#define _ST_APP_PLATFORM_HEAD_H_

#ifdef WINDOWSENV /* Windows */
#include <Winsock2.h>
#include <ws2tcpip.h>
// clang-format off
#include "win_posix.h"
// clang-format on
#else /* Linux */
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <numa.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#ifndef POLLIN /* For windows */
/* There is data to read */
#define POLLIN 0x001
#endif

#ifdef WINDOWSENV
typedef unsigned long int nfds_t;
#endif

#ifdef WINDOWSENV
#define strdup(p) _strdup(p)
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

static inline void st_usleep(
    useconds_t usec) {  // windows usleep function precision is only 1~15ms
#ifdef WINDOWSENV
  LARGE_INTEGER delay;
  HANDLE delay_timer_handle = NULL;
  delay.QuadPart = usec;
  delay.QuadPart = -(10 * delay.QuadPart);
  delay_timer_handle = CreateWaitableTimer(NULL, TRUE, NULL);
  if (delay_timer_handle) {
    SetWaitableTimer(delay_timer_handle, &delay, 0, NULL, NULL, 0);
    WaitForSingleObject(delay_timer_handle, INFINITE);
    CloseHandle(delay_timer_handle);
  } else {
    Sleep((usec + 999) / 1000);
  }
#else
  usleep(usec);
#endif
}

static inline int st_get_real_time(struct timespec* ts) {
#ifdef WINDOWSENV
  unsigned __int64 t;
  union {
    unsigned __int64 u64;
    FILETIME ft;
  } ct;
  GetSystemTimePreciseAsFileTime(&ct.ft);
  t = ct.u64 - INT64_C(116444736000000000);
  ts->tv_sec = t / 10000000;
  ts->tv_nsec = ((int)(t % 10000000)) * 100;
  return 0;
#else
  return clock_gettime(CLOCK_REALTIME, ts);
#endif
}

static inline int st_set_real_time(struct timespec* ts) {
#ifdef WINDOWSENV
  time_t secs = ts->tv_sec;
  WORD milliseconds = ts->tv_nsec / 1000000;

  struct tm* tm;
  tm = gmtime(&secs);

  st->wYear = tm->tm_year + 1900;
  st->wMonth = tm->tm_mon + 1;
  st->wDayOfWeek = tm->tm_wday;
  st->wDay = tm->tm_mday;
  st->wHour = tm->tm_hour;
  st->wMinute = tm->tm_min;
  st->wSecond = tm->tm_sec;
  st->wMilliseconds = milliseconds;

  if (!SetSystemTime(&st)) {
    /* set failed */
    return -EPERM;
  }
  return 0;
#else
  return clock_settime(CLOCK_REALTIME, ts);
#endif
}

#endif
