/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <mtl/mtl_sch_api.h>

#include <atomic>
#include <memory>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

namespace {

using ScopedTasklet =
    mtl_test::ScopedHandle<mtl_tasklet_handle, mtl_sch_unregister_tasklet>;

/* Move-only RAII guard for mtl_sch_handle: stops (if started) and frees on exit. */
class ScopedSch {
 public:
  ScopedSch() = default;
  explicit ScopedSch(mtl_sch_handle h) : handle_(h) {
  }
  ~ScopedSch() {
    reset();
  }
  ScopedSch(const ScopedSch&) = delete;
  ScopedSch& operator=(const ScopedSch&) = delete;
  ScopedSch(ScopedSch&& other) noexcept
      : handle_(other.handle_), started_(other.started_) {
    other.handle_ = nullptr;
    other.started_ = false;
  }
  ScopedSch& operator=(ScopedSch&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      started_ = other.started_;
      other.handle_ = nullptr;
      other.started_ = false;
    }
    return *this;
  }

  mtl_sch_handle get() const {
    return handle_;
  }
  explicit operator bool() const {
    return handle_ != nullptr;
  }

  int start() {
    int ret = mtl_sch_start(handle_);
    if (ret >= 0) started_ = true;
    return ret;
  }

  int stop() {
    if (!started_ || !handle_) return 0;
    started_ = false;
    return mtl_sch_stop(handle_);
  }

  void reset() {
    if (started_ && handle_) {
      EXPECT_GE(stop(), 0);
    }
    if (handle_) {
      int ret = mtl_sch_free(handle_);
      EXPECT_GE(ret, 0);
      handle_ = nullptr;
    }
  }

 private:
  mtl_sch_handle handle_ = nullptr;
  bool started_ = false;
};

struct SchDigestScenario {
  int sch_cnt = 1;
  int tasklets = 1;
  bool runtime = false;
  bool test_auto_unregister = false;
};

struct TaskletTestCtx {
  int sch_idx = 0;
  int tasklet_idx = 0;
  std::atomic<bool> start{false};
  std::atomic<int> job{0};
};

int test_tasklet_start(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->start.store(true, std::memory_order_release);
  return 0;
}

int test_tasklet_stop(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->start.store(false, std::memory_order_release);
  return 0;
}

int test_tasklet_handler(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->job.fetch_add(1, std::memory_order_release);
  return 0;
}

void sch_create_test(mtl_handle mt) {
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  ScopedSch sch(mtl_sch_create(mt, &sch_ops));
  ASSERT_TRUE(sch);
  EXPECT_GE(sch.start(), 0);
  /* Allow scheduler thread to enter its run loop before stop. */
  mtl_sleep_us(1000);
  EXPECT_GE(sch.stop(), 0);
  sch.reset();
}

void sch_create_max_test(mtl_handle mt, int max) {
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  std::vector<ScopedSch> schs;
  schs.reserve(static_cast<size_t>(max));
  for (int i = 0; i < max; i++) {
    ScopedSch sch(mtl_sch_create(mt, &sch_ops));
    if (!sch) break;
    if (sch.start() < 0) break;
    schs.push_back(std::move(sch));
  }

  info("%s, cnt: %zu\n", __func__, schs.size());
  EXPECT_GT(schs.size(), 0u);
  mtl_sleep_us(2000);

  for (auto& sch : schs) {
    EXPECT_GE(sch.stop(), 0);
    sch.reset();
  }
}

