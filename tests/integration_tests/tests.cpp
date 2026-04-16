/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tests.hpp"

#include <getopt.h>
#ifndef WINDOWSENV
#include <numa.h>
#endif
#include <arpa/inet.h>

#include "log.h"

enum test_args_cmd {
  TEST_ARG_UNKNOWN = 0,
  TEST_ARG_P_PORT = 0x100, /* start from end of ascii */
  TEST_ARG_R_PORT,
  TEST_ARG_P_SIP,
  TEST_ARG_PORT_LIST,
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
  TEST_ARG_QUEUE_CNT,
  TEST_ARG_HDR_SPLIT,
  TEST_ARG_TASKLET_THREAD,
  TEST_ARG_TSC_PACING,
  TEST_ARG_RXTX_SIMD_512,
  TEST_ARG_PACING_WAY,
  TEST_ARG_RSS_MODE,
  TEST_ARG_TX_NO_CHAIN,
  TEST_ARG_IOVA_MODE,
  TEST_ARG_MULTI_SRC_PORT,
  TEST_ARG_DHCP,
  TEST_ARG_MCAST_ONLY,
  TEST_ARG_ALLOW_ACROSS_NUMA_CORE,
  TEST_ARG_AUDIO_TX_PACING,
  TEST_ARG_NOCTX_TESTS
};

static struct option test_args_options[] = {
    {"p_port", required_argument, 0, TEST_ARG_P_PORT},
    {"r_port", required_argument, 0, TEST_ARG_R_PORT},
    {"p_sip", required_argument, 0, TEST_ARG_P_SIP},
    {"port_list", required_argument, 0, TEST_ARG_PORT_LIST},

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
    {"queue_cnt", required_argument, 0, TEST_ARG_QUEUE_CNT},
    {"hdr_split", no_argument, 0, TEST_ARG_HDR_SPLIT},
    {"tasklet_thread", no_argument, 0, TEST_ARG_TASKLET_THREAD},
    {"tsc", no_argument, 0, TEST_ARG_TSC_PACING},
    {"rxtx_simd_512", no_argument, 0, TEST_ARG_RXTX_SIMD_512},
    {"pacing_way", required_argument, 0, TEST_ARG_PACING_WAY},
    {"rss_mode", required_argument, 0, TEST_ARG_RSS_MODE},
    {"tx_no_chain", no_argument, 0, TEST_ARG_TX_NO_CHAIN},
    {"iova_mode", required_argument, 0, TEST_ARG_IOVA_MODE},
    {"multi_src_port", no_argument, 0, TEST_ARG_MULTI_SRC_PORT},
    {"dhcp", no_argument, 0, TEST_ARG_DHCP},
    {"mcast_only", no_argument, 0, TEST_ARG_MCAST_ONLY},
    {"allow_across_numa_core", no_argument, 0, TEST_ARG_ALLOW_ACROSS_NUMA_CORE},
    {"audio_tx_pacing", required_argument, 0, TEST_ARG_AUDIO_TX_PACING},
    {"no_ctx_tests", no_argument, 0, TEST_ARG_NOCTX_TESTS},

    {0, 0, 0, 0}};

static struct st_tests_context* g_test_ctx;

struct st_tests_context* st_test_ctx(void) {
  return g_test_ctx;
}

static int test_args_dma_dev(struct mtl_init_params* p, const char* in_dev) {
  if (!in_dev) return -EIO;
  char devs[128] = {0};
  snprintf(devs, 128 - 1, "%s", in_dev);

  dbg("%s, dev list %s\n", __func__, devs);
  char* next_dev = strtok(devs, ",");
  while (next_dev && (p->num_dma_dev_port < MTL_DMA_DEV_MAX)) {
    dbg("next_dev: %s\n", next_dev);
    snprintf(p->dma_dev_port[p->num_dma_dev_port], MTL_PORT_MAX_LEN - 1, "%s", next_dev);
    p->num_dma_dev_port++;
    next_dev = strtok(NULL, ",");
  }
  return 0;
}

