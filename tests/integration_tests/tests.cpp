/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tests.hpp"

#include <arpa/inet.h>
#include <getopt.h>

#include <string>
#include <vector>

#include "log.h"
#include "test_runtime.hpp"

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
  TEST_ARG_DMA_AUTO,
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
  TEST_ARG_AUTO_START_STOP,
  TEST_ARG_AF_XDP_ZC_DISABLE,
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
  TEST_ARG_SINGLE_PROCESS
};

static struct option test_args_options[] = {
    {"p_port", required_argument, 0, TEST_ARG_P_PORT},
    {"r_port", required_argument, 0, TEST_ARG_R_PORT},
    {"p_sip", required_argument, 0, TEST_ARG_P_SIP},
    {"port_list", required_argument, 0, TEST_ARG_PORT_LIST},

    {"lcores", required_argument, 0, TEST_ARG_LCORES},
    {"log_level", required_argument, 0, TEST_ARG_LOG_LEVEL},
    {"sch_session_quota", required_argument, 0, TEST_ARG_SCH_SESSION_QUOTA},
    {"dma_dev", required_argument, 0, TEST_ARG_DMA_DEV},
    {"dma_auto", no_argument, nullptr, TEST_ARG_DMA_AUTO},
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
    {"single_process", no_argument, 0, TEST_ARG_SINGLE_PROCESS},

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

static void test_parse_port_list(struct mtl_init_params* p, const char* in_list) {
  if (!in_list) return;
  char list[MTL_PORT_MAX * MTL_PORT_MAX_LEN] = {0};
  snprintf(list, sizeof(list) - 1, "%s", in_list);

  dbg("%s, port list %s\n", __func__, list);
  memset(p->port, 0, sizeof(p->port));
  p->num_ports = 0;
  char* next_port = strtok(list, ",");
  while (next_port && (p->num_ports < MTL_PORT_MAX)) {
    dbg("next_port: %s\n", next_port);
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
        if (p->num_ports < MTL_PORT_P + 1) p->num_ports = MTL_PORT_P + 1;
        break;
      case TEST_ARG_R_PORT:
        snprintf(p->port[MTL_PORT_R], sizeof(p->port[MTL_PORT_R]), "%s", optarg);
        if (p->num_ports < MTL_PORT_R + 1) p->num_ports = MTL_PORT_R + 1;
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
      case TEST_ARG_DMA_AUTO:
        p->flags |= MTL_FLAG_DMA_AUTO;
        break;
      case TEST_ARG_LOG_LEVEL:
        if (!strcmp(optarg, "debug")) {
          p->log_level = MTL_LOG_LEVEL_DEBUG;
          ctx->user_log_level = true;
        } else if (!strcmp(optarg, "info")) {
          p->log_level = MTL_LOG_LEVEL_INFO;
          ctx->user_log_level = true;
        } else if (!strcmp(optarg, "notice")) {
          p->log_level = MTL_LOG_LEVEL_NOTICE;
          ctx->user_log_level = true;
        } else if (!strcmp(optarg, "warning")) {
          p->log_level = MTL_LOG_LEVEL_WARNING;
          ctx->user_log_level = true;
        } else if (!strcmp(optarg, "error")) {
          p->log_level = MTL_LOG_LEVEL_ERR;
          ctx->user_log_level = true;
        } else {
          err("%s, unknow log level %s\n", __func__, optarg);
        }
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
      case TEST_ARG_SINGLE_PROCESS:
        ctx->single_process = true;
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

  srand(static_cast<unsigned int>(st_test_get_monotonic_time()));

  /* Only generate random IP if user didn't specify one */
  if (!ctx->user_p_sip) {
    p_ip[0] = 197;
    p_ip[1] = rand() % 0xFF;
    p_ip[2] = rand() % 0xFF;
    p_ip[3] = 1;
  }

  /* Give each port after the primary one address in the primary subnet. The
     host octet counts up from the primary one, and stays in 1..254 so no port
     takes the network or the broadcast address. It must come from the primary
     host octet, not from p_ip[0]: with a --p_sip the first octet is the user's
     network, so p_ip[0] + i collides with the primary address as soon as the
     two happen to be equal, and mt_user_params_check rejects the run with
     "same ip ... for port 1 and 0". */
  for (int i = MTL_PORT_R; i < p->num_ports && i < MTL_PORT_MAX && p->port[i][0] != '\0';
       i++) {
    p->sip_addr[i][0] = p_ip[0];
    p->sip_addr[i][1] = p_ip[1];
    p->sip_addr[i][2] = p_ip[2];
    p->sip_addr[i][3] = (uint8_t)((p_ip[3] - 1 + i) % 254 + 1);
  }

  uint8_t* p_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_P];
  uint8_t* r_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_R];

