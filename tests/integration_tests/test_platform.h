/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#pragma once

#ifdef WINDOWSENV /* Windows */
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#include <ws2tcpip.h>
#ifndef sleep
#define sleep(x) Sleep(1000 * x)
#endif
#else /* Linux */
#include <arpa/inet.h>
#include <poll.h>
#endif

#include <unistd.h>

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
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