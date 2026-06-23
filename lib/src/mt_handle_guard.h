/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * mt_handle_guard.h — public session-handle lifecycle gate. Closes the
 * use-after-free race where a reader thread is preempted between deciding
 * the handle is alive and dereferencing it, while another thread runs
 * *_free() on the same handle.
 *
 * Design:
 *   - Each public-API wrapper struct carries:
 *       _Atomic uint32_t lc_destroying;   (0 alive, 1 tearing down)
 *       _Atomic uint32_t lc_refcnt;       (in-flight public-API callers)
 *     plus, for the 8 blocking-capable pipeline ctx types:
 *       void (*wake_on_destroy)(void* self);
 *
 *   - Public APIs use MT_HANDLE_GUARD / MT_HANDLE_GUARD_VOID at entry to
 *     acquire one ref, and MT_HANDLE_RELEASE on every exit path. The
 *     intended pattern is single-exit:
 *
 *       int api(handle h, ...) {
 *         ctx_t* c = h;
 *         int ret;
 *         MT_HANDLE_GUARD(c, MT_HANDLE_X, -EIO);
 *         ...body...
 *         if (err) { ret = -EXXX; goto out; }
 *         ret = 0;
 *       out:
 *         MT_HANDLE_RELEASE(c);
 *         return ret;
 *       }
 *
 *     EVERY return after MT_HANDLE_GUARD must go through MT_HANDLE_RELEASE
 *     (typically via `goto out`). Missing a release is a ref leak that
 *     hangs *_free() forever in mt_handle_drain.
 *
 *   - *_free() calls mt_handle_begin_destroy() (CAS 0->1; one winner; loser
 *     gets -EBUSY), wakes any blocking sleeper via wake_on_destroy, drains
 *     in-flight callers via mt_handle_drain(), then runs the existing
 *     teardown and finally mt_rte_free()s the wrapper. Safe because drain
 *     guarantees no thread holds a bare pointer to the wrapper — sleepers
 *     in the blocking get_frame/put_frame paths keep their ref held across
 *     pthread_cond_timedwait, and the wake_on_destroy hook (cond_signal
 *     under block_wake_mutex) wakes them within microseconds so drain
 *     returns promptly.
 *
 * Memory ordering (Dekker store-buffer pattern):
 *   Reader (mt_handle_acquire):  fetch_add(refcnt) SEQ_CST; load(destroying) SEQ_CST.
 *   Writer (mt_handle_begin_destroy + mt_handle_drain):
 *                                CAS(destroying,0->1) SEQ_CST; load(refcnt) SEQ_CST.
 *   ALL FOUR ops must be SEQ_CST so the C11 SC total order rules out the
 *   reordering window on weakly-ordered architectures (notably ARM).
 *
 * Portability: pure C11 (<stdatomic.h>). No compiler extensions.
 */

#ifndef _MT_LIB_HANDLE_GUARD_HEAD_H_
#define _MT_LIB_HANDLE_GUARD_HEAD_H_

#include <errno.h>
#include <rte_pause.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "mt_header.h" /* enum mt_handle_type */

/* ---- acquire / release ---- */

/* Returns 0 on success (caller owns one ref);
 *        -EIO if the handle is dying or of the wrong type.
 *
 * On failure no ref is held and no further cleanup is needed. */
static inline int mt_handle_acquire(_Atomic uint32_t* destroying,
                                    _Atomic uint32_t* refcnt, enum mt_handle_type* type,
                                    enum mt_handle_type want) {
  /* Fast type reject: the type field is set once at *_create before the
   * handle is published to the caller and never mutated, so a plain load
   * is race-free. */
  if (*type != want) return -EIO;

  /* Fast destroying reject: avoid the inc-then-undo dance in the common
   * post-destroy case. ACQUIRE pairs with the writer's RELEASE-ish
   * publication of the destroying flag (the SEQ_CST CAS). */
  if (atomic_load_explicit(destroying, memory_order_acquire)) return -EIO;

  /* SEQ_CST mandatory here: this fetch_add and the following destroying
   * load form one half of a store-buffer / Dekker pair with the writer's
   * SEQ_CST CAS on destroying and SEQ_CST drain-load on refcnt. All four
   * ops must lie in the C11 SC total order. */
  atomic_fetch_add_explicit(refcnt, 1, memory_order_seq_cst);
  if (atomic_load_explicit(destroying, memory_order_seq_cst)) {
    atomic_fetch_sub_explicit(refcnt, 1, memory_order_release);
    return -EIO;
  }
  return 0;
}

static inline void mt_handle_release(_Atomic uint32_t* refcnt) {
  atomic_fetch_sub_explicit(refcnt, 1, memory_order_release);
}

/* ---- destroy claim (CAS) + drain ---- */

/* Returns 0 if THIS caller wins the destroy race (caller proceeds to tear
 * down the inner session);
 *        -EIO on type mismatch (no state mutation; matches the historic
 *              return value of every *_free type check);
 *        -EBUSY if another thread is already destroying this handle. */
static inline int mt_handle_begin_destroy(_Atomic uint32_t* destroying,
                                          enum mt_handle_type* type,
                                          enum mt_handle_type want) {
  if (*type != want) return -EIO;
  uint32_t expected = 0;
  if (!atomic_compare_exchange_strong_explicit(destroying, &expected, 1u,
                                               memory_order_seq_cst,  /* success */
                                               memory_order_acquire)) /* failure */
    return -EBUSY;
  return 0;
}

/* Spin until all in-flight public-API callers have decremented refcnt.
 *
 * SEQ_CST mandatory on the load (Dekker pair completion). DO NOT relax to
 * ACQUIRE: ACQUIRE loads are not part of the C11 SC total order, opening a
 * reordering window where a reader's inc lands AFTER this load returns 0
 * but BEFORE the reader observes destroying=1. */
static inline void mt_handle_drain(_Atomic uint32_t* refcnt) {
  while (atomic_load_explicit(refcnt, memory_order_seq_cst) > 0) rte_pause();
}

/* ---- acquire-or-return macros ---- */

/* MT_HANDLE_GUARD: acquire one ref or fail-return.
 * Use as the FIRST executable statement of every public API.
 *
 * Every exit path after this macro MUST call MT_HANDLE_RELEASE (typically
 * via single-exit `goto out;`). A missing release leaks a ref and hangs
 * *_free() forever in mt_handle_drain. */
#define MT_HANDLE_GUARD(impl, want_type, errret)                                     \
  do {                                                                               \
    if (mt_handle_acquire(&(impl)->lc_destroying, &(impl)->lc_refcnt, &(impl)->type, \
                          (want_type)) < 0)                                          \
      return (errret);                                                               \
  } while (0)

/* For void-returning APIs (the 5 *_rx_put_mbuf entry points). */
#define MT_HANDLE_GUARD_VOID(impl, want_type)                                        \
  do {                                                                               \
    if (mt_handle_acquire(&(impl)->lc_destroying, &(impl)->lc_refcnt, &(impl)->type, \
                          (want_type)) < 0)                                          \
      return;                                                                        \
  } while (0)

/* Pair with MT_HANDLE_GUARD / MT_HANDLE_GUARD_VOID. */
#define MT_HANDLE_RELEASE(impl) mt_handle_release(&(impl)->lc_refcnt)

#endif /* _MT_LIB_HANDLE_GUARD_HEAD_H_ */
