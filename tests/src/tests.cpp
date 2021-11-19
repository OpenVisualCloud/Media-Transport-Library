/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "tests.h"

#include <getopt.h>
#include <numa.h>

#include "log.h"

enum test_args_cmd {
  TEST_ARG_UNKNOWN = 0,
  TEST_ARG_P_PORT = 0x100, /* start from end of ascii */
  TEST_ARG_R_PORT,
  TEST_ARG_LCORES,
  TEST_ARG_LOG_LEVEL,
};

static struct option test_args_options[] = {
    {"p_port", required_argument, 0, TEST_ARG_P_PORT},
    {"r_port", required_argument, 0, TEST_ARG_R_PORT},

    {"lcores", required_argument, 0, TEST_ARG_LCORES},
    {"log_level", required_argument, 0, TEST_ARG_LOG_LEVEL},
    {0, 0, 0, 0}};

static struct st_tests_context* g_test_ctx;

struct st_tests_context* st_test_ctx(void) {
  return g_test_ctx;
}

static int test_parse_args(struct st_tests_context* ctx, struct st_init_params* p,
                           int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", test_args_options, &opt_idx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case TEST_ARG_P_PORT:
        snprintf(p->port[ST_PORT_P], sizeof(p->port[ST_PORT_P]), "%s", optarg);
        p->num_ports++;
        break;
      case TEST_ARG_R_PORT:
        snprintf(p->port[ST_PORT_R], sizeof(p->port[ST_PORT_R]), "%s", optarg);
        p->num_ports++;
        break;
      case TEST_ARG_LCORES:
        p->lcores = optarg;
        break;
      case TEST_ARG_LOG_LEVEL:
        if (!strcmp(optarg, "debug"))
          p->log_level = ST_LOG_LEVEL_DEBUG;
        else if (!strcmp(optarg, "info"))
          p->log_level = ST_LOG_LEVEL_INFO;
        else if (!strcmp(optarg, "warning"))
          p->log_level = ST_LOG_LEVEL_WARNING;
        else if (!strcmp(optarg, "error"))
          p->log_level = ST_LOG_LEVEL_ERROR;
        else
          err("%s, unknow log level %s\n", __func__, optarg);
        break;
      default:
        break;
    }
  };

  return 0;
}

static void test_random_ip(struct st_tests_context* ctx) {
  struct st_init_params* p = &ctx->para;
  uint8_t* p_ip = st_p_sip_addr(p);
  uint8_t* r_ip = st_r_sip_addr(p);

  srand(st_test_get_monotonic_time());

  p_ip[0] = 197;
  p_ip[1] = rand() % 0xFF;
  p_ip[2] = rand() % 0xFF;
  p_ip[3] = rand() % 0xFF;
  r_ip[0] = p_ip[0];
  r_ip[1] = p_ip[1];
  r_ip[2] = p_ip[2];
  r_ip[3] = p_ip[3] + 1;

  p_ip = ctx->mcast_ip_addr[ST_PORT_P];
  r_ip = ctx->mcast_ip_addr[ST_PORT_R];
  p_ip[0] = 239;
  p_ip[1] = rand() % 0xFF;
  p_ip[2] = rand() % 0xFF;
  p_ip[3] = rand() % 0xFF;
  r_ip[0] = p_ip[0];
  r_ip[1] = p_ip[1];
  r_ip[2] = p_ip[2];
  r_ip[3] = rand() % 0xFF;
}

static uint64_t test_ptp_from_real_time(void* priv) {
  auto ctx = (struct st_tests_context*)priv;
  struct timespec spec;

  clock_gettime(CLOCK_REALTIME, &spec);
  ctx->ptp_time = ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
  return ctx->ptp_time;
}

static void test_ctx_init(struct st_tests_context* ctx) {
  struct st_init_params* p = &ctx->para;
  int cpus_per_soc = 3;
  char* lcores_list = ctx->lcores_list;
  int pos = 0;
  int numa_nodes = 0;
  int max_cpus = 0;

  if (numa_available() >= 0) {
    numa_nodes = numa_max_node() + 1;
    max_cpus = numa_num_task_cpus();
  }

  memset(p, 0x0, sizeof(*p));
  p->flags = ST_FLAG_BIND_NUMA; /* default bind to numa */
  p->log_level = ST_LOG_LEVEL_ERROR;
  p->priv = ctx;
  p->ptp_get_time_fn = test_ptp_from_real_time;
  p->tx_sessions_cnt_max = 32;
  p->rx_sessions_cnt_max = 32;

  /* build default lcore list */
  pos += snprintf(lcores_list + pos, TEST_LCORE_LIST_MAX_LEN - pos, "0-%d",
                  cpus_per_soc - 1);
  /* build lcore list for other numa, e.g 0-2,28,29,30 for a two socket system */
  for (int numa = 1; numa < numa_nodes; numa++) {
    int cpus_add = 0;
    for (int cpu = 0; cpu < max_cpus; cpu++) {
      if (numa_node_of_cpu(cpu) == numa) {
        pos += snprintf(lcores_list + pos, TEST_LCORE_LIST_MAX_LEN - pos, ",%d", cpu);
        cpus_add++;
        if (cpus_add >= cpus_per_soc) break;
      }
    }
  }
  info("lcores_list: %s, max_cpus %d\n", ctx->lcores_list, max_cpus);
  p->lcores = ctx->lcores_list;
}

