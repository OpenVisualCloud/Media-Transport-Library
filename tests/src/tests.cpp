/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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
  TEST_ARG_TX_MONO_POOL,
  TEST_ARG_MONO_POOL,
  TEST_ARG_RX_SEPARATE_VIDEO_LCORE,
  TEST_ARG_MIGRATE_ENABLE,
  TEST_ARG_MIGRATE_DISABLE,
  TEST_ARG_NB_TX_DESC,
  TEST_ARG_NB_RX_DESC,
  TEST_ARG_LEVEL,
  TEST_ARG_AUTO_START_STOP,
  TEST_ARG_AF_XDP_ZC_DISABLE,
  TEST_ARG_START_QUEUE,
  TEST_ARG_P_START_QUEUE,
  TEST_ARG_R_START_QUEUE,
  TEST_ARG_HDR_SPLIT,
  TEST_ARG_TASKLET_THREAD,
  TEST_ARG_TSC_PACING,
  TEST_ARG_RXTX_SIMD_512,
  TEST_ARG_PACING_WAY,
  TEST_ARG_RSS_MODE,
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
    {"tx_mono_pool", no_argument, 0, TEST_ARG_TX_MONO_POOL},
    {"mono_pool", no_argument, 0, TEST_ARG_MONO_POOL},
    {"rx_separate_lcore", no_argument, 0, TEST_ARG_RX_SEPARATE_VIDEO_LCORE},
    {"migrate_enable", no_argument, 0, TEST_ARG_MIGRATE_ENABLE},
    {"migrate_disable", no_argument, 0, TEST_ARG_MIGRATE_DISABLE},
    {"nb_tx_desc", required_argument, 0, TEST_ARG_NB_TX_DESC},
    {"nb_rx_desc", required_argument, 0, TEST_ARG_NB_RX_DESC},
    {"auto_start_stop", no_argument, 0, TEST_ARG_AUTO_START_STOP},
    {"afxdp_zc_disable", no_argument, 0, TEST_ARG_AF_XDP_ZC_DISABLE},
    {"start_queue", required_argument, 0, TEST_ARG_START_QUEUE},
    {"p_start_queue", required_argument, 0, TEST_ARG_P_START_QUEUE},
    {"r_start_queue", required_argument, 0, TEST_ARG_R_START_QUEUE},
    {"hdr_split", no_argument, 0, TEST_ARG_HDR_SPLIT},
    {"tasklet_thread", no_argument, 0, TEST_ARG_TASKLET_THREAD},
    {"tsc", no_argument, 0, TEST_ARG_TSC_PACING},
    {"rxtx_simd_512", no_argument, 0, TEST_ARG_RXTX_SIMD_512},
    {"pacing_way", required_argument, 0, TEST_ARG_PACING_WAY},
    {"rss_mode", required_argument, 0, TEST_ARG_RSS_MODE},

    {0, 0, 0, 0}};

static struct st_tests_context* g_test_ctx;

struct st_tests_context* st_test_ctx(void) {
  return g_test_ctx;
}

static int test_args_dma_dev(struct mtl_init_params* p, const char* in_dev) {
  if (!in_dev) return -EIO;
  char devs[128] = {0};
  strncpy(devs, in_dev, 128 - 1);

  dbg("%s, dev list %s\n", __func__, devs);
  char* next_dev = strtok(devs, ",");
  while (next_dev && (p->num_dma_dev_port < MTL_DMA_DEV_MAX)) {
    dbg("next_dev: %s\n", next_dev);
    strncpy(p->dma_dev_port[p->num_dma_dev_port], next_dev, MTL_PORT_MAX_LEN - 1);
    p->num_dma_dev_port++;
    next_dev = strtok(NULL, ",");
  }
  return 0;
}