void test_parse_port_list(struct mtl_init_params* p, const char* in_list) {
  if (!in_list) return;
  char list[MTL_PORT_MAX * MTL_PORT_MAX_LEN] = {0};
  snprintf(list, sizeof(list) - 1, "%s", in_list);

  err("%s, port list %s\n", __func__, list);
  char* next_port = strtok(list, ",");
  while (next_port && (p->num_ports < MTL_PORT_MAX)) {
    err("next_port: %s\n", next_port);
    snprintf(p->port[p->num_ports], MTL_PORT_MAX_LEN - 1, "%s", next_port);
    p->num_ports++;
    next_port = strtok(NULL, ",");
  }
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
      case TEST_ARG_P_SIP:
        inet_pton(AF_INET, optarg, mtl_p_sip_addr(p));
        ctx->user_p_sip = true;
        break;
      case TEST_ARG_PORT_LIST:
        test_parse_port_list(p, optarg);
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
          p->log_level = MTL_LOG_LEVEL_ERR;
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
      case TEST_ARG_QUEUE_CNT: {
        uint16_t cnt = atoi(optarg);
        p->tx_queues_cnt[MTL_PORT_P] = cnt;
        p->tx_queues_cnt[MTL_PORT_R] = cnt;
        p->rx_queues_cnt[MTL_PORT_P] = cnt;
        p->rx_queues_cnt[MTL_PORT_R] = cnt;
        break;
      }
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
        else if (!strcmp(optarg, "be"))
          p->pacing = ST21_TX_PACING_WAY_BE;
        else
          err("%s, unknow pacing way %s\n", __func__, optarg);
        break;
      case TEST_ARG_RSS_MODE:
        if (!strcmp(optarg, "l3"))
          p->rss_mode = MTL_RSS_MODE_L3;
        else if (!strcmp(optarg, "l3_l4"))
          p->rss_mode = MTL_RSS_MODE_L3_L4;
        else if (!strcmp(optarg, "none"))
          p->rss_mode = MTL_RSS_MODE_NONE;
        else
          err("%s, unknow rss mode %s\n", __func__, optarg);
        break;
      case TEST_ARG_TX_NO_CHAIN:
        p->flags |= MTL_FLAG_TX_NO_CHAIN;
        break;
      case TEST_ARG_IOVA_MODE:
        if (!strcmp(optarg, "va"))
          p->iova_mode = MTL_IOVA_MODE_VA;
        else if (!strcmp(optarg, "pa"))
          p->iova_mode = MTL_IOVA_MODE_PA;
        else
          err("%s, unknow iova mode %s\n", __func__, optarg);
        break;
      case TEST_ARG_MULTI_SRC_PORT:
        p->flags |= MTL_FLAG_MULTI_SRC_PORT;
        break;
      case TEST_ARG_DHCP:
        for (int port = 0; port < MTL_PORT_MAX; ++port)
          p->net_proto[port] = MTL_PROTO_DHCP;
        ctx->dhcp = true;
        break;
      case TEST_ARG_MCAST_ONLY:
        ctx->mcast_only = true;
        break;
      case TEST_ARG_ALLOW_ACROSS_NUMA_CORE:
        p->flags |= MTL_FLAG_ALLOW_ACROSS_NUMA_CORE;
        break;
      case TEST_ARG_AUDIO_TX_PACING:
        if (!strcmp(optarg, "auto"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_AUTO;
        else if (!strcmp(optarg, "rl"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_RL;
        else if (!strcmp(optarg, "tsc"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_TSC;
        else
          err("%s, unknow audio tx pacing %s\n", __func__, optarg);
        break;
      case TEST_ARG_NOCTX_TESTS:
        ctx->noctx_tests = true;
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

  /* Only generate random IP if user didn't specify one */
  if (!ctx->user_p_sip) {
    p_ip[0] = 197;
    p_ip[1] = rand() % 0xFF;
    p_ip[2] = rand() % 0xFF;
    p_ip[3] = 1;
  }

  /* add interfaces ip addresses */
  for (int i = MTL_PORT_R; p->port[i][0] != '\0'; i++) {
    /* Skip if user already set this port's IP */
    if (i == MTL_PORT_P && ctx->user_p_sip) continue;
    p->sip_addr[i][0] = p_ip[0];
    p->sip_addr[i][1] = p_ip[1];
    p->sip_addr[i][2] = p_ip[2];
    p->sip_addr[i][3] = p_ip[0] + i;
  }

  srand(st_test_get_monotonic_time());

  uint8_t* p_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_P];
  uint8_t* r_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_R];

  p_ip_multicast[0] = 239;
  p_ip_multicast[1] = p->sip_addr[MTL_PORT_P][1];
  p_ip_multicast[2] = p->sip_addr[MTL_PORT_P][2];
  p_ip_multicast[3] = p->sip_addr[MTL_PORT_P][3];
  r_ip_multicast[0] = p_ip_multicast[0];
  r_ip_multicast[1] = p_ip_multicast[1];
  r_ip_multicast[2] = p_ip_multicast[2];
  r_ip_multicast[3] = p_ip_multicast[3] + 1;

  if (ctx->mcast_only) {
    r_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_2];
    r_ip_multicast[0] = p_ip_multicast[0];
    r_ip_multicast[1] = p_ip_multicast[1];
    r_ip_multicast[2] = p_ip_multicast[2];
    r_ip_multicast[3] = p_ip_multicast[3] + 2;
  }
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

  ctx->level = ST_TEST_LEVEL_MANDATORY;
  memset(p, 0x0, sizeof(*p));
  p->flags = MTL_FLAG_BIND_NUMA; /* default bind to numa */
  p->flags |= MTL_FLAG_RANDOM_SRC_PORT;
  p->flags |= MTL_FLAG_CNI_TASKLET; /* for rtcp test */
  p->log_level = MTL_LOG_LEVEL_ERR;
  p->priv = ctx;
  p->ptp_get_time_fn = test_ptp_from_real_time;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    p->tx_queues_cnt[i] = 8;
    p->rx_queues_cnt[i] = 8;
  }

  /* by deafult don't limit cores */
  memset(ctx->lcores_list, 0, TEST_LCORE_LIST_MAX_LEN);
  p->lcores = NULL;
}

static void test_ctx_uinit(struct st_tests_context* ctx) {
  if (ctx->handle) {
    mtl_uninit(ctx->handle);
    ctx->handle = NULL;
  }
  st_test_free(ctx);
}

TEST(Misc, version) {
  auto version_display = mtl_version();
  info("MTL version: %s\n", version_display);

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
  char* src = new char[size];
  char* dst = new char[size];

  for (size_t i = 0; i < size; i++) src[i] = i;
  memset(dst, 0, size);

  mtl_memcpy(dst, src, size);
  EXPECT_EQ(0, memcmp(src, dst, size));

  delete[] src;
  delete[] dst;
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
  if (num_port > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, false, false);
}

TEST(Misc, hp_zmalloc_expect_fail) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int num_port = st_test_num_port(ctx);

  hp_malloc_test(ctx, 0, MTL_PORT_P, true, false);
  if (num_port > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, true, false);
}

TEST(Misc, ptp) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;
  uint64_t real_time;
  uint64_t diff;

  /* the first read */
  uint64_t ptp = mtl_ptp_read_time(handle);
  EXPECT_EQ(ptp, ctx->ptp_time);

  for (int i = 0; i < 5; i++) {
    /* try again */
    st_usleep(1000 * 2);
    ptp = mtl_ptp_read_time(handle);
    real_time = test_ptp_from_real_time(ctx);
    if (ptp > real_time)
      diff = ptp - real_time;
    else
      diff = real_time - ptp;
    EXPECT_LT(diff, NS_PER_US * 5);
  }
}

