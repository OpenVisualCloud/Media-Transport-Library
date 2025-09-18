/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <mtl/mtl_sch_api.h>

#include "log.h"
#include "tests.hpp"

static void sch_create_test(mtl_handle mt) {
  struct mtl_sch_ops sch_ops;
  memset(&sch_ops, 0x0, sizeof(sch_ops));
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  mtl_sch_handle sch = mtl_sch_create(mt, &sch_ops);
  ASSERT_TRUE(sch != NULL);
  int ret = mtl_sch_start(sch);
  EXPECT_GE(ret, 0);
  /* sleep 1ms */
  mtl_sleep_us(1000 * 1);
  ret = mtl_sch_stop(sch);
  EXPECT_GE(ret, 0);
  ret = mtl_sch_free(sch);
  EXPECT_GE(ret, 0);
}

TEST(Sch, create_single) {
  auto ctx = st_test_ctx();
  sch_create_test(ctx->handle);
}

static void sch_create_max_test(mtl_handle mt, int max) {
  std::vector<mtl_sch_handle> schs(max);
  int cnt = 0;
  int ret;
  struct mtl_sch_ops sch_ops;
  memset(&sch_ops, 0x0, sizeof(sch_ops));
  sch_ops.name = "sch";
  sch_ops.nb_tasklets = 16;

  for (int i = 0; i < max; i++) {
    mtl_sch_handle sch = mtl_sch_create(mt, &sch_ops);
    if (!sch) break;
    ret = mtl_sch_start(sch);
    if (ret < 0) {
      ret = mtl_sch_free(sch);
      EXPECT_GE(ret, 0);
      break;
    }

    schs[cnt] = sch;
    cnt++;
  }

  info("%s, cnt: %d\n", __func__, cnt);
  EXPECT_GT(cnt, 0);
  mtl_sleep_us(1000 * 2);

  for (int i = 0; i < cnt; i++) {
    mtl_sch_handle sch = schs[i];
    ret = mtl_sch_stop(sch);
    EXPECT_GE(ret, 0);
    ret = mtl_sch_free(sch);
    EXPECT_GE(ret, 0);
  }
}

TEST(Sch, create_max) {
  auto ctx = st_test_ctx();
  sch_create_max_test(ctx->handle, 10);
}

struct sch_digest_test_para {
  int sch_cnt;
  int tasklets;
  bool runtime;
  bool test_auto_unregister;
};

static void sch_digest_test_para_init(struct sch_digest_test_para* para) {
  memset(para, 0, sizeof(*para));
  para->sch_cnt = 1;
  para->tasklets = 1;
  para->runtime = false;
  para->test_auto_unregister = false;
}

struct tasklet_test_ctx {
  int sch_idx;
  int tasklet_idx;
  bool start;
  int job;
  mtl_tasklet_handle handle;
};

static int test_tasklet_start(void* priv) {
  struct tasklet_test_ctx* ctx = (struct tasklet_test_ctx*)priv;
  ctx->start = true;
  return 0;
}

static int test_tasklet_stop(void* priv) {
  struct tasklet_test_ctx* ctx = (struct tasklet_test_ctx*)priv;
  ctx->start = false;
  return 0;
}

static int test_tasklet_handler(void* priv) {
  struct tasklet_test_ctx* ctx = (struct tasklet_test_ctx*)priv;
  ctx->job++;
  return 0;
}