  p_ip_multicast[0] = 239;
  p_ip_multicast[1] = p->sip_addr[MTL_PORT_P][1];
  p_ip_multicast[2] = p->sip_addr[MTL_PORT_P][2];
  p_ip_multicast[3] = p->sip_addr[MTL_PORT_P][3];
  const uint8_t base = p_ip_multicast[3];
  r_ip_multicast[0] = p_ip_multicast[0];
  r_ip_multicast[1] = p_ip_multicast[1];
  r_ip_multicast[2] = p_ip_multicast[2];
  r_ip_multicast[3] = static_cast<uint8_t>(((base + 253 + 1) % 254) + 1);

  if (ctx->mcast_only) {
    r_ip_multicast = ctx->mcast_ip_addr[MTL_PORT_2];
    r_ip_multicast[0] = p_ip_multicast[0];
    r_ip_multicast[1] = p_ip_multicast[1];
    r_ip_multicast[2] = p_ip_multicast[2];
    r_ip_multicast[3] = static_cast<uint8_t>(((base + 253 + 2) % 254) + 1);
  }
}

static uint64_t test_ptp_from_real_time(void* priv) {
  auto* ctx = static_cast<struct st_tests_context*>(priv);
  const uint64_t now = st_test_get_realtime_ns();
  __atomic_store_n(&ctx->ptp_time, now, __ATOMIC_RELEASE);
  return now;
}

static void test_ctx_init(struct st_tests_context* ctx) {
  struct mtl_init_params* p = &ctx->para;

  memset(p, 0x0, sizeof(*p));
  p->flags = MTL_FLAG_BIND_NUMA; /* default bind to numa */
  p->flags |= MTL_FLAG_RANDOM_SRC_PORT;
  p->flags |= MTL_FLAG_CNI_TASKLET; /* for rtcp test */
  p->log_level = MTL_LOG_LEVEL_ERR;
  p->priv = ctx;
  p->ptp_get_time_fn = test_ptp_from_real_time;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    p->tx_queues_cnt[i] = 16;
    p->rx_queues_cnt[i] = 16;
  }

  /* by default don't limit cores */
  p->lcores = NULL;
}

static void test_ctx_uinit(struct st_tests_context* ctx) {
  if (ctx->handle) {
    mtl_uninit(ctx->handle);
    ctx->handle = NULL;
  }
  st_test_free(ctx);
}

static void print_usage(const char* prog) {
  printf(
      "\nMTL Integration Test Options (%s):\n"
      "  --p_port <bdf|kernel:lo>   Primary port (default TX / port 0)\n"
      "  --r_port <bdf|kernel:lo>   Secondary port (default RX / port 1)\n"
      "  --port_list <p0,p1,...>    Comma-separated port list\n"
      "  --p_sip <ip>               Primary port source IPv4 address\n"
      "  --auto_start_stop          Enable MTL_FLAG_DEV_AUTO_START_STOP\n"
      "  --dma_auto                 Auto-probe DMA devices per port NUMA node\n"
      "  --log_level <level>        debug|info|notice|warning|error\n"
      "  --help                     Show GoogleTest and KahawaiTest options\n"
      "\nMode selection is automatic: shared suites share one mtl_init(); NoCtxTest\n"
      "cases run in their own clean process (multi-case and mixed filters dispatch\n"
      "automatically).\n",
      prog ? prog : "KahawaiTest");
}

