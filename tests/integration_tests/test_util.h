/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#pragma once

#include <openssl/sha.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <queue>

#include "test_platform.h"

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

#ifndef NS_PER_US
#define NS_PER_US (1000)
#endif

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

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
  return (static_cast<uint64_t>(ts.tv_sec) * NS_PER_S) + ts.tv_nsec;
}

/* Wall-clock time (in nanoseconds) for user PTP callbacks and clock checks. */
static inline uint64_t st_test_get_realtime_ns() {
  struct timespec spec;
#ifndef WINDOWSENV
  clock_gettime(CLOCK_REALTIME, &spec);
  return (static_cast<uint64_t>(spec.tv_sec) * NS_PER_S) + spec.tv_nsec;
#else
  unsigned __int64 t;
  union {
    unsigned __int64 u64;
    FILETIME ft;
  } ct;
  GetSystemTimePreciseAsFileTime(&ct.ft);
  t = ct.u64 - INT64_C(116444736000000000);
  spec.tv_sec = t / 10000000;
  spec.tv_nsec = static_cast<int>(t % 10000000) * 100;
  return (static_cast<uint64_t>(spec.tv_sec) * NS_PER_S) + spec.tv_nsec;
#endif
}

static inline void st_test_rand_data(uint8_t* p, size_t sz, uint8_t base) {
  /* Thread-local xorshift64* PRNG: lock-free and ~100x faster than rand(). */
  static thread_local uint64_t tls_state = 0;
  if (!tls_state) {
    tls_state = st_test_get_monotonic_time() ^
                (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tls_state)) *
                 0x9e3779b97f4a7c15ULL) ^
                0x2545f4914f6cdd1dULL;
  }
  uint64_t s = tls_state ^ (static_cast<uint64_t>(base + 1) * 0x9e3779b97f4a7c15ULL);
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
    p[i] = static_cast<uint8_t>((s * 0x2545f4914f6cdd1dULL) >> 56);
  }
  tls_state = s;
}

static inline void test_sha_dump(const char* tag, const unsigned char* sha) {
#ifdef DEBUG
  for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    printf("0x%02x ", sha[i]);
  }
  printf(", %s done\n", tag);
#else
  (void)tag;
  (void)sha;
#endif
}
