/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The two helpers of the concurrency tests: a short dwell inside a hot loop,
 * and a pin of one worker thread to one core. Each test held its own copy of
 * both, and the pin is one call per platform, so both are here now.
 *
 * C++ only.
 */

#ifndef _UT_CONCURRENCY_H_
#define _UT_CONCURRENCY_H_

#include <thread>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <pthread.h>
// clang-format on
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* A short dwell on the CPU, which makes the window of ownership wider, so a
 * second actor has the time to see a violation. The counter is volatile, so the
 * compiler keeps the loop. A ++ of a volatile variable is deprecated in C++20,
 * and mingw-w64 GCC takes C++20 as its default, so the step is an
 * assignment. */
inline void ut_dwell() {
  for (volatile int i = 0; i < 64; i = i + 1) {
  }
}

/* Put one worker on one core, so the test measures the lock-free ring and not
 * the scheduler. On a host with more than one socket, the scheduler moves an
 * unpinned busy loop and puts the workers together on one core. That cuts the
 * rate by about 600 times and breaks the deadlock budget of a design that is in
 * fact lock free.
 *
 * Core 0 is not a candidate. ut_eal_init() starts DPDK with "-c1", which pins
 * the main thread to core 0, so the affinity mask of the process is {0} after
 * the init and must not be the candidate set. The workers go to the cores from
 * 1 to nproc by slot instead.
 *
 * A failure of the pin is not an error: the pin makes a race more probable, it
 * does not make a test pass or fail. */
inline void ut_pin_worker(std::thread& t, int slot) {
  long nproc;
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  nproc = (long)si.dwNumberOfProcessors;
  /* One mask holds 64 cores. */
  if (nproc > 64) nproc = 64;
#else
  nproc = sysconf(_SC_NPROCESSORS_ONLN);
#endif
  if (nproc <= 1) return;

  int core = 1 + (slot % (int)(nproc - 1));
#ifdef _WIN32
  /* winpthreads has no pthread_setaffinity_np, and native_handle() of a
   * std::thread there gives a pthread_t and not a Win32 handle, so the handle
   * comes from pthread_gethandle(). */
  SetThreadAffinityMask((HANDLE)pthread_gethandle(t.native_handle()),
                        (DWORD_PTR)1 << core);
#else
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(core, &one);
  pthread_setaffinity_np(t.native_handle(), sizeof(one), &one);
#endif
}

#endif /* _UT_CONCURRENCY_H_ */