GTEST_API_ int main(int argc, char** argv) {
  std::vector<std::string> orig_argv;
  orig_argv.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    orig_argv.emplace_back(argv[i]);
  }

  std::vector<std::string> norm_args;
  norm_args.reserve(static_cast<size_t>(argc));
  if (argc > 0) {
    norm_args.emplace_back(argv[0]);
  }
  for (int i = 1; i < argc; ++i) {
    if ((strcmp(argv[i], "--gtest_filter") == 0 ||
         strcmp(argv[i], "--gtest_output") == 0) &&
        i + 1 < argc && argv[i + 1][0] != '-') {
      norm_args.emplace_back(std::string(argv[i]) + "=" + argv[i + 1]);
      ++i;
    } else {
      norm_args.emplace_back(argv[i]);
    }
  }
  std::vector<char*> norm_argv;
  norm_argv.reserve(norm_args.size() + 1);
  for (auto& arg : norm_args) {
    norm_argv.push_back(arg.data());
  }
  norm_argv.push_back(nullptr);
  int norm_argc = static_cast<int>(norm_args.size());

  bool help_requested = false;
  for (int i = 1; i < norm_argc; ++i) {
    if (strcmp(norm_argv[i], "--help") == 0 || strcmp(norm_argv[i], "-h") == 0 ||
        strcmp(norm_argv[i], "-?") == 0 || strcmp(norm_argv[i], "--gtest_help") == 0) {
      help_requested = true;
    }
  }

  testing::InitGoogleTest(&norm_argc, norm_argv.data());
  if (help_requested) {
    print_usage(norm_argc > 0 ? norm_argv[0] : "KahawaiTest");
    return 0;
  }
  if (testing::GTEST_FLAG(list_tests)) {
    return RUN_ALL_TESTS();
  }

  auto* ctx = static_cast<struct st_tests_context*>(
      st_test_zmalloc(sizeof(struct st_tests_context)));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return 1;
  }

  test_ctx_init(ctx);
  test_parse_args(ctx, &ctx->para, norm_argc, norm_argv.data());
  test_random_ip(ctx);
  g_test_ctx = ctx;

  const int ret = mtl_test::run_unified_tests(ctx, orig_argv);
  g_test_ctx = nullptr;
  test_ctx_uinit(ctx);
  return ret;
}

int tx_next_frame(void* priv, uint16_t* next_frame_idx) {
  auto ctx = (tests_context*)priv;

  if (!__atomic_load_n(&ctx->handle, __ATOMIC_ACQUIRE)) return -EIO; /* not ready */

  *next_frame_idx = ctx->fb_idx;
  dbg("%s, next_frame_idx %d\n", __func__, *next_frame_idx);
  ctx->fb_idx++;
  if (ctx->fb_idx >= ctx->fb_cnt) ctx->fb_idx = 0;
  __atomic_fetch_add(&ctx->fb_send, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

void sha_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  unsigned char result[SHA256_DIGEST_LENGTH];
  while (true) {
    void* frame = nullptr;
    {
      std::unique_lock<std::mutex> lck(ctx->mtx);
      ctx->cv.wait(lck, [ctx]() { return !ctx->buf_q.empty() || ctx->stop.load(); });
      if (ctx->buf_q.empty()) {
        if (ctx->stop.load()) break;
        continue;
      }
      frame = ctx->buf_q.front();
      ctx->buf_q.pop();
    }
    dbg("%s, frame %p\n", __func__, frame);
    int i;
    SHA256((unsigned char*)frame, ctx->frame_size, result);
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = ctx->shas[i];
      if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("rx_error_sha", result);
      __atomic_fetch_add(&ctx->sha_fail_cnt, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&ctx->check_sha_frame_cnt, 1, __ATOMIC_RELAXED);
    st_test_free(frame);
  }
}

int tests_context_unit(tests_context* ctx) {
  if (!ctx) return 0;
  for (int frame = 0; frame < TEST_MAX_SHA_HIST_NUM; frame++) {
    if (ctx->frame_buf[frame]) st_test_free(ctx->frame_buf[frame]);
    ctx->frame_buf[frame] = NULL;
  }
  while (!ctx->buf_q.empty()) {
    st_test_free(ctx->buf_q.front());
    ctx->buf_q.pop();
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
    if (ctx->ctx && ctx->ctx->handle) {
      mtl_dma_mem_free(ctx->ctx->handle, ctx->dma_mem);
    }
    ctx->dma_mem = NULL;
  }

  return 0;
}

int test_ctx_notify_event(void* priv, enum st_event event, void* args) {
  (void)args;
  if (event == ST_EVENT_VSYNC) {
    tests_context* s = (tests_context*)priv;
    __atomic_fetch_add(&s->vsync_cnt, 1, __ATOMIC_RELAXED);
#ifdef DEBUG
    struct st10_vsync_meta* meta = (struct st10_vsync_meta*)args;
    dbg("%s(%d,%p), epoch %" PRIu64 " vsync_cnt %d\n", __func__, s->idx, s, meta->epoch,
        s->vsync_cnt);
#endif
  }
  return 0;
}
