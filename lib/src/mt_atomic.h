/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * C11 atomic wrappers replacing deprecated rte_atomic32 DPDK API.
 * Uses GCC/Clang __atomic builtins which are available on all supported compilers.
 *
 * Memory ordering policy:
 *   Default ops (no suffix) use RELAXED — correct for stats counters, session counts,
 *   and any atomic protected by an external lock (mutex/spinlock).
 *
 *   Ordered variants:
 *     _acquire  — ACQUIRE load: use when polling a flag or checking refcnt before reuse.
 *     _release  — RELEASE store/RMW: use when signaling a flag or releasing a refcnt.
 *
 *   dec_and_test always uses ACQ_REL (correct for destroy-on-zero refcount pattern).
 *
 * See .github/copilot-docs/concurrency-and-locking.md for usage guidance.
 */

#ifndef _MT_LIB_ATOMIC_HEAD_H_
#define _MT_LIB_ATOMIC_HEAD_H_

#include <stdbool.h>
#include <stdint.h>

typedef int32_t mt_atomic32_t;

/* ── Relaxed (default) — stats, counts under external lock, init/teardown ── */

static inline void mt_atomic32_init(mt_atomic32_t* v) {
  __atomic_store_n(v, 0, __ATOMIC_RELAXED);
}

static inline int32_t mt_atomic32_read(const mt_atomic32_t* v) {
  return __atomic_load_n(v, __ATOMIC_RELAXED);
}

static inline void mt_atomic32_set(mt_atomic32_t* v, int32_t new_value) {
  __atomic_store_n(v, new_value, __ATOMIC_RELAXED);
}

static inline void mt_atomic32_inc(mt_atomic32_t* v) {
  __atomic_add_fetch(v, 1, __ATOMIC_RELAXED);
}

static inline void mt_atomic32_dec(mt_atomic32_t* v) {
  __atomic_sub_fetch(v, 1, __ATOMIC_RELAXED);
}

/* ── Acquire — polling stop flags, reading refcnt before frame reuse ── */

static inline int32_t mt_atomic32_read_acquire(const mt_atomic32_t* v) {
  return __atomic_load_n(v, __ATOMIC_ACQUIRE);
}

/* ── Release — signaling stop flags, publishing data, releasing refcnt ── */

static inline void mt_atomic32_set_release(mt_atomic32_t* v, int32_t new_value) {
  __atomic_store_n(v, new_value, __ATOMIC_RELEASE);
}

static inline void mt_atomic32_dec_release(mt_atomic32_t* v) {
  __atomic_sub_fetch(v, 1, __ATOMIC_RELEASE);
}

/* ── Acquire-Release — destroy-on-zero refcount pattern ── */

/**
 * Atomically decrement and return true if the result is 0.
 * ACQ_REL: release ensures prior accesses complete before decrement;
 * acquire (when result is 0) ensures subsequent cleanup sees all data.
 */
static inline bool mt_atomic32_dec_and_test(mt_atomic32_t* v) {
  return __atomic_sub_fetch(v, 1, __ATOMIC_ACQ_REL) == 0;
}

#endif /* _MT_LIB_ATOMIC_HEAD_H_ */
