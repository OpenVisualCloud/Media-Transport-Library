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
#ifndef WINDOWSENV
#include <numa.h>
#endif
#include "log.h"

enum test_args_cmd {
  TEST_ARG_UNKNOWN = 0,
  TEST_ARG_P_PORT = 0x100, /* start from end of ascii */
  TEST_ARG_R_PORT,
  TEST_ARG_LCORES,
  TEST_ARG_LOG_LEVEL,
  TEST_ARG_SCH_SESSION_QUOTA,
  TEST_ARG_DMA_DEV,
  TEST_ARG_CNI_THREAD,
  TEST_ARG_LIB_PTP,
  TEST_ARG_RX_MONO_POOL,
  TEST_ARG_RX_SEPARATE_VIDEO_LCORE,
  TEST_ARG_NB_TX_DESC,
  TEST_ARG_NB_RX_DESC,
  TEST_ARG_LEVEL,
};

static struct option test_args_options[] = {
    {"p_port", required_argument, 0, TEST_ARG_P_PORT},
    {"r_port", required_argument, 0, TEST_ARG_R_PORT},

    {"lcores", required_argument, 0, TEST_ARG_LCORES},
    {"log_level", required_argument, 0, TEST_ARG_LOG_LEVEL},
    {"level", required_argument, 0, TEST_ARG_LEVEL},
    {"sch_session_quota", required_argument, 0, TEST_ARG_SCH_SESSION_QUOTA},
    {"dma_dev", required_argument, 0, TEST_ARG_DMA_DEV},
    {"cni_thread", no_argument, 0, TEST_ARG_CNI_THREAD},
    {"ptp", no_argument, 0, TEST_ARG_LIB_PTP},
    {"rx_mono_pool", no_argument, 0, TEST_ARG_RX_MONO_POOL},
    {"rx_separate_lcore", no_argument, 0, TEST_ARG_RX_SEPARATE_VIDEO_LCORE},
    {"nb_tx_desc", required_argument, 0, TEST_ARG_NB_TX_DESC},
    {"nb_rx_desc", required_argument, 0, TEST_ARG_NB_RX_DESC},

    {0, 0, 0, 0}};

static struct st_tests_context* g_test_ctx;

struct st_tests_context* st_test_ctx(void) {
  return g_test_ctx;
}

static int test_args_dma_dev(struct st_init_params* p, const char* in_dev) {
  if (!in_dev) return -EIO;
  char devs[128];
  strncpy(devs, in_dev, 127);

  dbg("%s, dev list %s\n", __func__, devs);
  char* next_dev = strtok(devs, ",");
  while (next_dev) {
    dbg("next_dev: %s\n", next_dev);
    strncpy(p->dma_dev_port[p->num_dma_dev_port], next_dev, ST_PORT_MAX_LEN);
    p->num_dma_dev_port++;
    next_dev = strtok(NULL, ",");
  }
  return 0;
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
      case TEST_ARG_SCH_SESSION_QUOTA: /* unit: 1080p tx */
        p->data_quota_mbs_per_sch =
            atoi(optarg) * st20_1080p59_yuv422_10bit_bandwidth_mps();
        break;
      case TEST_ARG_DMA_DEV:
        test_args_dma_dev(p, optarg);
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
      case TEST_ARG_CNI_THREAD:
        p->flags |= ST_FLAG_CNI_THREAD;
        break;
      case TEST_ARG_RX_MONO_POOL:
        p->flags |= ST_FLAG_RX_QUEUE_MONO_POOL;
        break;
      case TEST_ARG_RX_SEPARATE_VIDEO_LCORE:
        p->flags |= ST_FLAG_RX_SEPARATE_VIDEO_LCORE;
        break;
      case TEST_ARG_LIB_PTP:
        p->flags |= ST_FLAG_PTP_ENABLE;
        p->ptp_get_time_fn = NULL; /* clear the user ptp func */
        break;
      case TEST_ARG_NB_TX_DESC:
        p->nb_tx_desc = atoi(optarg);
        break;
      case TEST_ARG_NB_RX_DESC:
        p->nb_rx_desc = atoi(optarg);
        break;
      case TEST_ARG_LEVEL:
        if (!strcmp(optarg, "all"))
          ctx->level = ST_TEST_LEVEL_ALL;
        else if (!strcmp(optarg, "mandatory"))
          ctx->level = ST_TEST_LEVEL_MANDATORY;
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
  r_ip[3] = p_ip[3] + 1;
}