static int test_parse_args(struct st_tests_context* ctx, struct mtl_init_params* p,
                           int argc, char** argv) {
  int cmd = -1, opt_idx = 0;
  int nb;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", test_args_options, &opt_idx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case TEST_ARG_P_PORT:
        snprintf(p->port[MTL_PORT_P], sizeof(p->port[MTL_PORT_P]), "%s", optarg);
        p->num_ports++;
        break;
      case TEST_ARG_R_PORT:
        snprintf(p->port[MTL_PORT_R], sizeof(p->port[MTL_PORT_R]), "%s", optarg);
        p->num_ports++;
        break;
      case TEST_ARG_LCORES:
        p->lcores = optarg;
        break;
      case TEST_ARG_SCH_SESSION_QUOTA: /* unit: 1080p tx */
        nb = atoi(optarg);
        if (nb > 0 && nb < 100) {
          p->data_quota_mbs_per_sch = nb * st20_1080p59_yuv422_10bit_bandwidth_mps();
        }
        break;
      case TEST_ARG_DMA_DEV:
        test_args_dma_dev(p, optarg);
        break;
      case TEST_ARG_LOG_LEVEL:
        if (!strcmp(optarg, "debug"))
          p->log_level = MTL_LOG_LEVEL_DEBUG;
        else if (!strcmp(optarg, "info"))
          p->log_level = MTL_LOG_LEVEL_INFO;
        else if (!strcmp(optarg, "notice"))
          p->log_level = MTL_LOG_LEVEL_NOTICE;
        else if (!strcmp(optarg, "warning"))
          p->log_level = MTL_LOG_LEVEL_WARNING;
        else if (!strcmp(optarg, "error"))
          p->log_level = MTL_LOG_LEVEL_ERROR;
        else
          err("%s, unknow log level %s\n", __func__, optarg);
        break;
      case TEST_ARG_CNI_THREAD:
        p->flags |= MTL_FLAG_CNI_THREAD;
        break;
      case TEST_ARG_RX_MONO_POOL:
        p->flags |= MTL_FLAG_RX_MONO_POOL;
        break;
      case TEST_ARG_TX_MONO_POOL:
        p->flags |= MTL_FLAG_TX_MONO_POOL;
        break;
      case TEST_ARG_MONO_POOL:
        p->flags |= MTL_FLAG_RX_MONO_POOL;
        p->flags |= MTL_FLAG_TX_MONO_POOL;
        break;
      case TEST_ARG_RX_SEPARATE_VIDEO_LCORE:
        p->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
        break;
      case TEST_ARG_MIGRATE_ENABLE:
        p->flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
        p->flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
        break;
      case TEST_ARG_MIGRATE_DISABLE:
        p->flags &= ~MTL_FLAG_RX_VIDEO_MIGRATE;
        p->flags &= ~MTL_FLAG_TX_VIDEO_MIGRATE;
        break;
      case TEST_ARG_LIB_PTP:
        p->flags |= MTL_FLAG_PTP_ENABLE;
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
      case TEST_ARG_AUTO_START_STOP:
        p->flags |= MTL_FLAG_DEV_AUTO_START_STOP;
        break;
      case TEST_ARG_AF_XDP_ZC_DISABLE:
        p->flags |= MTL_FLAG_AF_XDP_ZC_DISABLE;
        break;
      case TEST_ARG_START_QUEUE:
        p->xdp_info[MTL_PORT_P].start_queue = atoi(optarg);
        p->xdp_info[MTL_PORT_R].start_queue = atoi(optarg);
        break;
      case TEST_ARG_P_START_QUEUE:
        p->xdp_info[MTL_PORT_P].start_queue = atoi(optarg);
        break;
      case TEST_ARG_R_START_QUEUE:
        p->xdp_info[MTL_PORT_R].start_queue = atoi(optarg);
        break;
      case TEST_ARG_HDR_SPLIT:
        ctx->hdr_split = true;
        break;
      case TEST_ARG_TASKLET_THREAD:
        p->flags |= MTL_FLAG_TASKLET_THREAD;
        break;
      case TEST_ARG_TSC_PACING:
        p->pacing = ST21_TX_PACING_WAY_TSC;
        break;
      case TEST_ARG_RXTX_SIMD_512:
        p->flags |= MTL_FLAG_RXTX_SIMD_512;
        break;
      case TEST_ARG_PACING_WAY:
        if (!strcmp(optarg, "auto"))
          p->pacing = ST21_TX_PACING_WAY_AUTO;
        else if (!strcmp(optarg, "rl"))
          p->pacing = ST21_TX_PACING_WAY_RL;
        else if (!strcmp(optarg, "tsn"))
          p->pacing = ST21_TX_PACING_WAY_TSN;
        else if (!strcmp(optarg, "tsc"))
          p->pacing = ST21_TX_PACING_WAY_TSC;
        else if (!strcmp(optarg, "ptp"))
          p->pacing = ST21_TX_PACING_WAY_PTP;
        else
          err("%s, unknow pacing way %s\n", __func__, optarg);
        break;
      case TEST_ARG_RSS_MODE:
        if (!strcmp(optarg, "l3"))
          p->rss_mode = MT_RSS_MODE_L3;
        else if (!strcmp(optarg, "l4"))
          p->rss_mode = MT_RSS_MODE_L4;
        else
          err("%s, unknow rss mode %s\n", __func__, optarg);
        break;
      default:
        break;
    }
  };

  return 0;
}

