/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include "sch/mt_sch_harness.h"

namespace {

constexpr unsigned int kThreads = 4;
constexpr int kRounds = 2000;

/* ut_eal_init() pins the process to core 0, where the workers would barely interleave. */
void pin_worker(std::thread& t, unsigned int slot) {
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  if (nproc <= 1) return;
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(1 + (slot % (nproc - 1)), &one);
  pthread_setaffinity_np(t.native_handle(), sizeof(one), &one);
}

}  // namespace

/* Threads of one instance without MtlManager take and return lcores at once. Each holds
 * one at a time and there are as many lcores as threads, so no get may fail, no lcore
 * may reach two threads, and all are free once the threads are done. */
TEST(MtSchLcoreTest, ConcurrentGetPutHandsEachLcoreToOneThread) {
  ASSERT_EQ(ut_sch_init(), 0);
  mtl_handle mt = ut_sch_create_lcore_main(kThreads);
  ASSERT_NE(mt, nullptr);

  std::array<std::atomic<bool>, kThreads + 1> held{};
  std::atomic<unsigned int> ready{0};
  std::atomic<int> failures{0};
  std::atomic<int> shared{0};
  std::vector<std::thread> workers;
  for (unsigned int t = 0; t < kThreads; t++) {
    workers.emplace_back([&] {
      ready++;
      while (ready.load() < kThreads) std::this_thread::yield();
      for (int r = 0; r < kRounds; r++) {
        unsigned int lcore = 0;
        if (mtl_get_lcore(mt, &lcore) < 0 || lcore < 1 || lcore > kThreads) {
          failures++;
          return;
        }
        if (held[lcore].exchange(true)) shared++;
        std::this_thread::yield();
        held[lcore].store(false);
        if (mtl_put_lcore(mt, lcore) < 0) {
          failures++;
          return;
        }
      }
    });
    pin_worker(workers.back(), t);
  }
  for (auto& w : workers) w.join();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(shared.load(), 0);
  EXPECT_EQ(ut_sch_lcore_shm_used(mt), 0);
  EXPECT_EQ(ut_sch_lcore_cnt(mt), 0);
  ut_sch_destroy_lcore_main(mt);
}
