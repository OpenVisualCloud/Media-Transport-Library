/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <unistd.h>

#ifndef _ST_PLUGIN_PLATFORM_HEAD_H_
#define _ST_PLUGIN_PLATFORM_HEAD_H_

#ifdef CLOCK_MONOTONIC_RAW
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_RAW
#else
#define ST_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
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

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
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

#endif
