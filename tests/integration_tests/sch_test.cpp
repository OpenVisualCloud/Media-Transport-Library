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

class ScopedSchSleepUs {
 public:
  explicit ScopedSchSleepUs(mtl_handle mt) : mt_(mt) {
  }
  ~ScopedSchSleepUs() {
    if (mt_) mtl_sch_set_sleep_us(mt_, 0);
  }
  ScopedSchSleepUs(const ScopedSchSleepUs&) = delete;
  ScopedSchSleepUs& operator=(const ScopedSchSleepUs&) = delete;

 private:
  mtl_handle mt_ = nullptr;
};

void sch_create_test(mtl_handle mt) {
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  mtl_test::ScopedSch sch(mtl_sch_create(mt, &sch_ops));
  ASSERT_TRUE(sch);
  ASSERT_GE(sch.start(), 0);
  /* Allow scheduler thread to enter its run loop before stop. */
  mtl_sleep_us(1000);
  EXPECT_GE(sch.stop(), 0);
  sch.reset();
}

void sch_create_max_test(mtl_handle mt, int max) {
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  std::vector<mtl_test::ScopedSch> schs;
  schs.reserve(static_cast<size_t>(max));
  for (int i = 0; i < max; i++) {
    mtl_test::ScopedSch sch(mtl_sch_create(mt, &sch_ops));
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
  std::vector<mtl_test::ScopedSch> schs;
  std::vector<mtl_test::ScopedTasklet> tasklets;
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
      mtl_test::ScopedTasklet t(mtl_sch_register_tasklet(sch, &ops));
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
    ASSERT_GE(schs.back().start(), 0);
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

TEST(Sch, create_invalid_and_defaults) {
  mtl_handle mt = st_test_ctx()->handle;

  EXPECT_EQ(mtl_sch_create(mt, nullptr), nullptr);

  /* Default ops (null name, 0 nb_tasklets) must succeed and allow lifecycle edges. */
  struct mtl_sch_ops ops = {};
  mtl_test::ScopedSch sch(mtl_sch_create(mt, &ops));
  ASSERT_TRUE(sch);
  EXPECT_EQ(mtl_sch_stop(sch.get()), 0);
  ASSERT_GE(sch.start(), 0);
  EXPECT_LT(mtl_sch_start(sch.get()), 0);
  EXPECT_EQ(sch.stop(), 0);
  EXPECT_EQ(mtl_sch_stop(sch.get()), 0);
  sch.reset();

  /* Freeing a running scheduler directly via mtl_sch_free() must auto-stop cleanly. */
  using ScopedRawSch = mtl_test::ScopedHandle<mtl_sch_handle, mtl_sch_free>;
  {
    ScopedRawSch running_sch(mtl_sch_create(mt, &ops));
    ASSERT_TRUE(running_sch);
    ASSERT_EQ(mtl_sch_start(running_sch.get()), 0);
  }

  /* Invalid scheduler indices must be rejected. */
  EXPECT_LT(mtl_sch_enable_sleep(mt, -1, true), 0);
  EXPECT_LT(mtl_sch_enable_sleep(mt, 10000, false), 0);

  ScopedSchSleepUs sleep_guard(mt);
  EXPECT_EQ(mtl_sch_set_sleep_us(mt, 1000), 0);
  EXPECT_EQ(mtl_sch_set_sleep_us(mt, 0), 0);
}

TEST(Sch, tasklet_limits_and_lifecycle) {
  mtl_handle mt = st_test_ctx()->handle;

  /* Declare contexts before scheduler and tasklet handles for safe RAII teardown. */
  TaskletTestCtx ctx0;
  TaskletTestCtx ctx1;
  TaskletTestCtx ctx2;

  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch_limits";
  sch_ops.nb_tasklets = 2;

  mtl_test::ScopedSch sch(mtl_sch_create(mt, &sch_ops));
  ASSERT_TRUE(sch);

  /* Tasklet 0 uses start/stop/handler; tasklet 1 omits optional start/stop callbacks. */
  struct mtl_tasklet_ops ops0 = {};
  ops0.name = "t_full";
  ops0.priv = &ctx0;
  ops0.start = test_tasklet_start;
  ops0.stop = test_tasklet_stop;
  ops0.handler = test_tasklet_handler;

  struct mtl_tasklet_ops ops1 = {};
  ops1.name = "t_handler_only";
  ops1.priv = &ctx1;
  ops1.handler = test_tasklet_handler;

  mtl_test::ScopedTasklet t0(mtl_sch_register_tasklet(sch.get(), &ops0));
  ASSERT_TRUE(t0);
  mtl_test::ScopedTasklet t1(mtl_sch_register_tasklet(sch.get(), &ops1));
  ASSERT_TRUE(t1);

  /* Exceeding nb_tasklets = 2 must fail cleanly. */
  struct mtl_tasklet_ops ops_extra = ops0;
  ops_extra.name = "t_overflow";
  ops_extra.priv = &ctx2;
  EXPECT_EQ(mtl_sch_register_tasklet(sch.get(), &ops_extra), nullptr);

  /* Static pre-start unregister frees a slot for re-registration. */
  t1.reset();
  t1.reset(mtl_sch_register_tasklet(sch.get(), &ops1));
  ASSERT_TRUE(t1);
  EXPECT_EQ(mtl_sch_register_tasklet(sch.get(), &ops_extra), nullptr);

  /* Start scheduler and verify both tasklets execute. */
  ASSERT_GE(sch.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return ctx0.start.load(std::memory_order_acquire) &&
               ctx0.job.load(std::memory_order_acquire) > 0 &&
               ctx1.job.load(std::memory_order_acquire) > 0;
      },
      2000, 2));
  EXPECT_EQ(mtl_sch_register_tasklet(sch.get(), &ops_extra), nullptr);

  /* Runtime unregister invokes stop callback and frees a slot for re-registration. */
  t0.reset();
  EXPECT_FALSE(ctx0.start.load(std::memory_order_acquire));

  struct mtl_tasklet_ops ops2 = {};
  ops2.name = "t_runtime_re";
  ops2.priv = &ctx2;
  ops2.start = test_tasklet_start;
  ops2.stop = test_tasklet_stop;
  ops2.handler = test_tasklet_handler;

  mtl_test::ScopedTasklet t2(mtl_sch_register_tasklet(sch.get(), &ops2));
  ASSERT_TRUE(t2);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return ctx2.start.load(std::memory_order_acquire) &&
               ctx2.job.load(std::memory_order_acquire) > 0;
      },
      2000, 2));

  /* Stop/restart cycle invokes stop then start on surviving tasklets and resumes work. */
  ASSERT_EQ(sch.stop(), 0);
  EXPECT_FALSE(ctx2.start.load(std::memory_order_acquire));
  const int jobs1_before = ctx1.job.load(std::memory_order_acquire);
  const int jobs2_before = ctx2.job.load(std::memory_order_acquire);

  ASSERT_GE(sch.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return ctx2.start.load(std::memory_order_acquire) &&
               ctx1.job.load(std::memory_order_acquire) > jobs1_before &&
               ctx2.job.load(std::memory_order_acquire) > jobs2_before;
      },
      2000, 2));

  ASSERT_EQ(sch.stop(), 0);
  EXPECT_FALSE(ctx2.start.load(std::memory_order_acquire));
}
