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
  std::atomic<bool> start{false};
  std::atomic<int> start_cnt{0};
  std::atomic<int> stop_cnt{0};
  std::atomic<int> job{0};
  std::atomic<int> handler_ret{MTL_TASKLET_ALL_DONE};
};

int test_tasklet_start(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->start.store(true, std::memory_order_release);
  ctx->start_cnt.fetch_add(1, std::memory_order_release);
  return 0;
}

int test_tasklet_stop(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->start.store(false, std::memory_order_release);
  ctx->stop_cnt.fetch_add(1, std::memory_order_release);
  return 0;
}

int test_tasklet_handler(void* priv) {
  auto* ctx = static_cast<TaskletTestCtx*>(priv);
  ctx->job.fetch_add(1, std::memory_order_release);
  return ctx->handler_ret.load(std::memory_order_relaxed);
}

void sch_tasklet_digest_test(mtl_handle mt, const SchDigestScenario& para) {
  auto* global_ctx = st_test_ctx();
  const int base_sch = st_test_sch_cnt(global_ctx);
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

  auto register_tasklets = [&](mtl_sch_handle sch) {
    for (int j = 0; j < para.tasklets; j++) {
      auto ctx = std::make_unique<TaskletTestCtx>();
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
      ASSERT_NO_FATAL_FAILURE(register_tasklets(schs.back().get()));
    }
    ASSERT_GE(schs.back().start(), 0);
    if (para.runtime) {
      ASSERT_NO_FATAL_FAILURE(register_tasklets(schs.back().get()));
    }
  }
  EXPECT_EQ(st_test_sch_cnt(global_ctx), base_sch + para.sch_cnt);

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
    if (para.runtime) {
      EXPECT_GE(tasklet_ctxs[i]->start_cnt.load(std::memory_order_acquire), 1);
    } else {
      EXPECT_EQ(tasklet_ctxs[i]->start_cnt.load(std::memory_order_acquire), 1);
    }
    EXPECT_EQ(tasklet_ctxs[i]->stop_cnt.load(std::memory_order_acquire), 0);
    EXPECT_GT(tasklet_ctxs[i]->job.load(std::memory_order_acquire), 0);
  }

  std::vector<int> frozen_jobs(tasklet_ctxs.size(), 0);
  if (para.runtime) {
    /* Runtime unregister must invoke stop and freeze job execution while sch runs. */
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      tasklets[i].reset();
      EXPECT_FALSE(tasklet_ctxs[i]->start.load(std::memory_order_acquire));
      EXPECT_EQ(tasklet_ctxs[i]->stop_cnt.load(std::memory_order_acquire), 1);
      frozen_jobs[i] = tasklet_ctxs[i]->job.load(std::memory_order_acquire);
    }
    mtl_sleep_us(5000);
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      EXPECT_EQ(tasklet_ctxs[i]->job.load(std::memory_order_acquire), frozen_jobs[i]);
    }
    for (auto& sch : schs) {
      EXPECT_GE(sch.stop(), 0);
    }
    for (auto& sch : schs) {
      sch.reset();
    }
  } else if (para.test_auto_unregister) {
    /* Free running schedulers directly via mtl_sch_free() with live tasklets so
     * mtl_sch_free() must auto-stop and auto-unregister them cleanly. */
    for (auto& t : tasklets) t.release();
    for (auto& sch : schs) {
      mtl_sch_handle raw = sch.release();
      ASSERT_NE(raw, nullptr);
      EXPECT_GE(mtl_sch_free(raw), 0);
    }
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      frozen_jobs[i] = tasklet_ctxs[i]->job.load(std::memory_order_acquire);
    }
    mtl_sleep_us(5000);
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      EXPECT_EQ(tasklet_ctxs[i]->job.load(std::memory_order_acquire), frozen_jobs[i]);
    }
  } else {
    for (auto& sch : schs) {
      EXPECT_GE(sch.stop(), 0);
    }
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      frozen_jobs[i] = tasklet_ctxs[i]->job.load(std::memory_order_acquire);
    }
    mtl_sleep_us(5000);
    for (size_t i = 0; i < tasklet_ctxs.size(); i++) {
      EXPECT_EQ(tasklet_ctxs[i]->job.load(std::memory_order_acquire), frozen_jobs[i]);
    }
    for (auto& t : tasklets) t.reset();
    for (auto& sch : schs) {
      sch.reset();
    }
  }

  /* Verify every tasklet stopped cleanly after running. */
  for (const auto& ctx : tasklet_ctxs) {
    EXPECT_FALSE(ctx->start.load(std::memory_order_acquire));
    if (para.runtime) {
      EXPECT_GE(ctx->start_cnt.load(std::memory_order_acquire), 1);
    } else {
      EXPECT_EQ(ctx->start_cnt.load(std::memory_order_acquire), 1);
    }
    EXPECT_EQ(ctx->stop_cnt.load(std::memory_order_acquire), 1);
  }
  EXPECT_EQ(st_test_sch_cnt(global_ctx), base_sch);
}

}  // namespace

