/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <openssl/sha.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>

#include "test_platform.h"

#pragma once

#define TEST_DATA_FIXED_PATTER (0)

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

#ifndef NS_PER_US
#define NS_PER_US (1000)
#endif

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

enum st_test_level {
  ST_TEST_LEVEL_ALL = 0,
  ST_TEST_LEVEL_MANDATORY,
  ST_TEST_LEVEL_MAX, /* max value of this enum */
};

static inline void* st_test_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void st_test_free(void* p) {
  free(p);
}

static inline void st_test_rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
#if TEST_DATA_FIXED_PATTER
    p[i] = base + i;
#else
    p[i] = rand();
#endif
  }
}

static inline void st_test_rand_v210(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
#if TEST_DATA_FIXED_PATTER
    p[i] = base + i;
#else
    p[i] = rand();
#endif
    if ((i % 4) == 3) p[i] &= 0x3F;
  }
}

int st_test_check_patter(uint8_t* p, size_t sz, uint8_t base);

int st_test_cmp(uint8_t* s1, uint8_t* s2, size_t sz);

int st_test_cmp_u16(uint16_t* s1, uint16_t* s2, size_t sz);

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_test_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

void test_sha_dump(const char* tag, unsigned char* sha);