static void sch_tasklet_digest_test(mtl_handle mt, struct sch_digest_test_para* para) {
  const int sch_cnt = para->sch_cnt;
  int tasklet_cnt = para->tasklets;
  std::vector<mtl_sch_handle> schs(sch_cnt);
  int ret;

  struct mtl_sch_ops sch_ops;
  memset(&sch_ops, 0x0, sizeof(sch_ops));
  sch_ops.name = "sch_test";
  sch_ops.nb_tasklets = tasklet_cnt;

  struct mtl_tasklet_ops ops;
  memset(&ops, 0x0, sizeof(ops));
  ops.name = "test";
  ops.start = test_tasklet_start;
  ops.stop = test_tasklet_stop;
  ops.handler = test_tasklet_handler;

  std::vector<tasklet_test_ctx*> tasklet_ctxs;
  tasklet_ctxs.resize((size_t)sch_cnt * tasklet_cnt);

  /* create the sch */
  for (int i = 0; i < sch_cnt; i++) {
    mtl_sch_handle sch = mtl_sch_create(mt, &sch_ops);
    ASSERT_TRUE(sch != NULL);
    if (!para->runtime) {
      for (int j = 0; j < tasklet_cnt; j++) {
        tasklet_test_ctx* ctx = new tasklet_test_ctx();
        ASSERT_TRUE(ctx != NULL);
        ctx->sch_idx = i;
        ctx->tasklet_idx = j;
        ops.priv = ctx;
        ctx->handle = mtl_sch_register_tasklet(sch, &ops);
        ASSERT_TRUE(ctx->handle != NULL);
        tasklet_ctxs[i * tasklet_cnt + j] = ctx;
      }
    }

    ret = mtl_sch_start(sch);
    EXPECT_GE(ret, 0);

    if (para->runtime) {
      for (int j = 0; j < tasklet_cnt; j++) {
        tasklet_test_ctx* ctx = new tasklet_test_ctx();
        ASSERT_TRUE(ctx != NULL);
        ctx->sch_idx = i;
        ctx->tasklet_idx = j;
        ctx->start = false;
        ops.priv = ctx;
        ctx->handle = mtl_sch_register_tasklet(sch, &ops);
        ASSERT_TRUE(ctx->handle != NULL);
        tasklet_ctxs[i * tasklet_cnt + j] = ctx;
      }
    }

    schs[i] = sch;
  }

  mtl_sleep_us(1000 * 1000);
  /* check if all tasklet started */
  for (int i = 0; i < sch_cnt * tasklet_cnt; i++) {
    tasklet_test_ctx* ctx = tasklet_ctxs[i];
    EXPECT_TRUE(ctx->start);
    EXPECT_GT(ctx->job, 0);
    if (para->runtime) {
      ret = mtl_sch_unregister_tasklet(ctx->handle);
      EXPECT_GE(ret, 0);
    }
  }

  for (int i = 0; i < sch_cnt; i++) {
    mtl_sch_handle sch = schs[i];
    ret = mtl_sch_stop(sch);
    EXPECT_GE(ret, 0);
  }

  if (!para->runtime && !para->test_auto_unregister) {
    for (int i = 0; i < sch_cnt * tasklet_cnt; i++) {
      tasklet_test_ctx* ctx = tasklet_ctxs[i];
      ret = mtl_sch_unregister_tasklet(ctx->handle);
      EXPECT_GE(ret, 0);
    }
  }

  for (int i = 0; i < sch_cnt; i++) {
    mtl_sch_handle sch = schs[i];
    ret = mtl_sch_free(sch);
    EXPECT_GE(ret, 0);
  }

  /* check if all tasklet stopped */
  for (int i = 0; i < sch_cnt * tasklet_cnt; i++) {
    tasklet_test_ctx* ctx = tasklet_ctxs[i];
    EXPECT_FALSE(ctx->start);
    delete tasklet_ctxs[i];
  }
}

TEST(Sch, tasklet_single) {
  auto ctx = st_test_ctx();
  struct sch_digest_test_para para;

  sch_digest_test_para_init(&para);
  para.test_auto_unregister = true;
  sch_tasklet_digest_test(ctx->handle, &para);
}

TEST(Sch, tasklet_multi) {
  auto ctx = st_test_ctx();
  struct sch_digest_test_para para;

  sch_digest_test_para_init(&para);
  para.sch_cnt = 2;
  para.tasklets = 8;
  sch_tasklet_digest_test(ctx->handle, &para);
}

TEST(Sch, tasklet_runtime) {
  auto ctx = st_test_ctx();
  struct sch_digest_test_para para;

  sch_digest_test_para_init(&para);
  para.sch_cnt = 2;
  para.tasklets = 4;
  para.runtime = true;
  sch_tasklet_digest_test(ctx->handle, &para);
}