static void test_random_ip(struct st_tests_context* ctx) {
  struct mtl_init_params* p = &ctx->para;
  uint8_t* p_ip = mtl_p_sip_addr(p);
  uint8_t* r_ip = mtl_r_sip_addr(p);

  srand(st_test_get_monotonic_time());

  p_ip[0] = 197;
  p_ip[1] = rand() % 0xFF;
  p_ip[2] = rand() % 0xFF;
  p_ip[3] = rand() % 0xFF;
  r_ip[0] = p_ip[0];
  r_ip[1] = p_ip[1];
  r_ip[2] = p_ip[2];
  r_ip[3] = p_ip[3] + 1;

  p_ip = ctx->mcast_ip_addr[MTL_PORT_P];
  r_ip = ctx->mcast_ip_addr[MTL_PORT_R];
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
  struct mtl_init_params* p = &ctx->para;
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
  p->flags = MTL_FLAG_BIND_NUMA; /* default bind to numa */
  p->log_level = MTL_LOG_LEVEL_ERROR;
  p->priv = ctx;
  p->ptp_get_time_fn = test_ptp_from_real_time;
  p->tx_sessions_cnt_max = 16;
  p->rx_sessions_cnt_max = 16;
  /* defalut start queue set to 1 */
  p->xdp_info[MTL_PORT_P].start_queue = 1;
  p->xdp_info[MTL_PORT_R].start_queue = 1;

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
  mtl_uninit(ctx->handle);
  ctx->handle = NULL;
  st_test_free(ctx);
}

TEST(Misc, version) {
  auto version_display = mtl_version();
  info("st version: %s\n", version_display);

  uint32_t version_no =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR, MTL_VERSION_LAST);
  EXPECT_EQ((uint32_t)MTL_VERSION, version_no);
}

TEST(Misc, version_compare) {
  auto version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR + 1, MTL_VERSION_MINOR, MTL_VERSION_LAST);
  EXPECT_LT(MTL_VERSION, version_next);
  version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR + 1, MTL_VERSION_LAST);
  EXPECT_LT(MTL_VERSION, version_next);
  version_next =
      MTL_VERSION_NUM(MTL_VERSION_MAJOR, MTL_VERSION_MINOR, MTL_VERSION_LAST + 1);
  EXPECT_LT(MTL_VERSION, version_next);
}

static void mtl_memcpy_test(size_t size) {
  ASSERT_TRUE(size > 0);
  char src[size];
  char dst[size];

  for (size_t i = 0; i < size; i++) src[i] = i;
  memset(dst, 0, size);

  mtl_memcpy(dst, src, size);
  EXPECT_EQ(0, memcmp(src, dst, size));
}

TEST(Misc, memcpy) {
  mtl_memcpy_test(1);
  mtl_memcpy_test(4096);
  mtl_memcpy_test(4096 + 100);
}

static void hp_malloc_test(struct st_tests_context* ctx, size_t size, enum mtl_port port,
                           bool zero, bool expect_succ) {
  auto m_handle = ctx->handle;
  void* p;

  if (zero)
    p = mtl_hp_malloc(m_handle, size, port);
  else
    p = mtl_hp_zmalloc(m_handle, size, port);
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
    mtl_hp_free(m_handle, p);
  }
}

static void hp_malloc_tests(struct st_tests_context* ctx, enum mtl_port port, bool zero) {
  hp_malloc_test(ctx, 1, port, zero, true);
  hp_malloc_test(ctx, 1024, port, zero, true);
  hp_malloc_test(ctx, 1024 + 3, port, zero, true);
}

TEST(Misc, hp_malloc) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_tests(ctx, MTL_PORT_P, false);
  if (num_port > 1) hp_malloc_tests(ctx, MTL_PORT_R, false);
}

TEST(Misc, hp_zmalloc) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_tests(ctx, MTL_PORT_P, true);
  if (num_port > 1) hp_malloc_tests(ctx, MTL_PORT_R, true);
}

TEST(Misc, hp_malloc_expect_fail) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_test(ctx, 0, MTL_PORT_P, false, false);
  hp_malloc_test(ctx, 8, MTL_PORT_MAX, false, false);
  if (num_port > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, false, false);
}

TEST(Misc, hp_zmalloc_expect_fail) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_test(ctx, 0, MTL_PORT_P, true, false);
  hp_malloc_test(ctx, 8, MTL_PORT_MAX, true, false);
  if (num_port > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, true, false);
}

TEST(Misc, ptp) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;
  uint64_t ptp = mtl_ptp_read_time(handle);
  EXPECT_EQ(ptp, ctx->ptp_time);
  /* try again */
  st_usleep(1);
  ptp = mtl_ptp_read_time(handle);
  EXPECT_EQ(ptp, ctx->ptp_time);
}

