/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#pragma once

#include <openssl/sha.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>

#include "test_platform.h"

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

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_test_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}

static inline void st_test_rand_data(uint8_t* p, size_t sz, uint8_t base) {
#if TEST_DATA_FIXED_PATTER
  for (size_t i = 0; i < sz; i++) {
    p[i] = base + i;
  }
#else
  /* Thread-local xorshift64* PRNG: lock-free and ~100x faster than rand(). */
  static thread_local uint64_t tls_state = 0;
  if (!tls_state) {
    tls_state = st_test_get_monotonic_time() ^
                ((uint64_t)(uintptr_t)&tls_state * 0x9e3779b97f4a7c15ULL) ^
                0x2545f4914f6cdd1dULL;
  }
  uint64_t s = tls_state ^ ((uint64_t)(base + 1) * 0x9e3779b97f4a7c15ULL);
  if (!s) s = 0x2545f4914f6cdd1dULL;
  size_t i = 0;
  for (; i + sizeof(uint64_t) <= sz; i += sizeof(uint64_t)) {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    uint64_t v = s * 0x2545f4914f6cdd1dULL;
    memcpy(p + i, &v, sizeof(v));
  }
  for (; i < sz; i++) {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    p[i] = (uint8_t)((s * 0x2545f4914f6cdd1dULL) >> 56);
  }
  tls_state = s;
#endif
}

static inline void st_test_rand_v210(uint8_t* p, size_t sz, uint8_t base) {
  st_test_rand_data(p, sz, base);
  for (size_t i = 3; i < sz; i += 4) {
    p[i] &= 0x3F;
  }
}

int st_test_cmp(uint8_t* s1, uint8_t* s2, size_t sz);

void test_sha_dump(const char* tag, unsigned char* sha);