void sch_tasklet_digest_test(mtl_handle mt, const SchDigestScenario& para) {
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch_test";
  sch_ops.nb_tasklets = static_cast<uint32_t>(para.tasklets);

  struct mtl_tasklet_ops ops = {};
  ops.name = "test";
  ops.start = test_tasklet_start;
  ops.stop = test_tasklet_stop;
  ops.handler = test_tasklet_handler;

  /*
   * Strict teardown order on any early ASSERT_* failure:
   * tasklets unregister first (while schs are alive), then schs stop and free,
   * then tasklet_ctxs delete (after no scheduler thread can touch priv).
   */
  std::vector<std::unique_ptr<TaskletTestCtx>> tasklet_ctxs;
  std::vector<ScopedSch> schs;
  std::vector<ScopedTasklet> tasklets;
  const size_t total_tasklets =
      static_cast<size_t>(para.sch_cnt) * static_cast<size_t>(para.tasklets);
  tasklet_ctxs.reserve(total_tasklets);
  schs.reserve(static_cast<size_t>(para.sch_cnt));
  tasklets.reserve(total_tasklets);

  auto register_tasklets = [&](int sch_idx, mtl_sch_handle sch) {
    for (int j = 0; j < para.tasklets; j++) {
      auto ctx = std::make_unique<TaskletTestCtx>();
      ctx->sch_idx = sch_idx;
      ctx->tasklet_idx = j;
      ops.priv = ctx.get();
      ScopedTasklet t(mtl_sch_register_tasklet(sch, &ops));
      ASSERT_TRUE(t);
      tasklet_ctxs.push_back(std::move(ctx));
      tasklets.push_back(std::move(t));
    }
  };

  for (int i = 0; i < para.sch_cnt; i++) {
    schs.emplace_back(mtl_sch_create(mt, &sch_ops));
    ASSERT_TRUE(schs.back());
    if (!para.runtime) {
      ASSERT_NO_FATAL_FAILURE(register_tasklets(i, schs.back().get()));
    }
    EXPECT_GE(schs.back().start(), 0);
    if (para.runtime) {
      ASSERT_NO_FATAL_FAILURE(register_tasklets(i, schs.back().get()));
    }
  }

  /* Wait until every tasklet has started and executed at least one job (2 ms poll,
   * 2000 ms bound; replaces legacy 1000 ms blind sleep and completes in <5 ms). */
  bool ready = mtl_test::wait_until(
      [&]() {
        for (const auto& ctx : tasklet_ctxs) {
          if (!ctx->start.load(std::memory_order_acquire) ||
              ctx->job.load(std::memory_order_acquire) <= 0) {
            return false;
          }
        }
        return true;
      },
      2000, 2);
  EXPECT_TRUE(ready);

  for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
    EXPECT_TRUE(tasklet_ctxs[i]->start.load(std::memory_order_acquire));
    EXPECT_GT(tasklet_ctxs[i]->job.load(std::memory_order_acquire), 0);
    if (para.runtime) {
      tasklets[i].reset();
    }
  }

  for (auto& sch : schs) {
    EXPECT_GE(sch.stop(), 0);
  }

  if (!para.runtime) {
    if (para.test_auto_unregister) {
      /* Release handles so mtl_sch_free() exercises automatic tasklet unregistration. */
      for (auto& t : tasklets) t.release();
    } else {
      for (auto& t : tasklets) t.reset();
    }
  }

  for (auto& sch : schs) {
    sch.reset();
  }

  /* Verify every tasklet received its stop callback. */
  for (const auto& ctx : tasklet_ctxs) {
    EXPECT_FALSE(ctx->start.load(std::memory_order_acquire));
  }
}

}  // namespace

TEST(Sch, create_single) {
  sch_create_test(st_test_ctx()->handle);
}

TEST(Sch, create_max) {
  sch_create_max_test(st_test_ctx()->handle, 10);
}

TEST(Sch, tasklet_single) {
  SchDigestScenario para;
  para.test_auto_unregister = true;
  sch_tasklet_digest_test(st_test_ctx()->handle, para);
}

TEST(Sch, tasklet_multi) {
  SchDigestScenario para;
  para.sch_cnt = 2;
  para.tasklets = 8;
  sch_tasklet_digest_test(st_test_ctx()->handle, para);
}

TEST(Sch, tasklet_runtime) {
  SchDigestScenario para;
  para.sch_cnt = 2;
  para.tasklets = 4;
  para.runtime = true;
  sch_tasklet_digest_test(st_test_ctx()->handle, para);
}