static uint64_t test_ptp_from_real_time(void* priv) {
  auto ctx = (struct st_tests_context*)priv;
  struct timespec spec;
#ifndef WINDOWSENV
  clock_gettime(CLOCK_REALTIME, &spec);
  ctx->ptp_time = ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
#else
  unsigned __int64 t;
  union {
    unsigned __int64 u64;
    FILETIME ft;
  } ct;
  GetSystemTimePreciseAsFileTime(&ct.ft);
  t = ct.u64 - INT64_C(116444736000000000);
  spec.tv_sec = t / 10000000;
  spec.tv_nsec = ((int)(t % 10000000)) * 100;
  ctx->ptp_time = ((uint64_t)spec.tv_sec * NS_PER_S) + spec.tv_nsec;
#endif
  return ctx->ptp_time;
}

static void test_ctx_init(struct st_tests_context* ctx) {
  struct st_init_params* p = &ctx->para;
  int cpus_per_soc = 4;
  char* lcores_list = ctx->lcores_list;
  int pos = 0;
#ifndef WINDOWSENV
  int numa_nodes = 0;
  int max_cpus = 0;
#endif
  ctx->level = ST_TEST_LEVEL_MANDATORY;
#ifndef WINDOWSENV
  if (numa_available() >= 0) {
    numa_nodes = numa_max_node() + 1;
    max_cpus = numa_num_task_cpus();
  }
#endif
  memset(p, 0x0, sizeof(*p));
  p->flags = ST_FLAG_BIND_NUMA; /* default bind to numa */
  p->log_level = ST_LOG_LEVEL_WARNING;
  p->priv = ctx;
  p->ptp_get_time_fn = test_ptp_from_real_time;
  p->tx_sessions_cnt_max = 32;
  p->rx_sessions_cnt_max = 32;

  /* build default lcore list */
  pos += snprintf(lcores_list + pos, TEST_LCORE_LIST_MAX_LEN - pos, "0-%d",
                  cpus_per_soc - 1);
#ifndef WINDOWSENV
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
#endif
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
  ASSERT_TRUE(size > 0);
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

  uint64_t start_time_ns = st_test_get_monotonic_time();

  ret = RUN_ALL_TESTS();

  uint64_t end_time_ns = st_test_get_monotonic_time();
  int time_s = (end_time_ns - start_time_ns) / NS_PER_S;
  int time_least = 10;
  if (time_s < time_least) {
    /* wa for linkFlapErrDisabled in the hub */
    info("%s, sleep %ds before disable the port\n", __func__, time_least - time_s);
    sleep(time_least - time_s);
  }

  test_ctx_uinit(ctx);
  return ret;
}

int tx_next_frame(void* priv, uint16_t* next_frame_idx) {
  auto ctx = (tests_context*)priv;

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

void test_md5_dump(const char* tag, unsigned char* md5) {
  for (size_t i = 0; i < MD5_DIGEST_LENGTH; i++) {
    dbg("0x%02x ", md5[i]);
  }
  dbg(", %s done\n", tag);
}

int st_test_check_patter(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    if (p[i] != ((base + i) & 0xFF)) {
      err("%s, fail data 0x%x on %lu base 0x%x\n", __func__, p[i], i, base);
      return -EIO;
    }
  }

  return 0;
}

void md5_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  unsigned char result[MD5_DIGEST_LENGTH];
  while (!ctx->stop) {
    if (ctx->buf_q.empty()) {
      lck.lock();
      if (!ctx->stop) ctx->cv.wait(lck);
      lck.unlock();
      continue;
    } else {
      void* frame = ctx->buf_q.front();
      ctx->buf_q.pop();
      dbg("%s, frame %p\n", __func__, frame);
      int i;
      MD5((unsigned char*)frame, ctx->frame_size, result);
      for (i = 0; i < TEST_MD5_HIST_NUM; i++) {
        unsigned char* target_md5 = ctx->md5s[i];
        if (!memcmp(result, target_md5, MD5_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_MD5_HIST_NUM) {
        test_md5_dump("rx_error_md5", result);
        ctx->fail_cnt++;
      }
      ctx->check_md5_frame_cnt++;
      st_test_free(frame);
    }
  }
}