TEST(Misc, log_level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;
  int ret;

  enum mtl_log_level orig_level = mtl_get_log_level(handle);
  ret = mtl_set_log_level(handle, MTL_LOG_LEVEL_INFO);
  EXPECT_GE(ret, 0);
  ret = mtl_set_log_level(handle, MTL_LOG_LEVEL_ERR);
  EXPECT_GE(ret, 0);
  ret = mtl_set_log_level(handle, orig_level);
  EXPECT_GE(ret, 0);
}

TEST(Misc, get_numa_id) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto handle = ctx->handle;
  int ret;

  ret = mtl_get_numa_id(handle, MTL_PORT_P);
  EXPECT_GE(ret, 0);

  if (ctx->para.num_ports > 1) {
    ret = mtl_get_numa_id(handle, MTL_PORT_R);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_get_numa_id(handle, (enum mtl_port)MTL_PORT_MAX);
  EXPECT_LT(ret, 0);
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

TEST(Misc, St10_timestamp) {
  st10_timestamp_test(90 * 1000);
  st10_timestamp_test(48 * 1000);
  st10_timestamp_test(96 * 1000);
}

namespace {
constexpr uint32_t kVideoSamplingRate = 90 * 1000;
constexpr uint64_t kStartTai = 1764762541892350000ULL;
constexpr uint64_t kNsPerSecond = 1000000000ULL;

static uint64_t gcd64(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

static uint64_t tai_after_iterations(uint64_t start, uint64_t step_num, uint64_t step_den,
                                     size_t iteration) {
  const long double step = (long double)step_num / (long double)step_den;
  const long double offset = step * (long double)iteration;
  return start + (uint64_t)offset;
}

static uint32_t expected_floor(uint64_t numerator, uint64_t denominator) {
  return (uint32_t)(numerator / denominator);
}

static uint32_t expected_ceil(uint64_t numerator, uint64_t denominator) {
  return (uint32_t)((numerator + denominator - 1) / denominator);
}

struct cadence_case {
  const char* label;
  uint32_t sampling_rate;
  uint64_t step_num;
  uint64_t step_den;
  size_t samples;
};

static void ExpectCadence(const cadence_case& tc) {
  SCOPED_TRACE(tc.label);
  const uint64_t diff_num = tc.step_num * tc.sampling_rate;
  const uint64_t diff_den = tc.step_den * kNsPerSecond;
  const uint32_t diff_floor = expected_floor(diff_num, diff_den);
  const uint32_t diff_ceil = expected_ceil(diff_num, diff_den);
  uint32_t prev = st10_tai_to_media_clk(kStartTai, tc.sampling_rate);

  if (diff_floor == diff_ceil) {
    for (size_t i = 1; i < tc.samples; ++i) {
      const uint64_t tai_ns =
          tai_after_iterations(kStartTai, tc.step_num, tc.step_den, i);
      const uint32_t current = st10_tai_to_media_clk(tai_ns, tc.sampling_rate);
      const uint32_t diff = current - prev;
      EXPECT_EQ(diff, diff_floor) << "iteration=" << i << " diff=" << diff;
      prev = current;
    }
    return;
  }

  const uint64_t remainder = diff_num % diff_den;
  ASSERT_NE(remainder, 0ULL);

  const uint64_t g = gcd64(remainder, diff_den);
  const uint32_t ceil_run = (uint32_t)(remainder / g);
  const uint32_t floor_run = (uint32_t)((diff_den / g) - ceil_run);

  bool saw_floor = false;
  bool saw_ceil = false;
  uint32_t run_value = 0;
  uint32_t run_length = 0;
  bool have_run = false;

  for (size_t i = 1; i < tc.samples; ++i) {
    const uint64_t tai_ns = tai_after_iterations(kStartTai, tc.step_num, tc.step_den, i);
    const uint32_t current = st10_tai_to_media_clk(tai_ns, tc.sampling_rate);

    const uint32_t diff = current - prev;
    EXPECT_GE(diff, diff_floor) << "iteration=" << i << " diff=" << diff;
    EXPECT_LE(diff, diff_ceil) << "iteration=" << i << " diff=" << diff;
    EXPECT_TRUE(diff == diff_floor || diff == diff_ceil)
        << "iteration=" << i << " diff=" << diff << " allowed {" << diff_floor << ", "
        << diff_ceil << "}";

    if (!have_run || diff != run_value) {
      run_value = diff;
      run_length = 1;
      have_run = true;
    } else {
      run_length += 1;
    }

    const uint32_t allowed_run = (diff == diff_ceil) ? ceil_run : floor_run;
    EXPECT_LE(run_length, allowed_run)
        << "iteration=" << i << " diff=" << diff << " exceeded cadence run";

    if (diff == diff_floor) saw_floor = true;
    if (diff == diff_ceil) saw_ceil = true;

    prev = current;
  }

  EXPECT_TRUE(saw_floor) << "missing floor diff";
  EXPECT_TRUE(saw_ceil) << "missing ceil diff";
}

} /* namespace */

TEST(Misc, TaiToMClkRoundsDownOnExactHalf) {
  constexpr uint64_t tie_tai = 50000ULL;  // produces remainder == divisor / 2
  const uint32_t result = st10_tai_to_media_clk(tie_tai, kVideoSamplingRate);
  EXPECT_EQ(result, 4U);
}

TEST(Misc, TaiToMClkRoundsUpWhenPastHalf) {
  constexpr uint64_t tai = 5556ULL;  // produces remainder > divisor / 2
  const uint32_t result = st10_tai_to_media_clk(tai, kVideoSamplingRate);
  EXPECT_EQ(result, 1U);
}

TEST(Misc, TaiToMClkMatchesCommonFrameRates) {
  const cadence_case cases[] = {
      {"59.94fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 60000ULL, 200},
      {"29.97fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 30000ULL, 120},
      {"23.98fps", kVideoSamplingRate, 1001ULL * kNsPerSecond, 24000ULL, 120},
      {"120fps", kVideoSamplingRate, kNsPerSecond, 120ULL, 120},
      {"50fps", kVideoSamplingRate, kNsPerSecond, 50ULL, 120},
  };

  for (size_t idx = 0; idx < (sizeof(cases) / sizeof(cases[0])); ++idx) {
    ExpectCadence(cases[idx]);
  }
}

TEST(Misc, MClkToTaiConvertsExactSecondWithoutRounding) {
  const uint64_t ns = st10_media_clk_to_ns(kVideoSamplingRate, kVideoSamplingRate);
  EXPECT_EQ(ns, kNsPerSecond);
}

TEST(Misc, MClkToTaiRoundsUpWhenPastHalf) {
  constexpr uint32_t media_ticks = 5;  // remainder 50,000 (> 45,000)
  const uint64_t ns = st10_media_clk_to_ns(media_ticks, kVideoSamplingRate);
  EXPECT_EQ(ns, 55556ULL);
}

TEST(Misc, MClkToTaiRoundsDownOnExactHalf) {
  constexpr uint32_t kCustomSamplingRate = 1024;
  constexpr uint32_t media_ticks = 1;  // remainder == sampling_rate / 2
  const uint64_t ns = st10_media_clk_to_ns(media_ticks, kCustomSamplingRate);
  EXPECT_EQ(ns, 976562ULL);
}

TEST(Misc, MClkToTaiHandlesAudioSamplingRates) {
  struct media_case {
    const char* label;
    uint32_t sampling_rate;
    uint32_t ticks;
    uint64_t expected_ns;
  };

  const media_case cases[] = {
      {"48kHz one sample", 48 * 1000, 1, 20833ULL},
      {"48kHz millisecond", 48 * 1000, 48, 1000000ULL},
      {"96kHz rounding", 96 * 1000, 5, 52083ULL},
      {"44.1kHz fractional", 44100, 147, 3333333ULL},
  };

  for (size_t idx = 0; idx < (sizeof(cases) / sizeof(cases[0])); ++idx) {
    const media_case& tc = cases[idx];
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(st10_media_clk_to_ns(tc.ticks, tc.sampling_rate), tc.expected_ns);
  }
}

TEST(St10Conversions, ZeroSamplingRateIsGraceful) {
  EXPECT_EQ(st10_tai_to_media_clk(123456789ULL, 0), 0U);
  EXPECT_EQ(st10_media_clk_to_ns(1234U, 0), 0U);
}

static int run_all_test(int argc, char** argv, struct st_tests_context* ctx) {
  bool link_flap_wa = false;
  int ret;

  /* parse af xdp pmd info */
  for (int i = 0; i < ctx->para.num_ports; i++) {
    ctx->para.pmd[i] = mtl_pmd_by_port_name(ctx->para.port[i]);
    if (ctx->para.pmd[i] != MTL_PMD_DPDK_USER) {
      ctx->para.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
    } else {
      link_flap_wa = true;
    }
  }
  if (ctx->hdr_split) {
    ctx->para.nb_rx_hdr_split_queues = 1;
  }

  if (!ctx->noctx_tests) {
    ctx->handle = mtl_init(&ctx->para);
    if (!ctx->handle) {
      err("%s, mtl_init fail\n", __func__);
      return -EIO;
    }
  }

  for (int i = 0; i < ctx->para.num_ports; i++) {
    mtl_port_ip_info(ctx->handle, (enum mtl_port)i, ctx->para.sip_addr[i],
                     ctx->para.netmask[i], ctx->para.gateway[i]);
    uint8_t* ip = ctx->para.sip_addr[i];
    info("%s, if ip %u.%u.%u.%u for port %s\n", __func__, ip[0], ip[1], ip[2], ip[3],
         ctx->para.port[i]);
  }

  if (ctx->para.num_ports > 1) {
    if (0 == strcmp(ctx->para.port[MTL_PORT_P], ctx->para.port[MTL_PORT_R])) {
      /* for test with --p_port kernel:lo --r_port kernel:lo */
      ctx->same_dual_port = true;
    }
  }

  ctx->iova = mtl_iova_mode_get(ctx->handle);
  ctx->rss_mode = mtl_rss_mode_get(ctx->handle);

  st_test_st22_plugin_register(ctx);
  st_test_convert_plugin_register(ctx);

  uint64_t start_time_ns = st_test_get_monotonic_time();

  ret = RUN_ALL_TESTS();

  uint64_t end_time_ns = st_test_get_monotonic_time();
  int time_s = (end_time_ns - start_time_ns) / NS_PER_S;
  int time_least = 10;

  if (!ctx->noctx_tests && link_flap_wa && (time_s < time_least)) {
    /* wa for linkFlapErrDisabled in the hub */
    info("%s, sleep %ds before disable the port\n", __func__, time_least - time_s);
    sleep(time_least - time_s);
  }

  st_test_st22_plugin_unregister(ctx);
  st_test_convert_plugin_unregister(ctx);
  test_ctx_uinit(ctx);

  return ret;
}

bool filter_includes_no_ctx_tests(const std::string& filter) {
  if (filter == "*" || filter.empty()) return true;
  if (filter.rfind("NOCTX") != std::string::npos) return true;
  return false;
}

GTEST_API_ int main(int argc, char** argv) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  int ret = 1;

  ctx = (struct st_tests_context*)st_test_zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }

  testing::InitGoogleTest(&argc, argv);
  test_ctx_init(ctx);
  test_parse_args(ctx, &ctx->para, argc, argv);
  test_random_ip(ctx);
  g_test_ctx = ctx;
  ret = run_all_test(argc, argv, ctx);

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
        ctx->sha_fail_cnt++;
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
  if (ctx->dma_mem) {
    /* dma_mem is owned by the tests_context when set */
    mtl_dma_mem_free(ctx->ctx->handle, ctx->dma_mem);
    ctx->dma_mem = NULL;
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