TEST(Sch, create_single) {
  auto* ctx = st_test_ctx();
  const int base_sch = st_test_sch_cnt(ctx);
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  mtl_test::ScopedSch sch(mtl_sch_create(ctx->handle, &sch_ops));
  ASSERT_TRUE(sch);
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch + 1);
  ASSERT_GE(sch.start(), 0);
  /* Allow scheduler thread to enter its run loop before stop. */
  mtl_sleep_us(1000);
  EXPECT_GE(sch.stop(), 0);
  sch.reset();
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);
}

TEST(Sch, create_max) {
  auto* ctx = st_test_ctx();
  mtl_handle mt = ctx->handle;
  const int base_sch = st_test_sch_cnt(ctx);
  constexpr int kMaxSch = 10;
  struct mtl_sch_ops sch_ops = {};
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  std::vector<mtl_test::ScopedSch> schs;
  schs.reserve(kMaxSch);
  for (int i = 0; i < kMaxSch; i++) {
    mtl_test::ScopedSch sch(mtl_sch_create(mt, &sch_ops));
    if (!sch || sch.start() < 0) break;
    schs.push_back(std::move(sch));
  }

  info("%s, cnt: %zu\n", __func__, schs.size());
  EXPECT_GE(schs.size(), 2u);
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch + static_cast<int>(schs.size()));
  mtl_sleep_us(2000);
  for (auto& sch : schs) {
    EXPECT_GE(sch.stop(), 0);
  }
  schs.clear();
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);

  /* Verify scheduler slots and lcores were reclaimed by running a follow-up sch. */
  {
    mtl_test::ScopedSch follow_up(mtl_sch_create(mt, &sch_ops));
    ASSERT_TRUE(follow_up);
    EXPECT_EQ(st_test_sch_cnt(ctx), base_sch + 1);
    ASSERT_GE(follow_up.start(), 0);
    mtl_sleep_us(1000);
    EXPECT_GE(follow_up.stop(), 0);
  }
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);
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
  auto* global_ctx = st_test_ctx();
  mtl_handle mt = global_ctx->handle;
  const int base_sch = st_test_sch_cnt(global_ctx);

  EXPECT_EQ(mtl_sch_create(mt, nullptr), nullptr);

  /* Default ops (null name, 0 nb_tasklets) must succeed and allow lifecycle edges. */
  struct mtl_sch_ops ops = {};
  TaskletTestCtx ctx_sch;
  {
    mtl_test::ScopedSch sch(mtl_sch_create(mt, &ops));
    ASSERT_TRUE(sch);

    struct mtl_tasklet_ops t_ops = {};
    t_ops.name = "t_defaults";
    t_ops.priv = &ctx_sch;
    t_ops.start = test_tasklet_start;
    t_ops.stop = test_tasklet_stop;
    t_ops.handler = test_tasklet_handler;
    mtl_test::ScopedTasklet t_sch(mtl_sch_register_tasklet(sch.get(), &t_ops));
    ASSERT_TRUE(t_sch);

    EXPECT_EQ(mtl_sch_stop(sch.get()), 0);
    EXPECT_EQ(ctx_sch.stop_cnt.load(std::memory_order_acquire), 0);

    ASSERT_GE(sch.start(), 0);
    EXPECT_LT(mtl_sch_start(sch.get()), 0);
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          return ctx_sch.start.load(std::memory_order_acquire) &&
                 ctx_sch.job.load(std::memory_order_acquire) > 0;
        },
        2000, 2));
    EXPECT_EQ(ctx_sch.start_cnt.load(std::memory_order_acquire), 1);

    EXPECT_EQ(sch.stop(), 0);
    EXPECT_FALSE(ctx_sch.start.load(std::memory_order_acquire));
    EXPECT_EQ(ctx_sch.stop_cnt.load(std::memory_order_acquire), 1);
    EXPECT_EQ(mtl_sch_stop(sch.get()), 0);
    EXPECT_EQ(ctx_sch.stop_cnt.load(std::memory_order_acquire), 1);

    /* Restart must invoke start again and resume job execution. */
    const int jobs_before_restart = ctx_sch.job.load(std::memory_order_acquire);
    ASSERT_GE(sch.start(), 0);
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          return ctx_sch.start.load(std::memory_order_acquire) &&
                 ctx_sch.job.load(std::memory_order_acquire) > jobs_before_restart;
        },
        2000, 2));
    EXPECT_EQ(ctx_sch.start_cnt.load(std::memory_order_acquire), 2);
    EXPECT_EQ(sch.stop(), 0);
    EXPECT_FALSE(ctx_sch.start.load(std::memory_order_acquire));
    EXPECT_EQ(ctx_sch.stop_cnt.load(std::memory_order_acquire), 2);

    /* Valid active scheduler index 0 must accept sleep toggle. */
    EXPECT_EQ(mtl_sch_enable_sleep(mt, 0, true), 0);
    EXPECT_EQ(mtl_sch_enable_sleep(mt, 0, false), 0);
  }
  EXPECT_EQ(st_test_sch_cnt(global_ctx), base_sch);

  /* Freeing a running scheduler directly via mtl_sch_free() must auto-stop cleanly
   * and invoke the tasklet stop callback. */
  TaskletTestCtx ctx_run;
  using ScopedRawSch = mtl_test::ScopedHandle<mtl_sch_handle, mtl_sch_free>;
  {
    ScopedRawSch running_sch(mtl_sch_create(mt, &ops));
    ASSERT_TRUE(running_sch);
    struct mtl_tasklet_ops t_ops = {};
    t_ops.name = "t_running_free";
    t_ops.priv = &ctx_run;
    t_ops.start = test_tasklet_start;
    t_ops.stop = test_tasklet_stop;
    t_ops.handler = test_tasklet_handler;
    mtl_tasklet_handle t_run = mtl_sch_register_tasklet(running_sch.get(), &t_ops);
    ASSERT_NE(t_run, nullptr);
    ASSERT_EQ(mtl_sch_start(running_sch.get()), 0);
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          return ctx_run.start.load(std::memory_order_acquire) &&
                 ctx_run.job.load(std::memory_order_acquire) > 0;
        },
        2000, 2));
  }
  EXPECT_FALSE(ctx_run.start.load(std::memory_order_acquire));
  EXPECT_EQ(ctx_run.start_cnt.load(std::memory_order_acquire), 1);
  EXPECT_EQ(ctx_run.stop_cnt.load(std::memory_order_acquire), 1);
  const int run_jobs_after_free = ctx_run.job.load(std::memory_order_acquire);
  mtl_sleep_us(5000);
  EXPECT_EQ(ctx_run.job.load(std::memory_order_acquire), run_jobs_after_free);
  EXPECT_EQ(st_test_sch_cnt(global_ctx), base_sch);

  /* Invalid scheduler indices must be rejected. */
  EXPECT_LT(mtl_sch_enable_sleep(mt, -1, true), 0);
  EXPECT_LT(mtl_sch_enable_sleep(mt, 10000, false), 0);

  EXPECT_EQ(mtl_sch_set_sleep_us(mt, 1000), 0);
  EXPECT_EQ(mtl_sch_set_sleep_us(mt, 0), 0);
}