static void test_ctx_uinit(struct st_tests_context* ctx) {
  st_uninit(ctx->handle);
  ctx->handle = NULL;
  st_test_free(ctx);
}

TEST(Misc, version) {
  auto version_display = st_version();
  info("st version: %s\n", version_display);

  uint32_t version_no =
      ST_VERSION_NUM(ST_VERSION_MAJOR, ST_VERSION_MINOR, ST_VERSION_LAST);
  EXPECT_EQ(ST_VERSION, version_no);
}

TEST(Misc, version_compare) {
  auto version_next =
      ST_VERSION_NUM(ST_VERSION_MAJOR + 1, ST_VERSION_MINOR, ST_VERSION_LAST);
  EXPECT_LT(ST_VERSION, version_next);
  version_next = ST_VERSION_NUM(ST_VERSION_MAJOR, ST_VERSION_MINOR + 1, ST_VERSION_LAST);
  EXPECT_LT(ST_VERSION, version_next);
  version_next = ST_VERSION_NUM(ST_VERSION_MAJOR, ST_VERSION_MINOR, ST_VERSION_LAST + 1);
  EXPECT_LT(ST_VERSION, version_next);
}

static void st_memcpy_test(size_t size) {
  char src[size];
  char dst[size];

  for (size_t i = 0; i < size; i++) src[i] = i;
  memset(dst, 0, size);

  st_memcpy(dst, src, size);
  EXPECT_EQ(0, memcmp(src, dst, size));
}

TEST(Misc, memcpy) {
  st_memcpy_test(1);
  st_memcpy_test(4096);
  st_memcpy_test(4096 + 100);
}

static void hp_malloc_test(struct st_tests_context* ctx, size_t size, enum st_port port,
                           bool zero, bool expect_succ) {
  auto m_handle = ctx->handle;
  void* p;

  if (zero)
    p = st_hp_malloc(m_handle, size, port);
  else
    p = st_hp_zmalloc(m_handle, size, port);
  if (expect_succ)
    EXPECT_TRUE(p != NULL);
  else
    EXPECT_TRUE(p == NULL);
  if (p) {
    if (zero) {
      void* dst = malloc(size);
      memset(dst, 0, size);
      EXPECT_EQ(0, memcmp(p, dst, size));
      free(dst);
    }
    memset(p, 0, size);
    st_hp_free(m_handle, p);
  }
}

static void hp_malloc_tests(struct st_tests_context* ctx, enum st_port port, bool zero) {
  hp_malloc_test(ctx, 1, port, zero, true);
  hp_malloc_test(ctx, 1024, port, zero, true);
  hp_malloc_test(ctx, 1024 + 3, port, zero, true);
}

TEST(Misc, hp_malloc) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_tests(ctx, ST_PORT_P, false);
  if (num_port > 1) hp_malloc_tests(ctx, ST_PORT_R, false);
}

TEST(Misc, hp_zmalloc) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_tests(ctx, ST_PORT_P, true);
  if (num_port > 1) hp_malloc_tests(ctx, ST_PORT_R, true);
}

TEST(Misc, hp_malloc_expect_fail) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_test(ctx, 0, ST_PORT_P, false, false);
  hp_malloc_test(ctx, 8, ST_PORT_MAX, false, false);
  if (num_port > 1) hp_malloc_test(ctx, 0, ST_PORT_R, false, false);
}

TEST(Misc, hp_zmalloc_expect_fail) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_test(ctx, 0, ST_PORT_P, true, false);
  hp_malloc_test(ctx, 8, ST_PORT_MAX, true, false);
  if (num_port > 1) hp_malloc_test(ctx, 0, ST_PORT_R, true, false);
}

TEST(Misc, ptp) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;
  uint64_t ptp = st_ptp_read_time(handle);
  EXPECT_EQ(ptp, ctx->ptp_time);
  /* try again */
  usleep(1);
  ptp = st_ptp_read_time(handle);
  EXPECT_EQ(ptp, ctx->ptp_time);
}

GTEST_API_ int main(int argc, char** argv) {
  struct st_tests_context* ctx;
  int ret;

  testing::InitGoogleTest(&argc, argv);

  ctx = (struct st_tests_context*)st_test_zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }

  test_ctx_init(ctx);
  test_parse_args(ctx, &ctx->para, argc, argv);
  test_random_ip(ctx);
  g_test_ctx = ctx;

  ctx->handle = st_init(&ctx->para);
  if (!ctx->handle) {
    err("%s, st_init fail\n", __func__);
    return -EIO;
  }

  ret = RUN_ALL_TESTS();

  test_ctx_uinit(ctx);
  return ret;
}

int tx_next_frame(void* priv, uint16_t* next_frame_idx) {
  auto ctx = (struct tests_context*)priv;

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}