static void st10_timestamp_test(uint32_t sampling_rate) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;

  uint64_t ptp1 = mtl_ptp_read_time(handle);
  uint32_t media1 = st10_tai_to_media_clk(ptp1, sampling_rate);
  /* sleep 100us */
  st_usleep(100);
  uint64_t ptp2 = mtl_ptp_read_time(handle);
  uint32_t media2 = st10_tai_to_media_clk(ptp2, sampling_rate);
  EXPECT_GT(ptp2, ptp1);
  EXPECT_GT(media2, media1);

  uint64_t ns_delta = st10_media_clk_to_ns(media2 - media1, sampling_rate);
  uint64_t expect_delta = ptp2 - ptp1;
  dbg("%s, delta %" PRIu64 " %" PRIu64 "\n", __func__, ns_delta, expect_delta);
  EXPECT_NEAR(ns_delta, expect_delta, expect_delta * 0.5);
}

TEST(Misc, st10_timestamp) {
  st10_timestamp_test(90 * 1000);
  st10_timestamp_test(48 * 1000);
  st10_timestamp_test(96 * 1000);
}

GTEST_API_ int main(int argc, char** argv) {
  struct st_tests_context* ctx;
  int ret;
  bool link_flap_wa = false;

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

  /* parse af xdp pmd info */
  for (int i = 0; i < ctx->para.num_ports; i++) {
    ctx->para.pmd[i] = mtl_pmd_by_port_name(ctx->para.port[i]);
    if (ctx->para.pmd[i] != MTL_PMD_DPDK_USER) {
      mtl_get_if_ip(ctx->para.port[i], ctx->para.sip_addr[i], ctx->para.netmask[i]);
      ctx->para.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
      ctx->para.tx_sessions_cnt_max = 8;
      ctx->para.rx_sessions_cnt_max = 8;
      ctx->para.xdp_info[i].queue_count = 8;
    } else {
      link_flap_wa = true;
    }
  }
  if (ctx->hdr_split) {
    ctx->para.nb_rx_hdr_split_queues = 1;
  }

  ctx->handle = mtl_init(&ctx->para);
  if (!ctx->handle) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

  st_test_st22_plugin_register(ctx);
  st_test_convert_plugin_register(ctx);

  uint64_t start_time_ns = st_test_get_monotonic_time();

  ret = RUN_ALL_TESTS();

  uint64_t end_time_ns = st_test_get_monotonic_time();
  int time_s = (end_time_ns - start_time_ns) / NS_PER_S;
  int time_least = 10;
  if (link_flap_wa && (time_s < time_least)) {
    /* wa for linkFlapErrDisabled in the hub */
    info("%s, sleep %ds before disable the port\n", __func__, time_least - time_s);
    sleep(time_least - time_s);
  }

  st_test_st22_plugin_unregister(ctx);
  st_test_convert_plugin_unregister(ctx);

  test_ctx_uinit(ctx);
  return ret;
}

int tx_next_frame(void* priv, uint16_t* next_frame_idx) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  ctx->fb_send++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

void sha_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  unsigned char result[SHA256_DIGEST_LENGTH];
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
      SHA256((unsigned char*)frame, ctx->frame_size, result);
      for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
        unsigned char* target_sha = ctx->shas[i];
        if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_SHA_HIST_NUM) {
        test_sha_dump("rx_error_sha", result);
        ctx->fail_cnt++;
      }
      ctx->check_sha_frame_cnt++;
      st_test_free(frame);
    }
  }
}

int tests_context_unit(tests_context* ctx) {
  for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
    if (ctx->frame_buf[frame]) st_test_free(ctx->frame_buf[frame]);
    ctx->frame_buf[frame] = NULL;
  }
  if (ctx->ooo_mapping) {
    st_test_free(ctx->ooo_mapping);
    ctx->ooo_mapping = NULL;
  }
  if (ctx->priv) {
    st_test_free(ctx->priv);
    ctx->priv = NULL;
  }
  if (ctx->ext_fb_malloc) {
    st_test_free(ctx->ext_fb_malloc);
    ctx->ext_fb_malloc = NULL;
  }
  if (ctx->ext_frames) {
    st_test_free(ctx->ext_frames);
    ctx->ext_frames = NULL;
  }

  return 0;
}

int test_ctx_notify_event(void* priv, enum st_event event, void* args) {
  if (event == ST_EVENT_VSYNC) {
    tests_context* s = (tests_context*)priv;
    s->vsync_cnt++;
    if (!s->first_vsync_time) s->first_vsync_time = st_test_get_monotonic_time();
#ifdef DEBUG
    struct st10_vsync_meta* meta = (struct st10_vsync_meta*)args;
    dbg("%s(%d,%p), epoch %" PRIu64 " vsync_cnt %d\n", __func__, s->idx, s, meta->epoch,
        s->vsync_cnt);
#endif
  }
  return 0;
}