TEST(Sch, sleep_control) {
  auto* global_ctx = st_test_ctx();
  mtl_handle mt = global_ctx->handle;

  /* Find the first inactive scheduler slot (the next mtl_sch_create() acquires it). */
  int sch_idx = -1;
  for (int idx = 0; idx < 64; idx++) {
    if (mtl_sch_enable_sleep(mt, idx, false) < 0) {
      sch_idx = idx;
      break;
    }
  }
  ASSERT_GE(sch_idx, 0);

  struct SleepResetGuard {
    mtl_handle h;
    ~SleepResetGuard() {
      (void)mtl_sch_set_sleep_us(h, 0);
    }
  } sleep_guard{mt};

  TaskletTestCtx tctx;
  struct mtl_sch_ops ops = {};
  ops.name = "sch_sleep_ctrl";
  ops.nb_tasklets = 1;
  mtl_test::ScopedSch sch(mtl_sch_create(mt, &ops));
  ASSERT_TRUE(sch);

  struct mtl_tasklet_ops t_ops = {};
  t_ops.name = "t_sleep_ctrl";
  t_ops.priv = &tctx;
  t_ops.start = test_tasklet_start;
  t_ops.stop = test_tasklet_stop;
  t_ops.handler = test_tasklet_handler;
  mtl_test::ScopedTasklet tasklet(mtl_sch_register_tasklet(sch.get(), &t_ops));
  ASSERT_TRUE(tasklet);

  ASSERT_EQ(mtl_sch_set_sleep_us(mt, 5000), 0);
  ASSERT_GE(sch.start(), 0);
  ASSERT_TRUE(mtl_test::wait_until(
      [&]() { return tctx.job.load(std::memory_order_acquire) > 0; }, 2000, 2));

  /* Idle + sleep enabled: 5 ms forced sleep limits 30 ms window to ~6 iterations. */
  tctx.handler_ret.store(MTL_TASKLET_ALL_DONE, std::memory_order_release);
  ASSERT_EQ(mtl_sch_enable_sleep(mt, sch_idx, true), 0);
  mtl_sleep_us(6000);
  const int j0 = tctx.job.load(std::memory_order_acquire);
  mtl_sleep_us(30000);
  const int delta_idle = tctx.job.load(std::memory_order_acquire) - j0;
  EXPECT_GE(delta_idle, 2);
  EXPECT_LE(delta_idle, 15);

  /* Pending work + sleep enabled: scheduler must busy-poll without sleeping. */
  tctx.handler_ret.store(MTL_TASKLET_HAS_PENDING, std::memory_order_release);
  mtl_sleep_us(8000);
  const int j1 = tctx.job.load(std::memory_order_acquire);
  mtl_sleep_us(20000);
  const int delta_busy = tctx.job.load(std::memory_order_acquire) - j1;
  EXPECT_GT(delta_busy, 100);

  /* Idle + sleep disabled: disabling sleep restores busy-poll. */
  ASSERT_EQ(mtl_sch_enable_sleep(mt, sch_idx, false), 0);
  tctx.handler_ret.store(MTL_TASKLET_ALL_DONE, std::memory_order_release);
  mtl_sleep_us(2000);
  const int j2 = tctx.job.load(std::memory_order_acquire);
  mtl_sleep_us(20000);
  const int delta_nosleep = tctx.job.load(std::memory_order_acquire) - j2;
  EXPECT_GT(delta_nosleep, 100);

  tasklet.reset();
  sch.reset();
  EXPECT_LT(mtl_sch_enable_sleep(mt, sch_idx, true), 0);
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

  /* Runtime unregister invokes stop callback, freezes job count, and frees a slot. */
  t0.reset();
  EXPECT_FALSE(ctx0.start.load(std::memory_order_acquire));
  EXPECT_EQ(ctx0.start_cnt.load(std::memory_order_acquire), 1);
  EXPECT_EQ(ctx0.stop_cnt.load(std::memory_order_acquire), 1);
  const int jobs0_after_unreg = ctx0.job.load(std::memory_order_acquire);
  mtl_sleep_us(5000);
  EXPECT_EQ(ctx0.job.load(std::memory_order_acquire), jobs0_after_unreg);

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
  EXPECT_EQ(ctx2.start_cnt.load(std::memory_order_acquire), 1);

  /* Stop/restart cycle invokes stop then start on surviving tasklets and resumes work. */
  ASSERT_EQ(sch.stop(), 0);
  EXPECT_FALSE(ctx2.start.load(std::memory_order_acquire));
  EXPECT_EQ(ctx2.stop_cnt.load(std::memory_order_acquire), 1);
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
  EXPECT_EQ(ctx2.start_cnt.load(std::memory_order_acquire), 2);

  ASSERT_EQ(sch.stop(), 0);
  EXPECT_FALSE(ctx2.start.load(std::memory_order_acquire));
  EXPECT_EQ(ctx2.stop_cnt.load(std::memory_order_acquire), 2);
  const int jobs2_after_stop = ctx2.job.load(std::memory_order_acquire);
  mtl_sleep_us(5000);
  EXPECT_EQ(ctx2.job.load(std::memory_order_acquire), jobs2_after_stop);
}
