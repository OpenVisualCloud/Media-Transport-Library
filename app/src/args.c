/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <getopt.h>
#include <inttypes.h>

#include "app_base.h"
#define DEBUG
#include "log.h"

enum st_args_cmd {
  ST_ARG_UNKNOWN = 0,

  ST_ARG_P_PORT = 0x100, /* start from end of ascii */
  ST_ARG_R_PORT,
  ST_ARG_P_TX_IP,
  ST_ARG_R_TX_IP,
  ST_ARG_P_RX_IP,
  ST_ARG_R_RX_IP,
  ST_ARG_P_SIP,
  ST_ARG_R_SIP,
  ST_ARG_P_NETMASK,
  ST_ARG_R_NETMASK,
  ST_ARG_P_GATEWAY,
  ST_ARG_R_GATEWAY,

  ST_ARG_TX_VIDEO_URL = 0x200,
  ST_ARG_TX_VIDEO_SESSIONS_CNT,
  ST_ARG_TX_VIDEO_RTP_RING_SIZE,
  ST_ARG_TX_AUDIO_URL,
  ST_ARG_TX_AUDIO_SESSIONS_CNT,
  ST_ARG_TX_AUDIO_RTP_RING_SIZE,
  ST_ARG_TX_ANC_URL,
  ST_ARG_TX_ANC_SESSIONS_CNT,
  ST_ARG_TX_ANC_RTP_RING_SIZE,
  ST22_ARG_TX_SESSIONS_CNT,
  ST22_ARG_TX_URL,
  ST_ARG_RX_VIDEO_SESSIONS_CNT,
  ST_ARG_RX_VIDEO_FLIE_FRAMES,
  ST_ARG_RX_VIDEO_FB_CNT,
  ST_ARG_RX_VIDEO_RTP_RING_SIZE,
  ST_ARG_RX_AUDIO_SESSIONS_CNT,
  ST_ARG_RX_AUDIO_RTP_RING_SIZE,
  ST_ARG_RX_ANC_SESSIONS_CNT,
  ST22_ARG_RX_SESSIONS_CNT,
  ST_ARG_HDR_SPLIT,
  ST_ARG_PACING_WAY,

  ST_ARG_CONFIG_FILE = 0x300,
  ST_ARG_TEST_TIME,
  ST_ARG_PTP_UNICAST_ADDR,
  ST_ARG_CNI_THREAD,
  ST_ARG_RX_EBU,
  ST_ARG_USER_LCORES,
  ST_ARG_SCH_DATA_QUOTA,
  ST_ARG_SCH_SESSION_QUOTA,
  ST_ARG_P_TX_DST_MAC,
  ST_ARG_R_TX_DST_MAC,
  ST_ARG_NIC_RX_PROMISCUOUS,
  ST_ARG_LIB_PTP,
  ST_ARG_RX_MONO_POOL,
  ST_ARG_TX_MONO_POOL,
  ST_ARG_MONO_POOL,
  ST_ARG_RX_POOL_DATA_SIZE,
  ST_ARG_LOG_LEVEL,
  ST_ARG_NB_TX_DESC,
  ST_ARG_NB_RX_DESC,
  ST_ARG_DMA_DEV,
  ST_ARG_RX_SEPARATE_VIDEO_LCORE,
  ST_ARG_RX_MIX_VIDEO_LCORE,
  ST_ARG_TSC_PACING,
  ST_ARG_PCAPNG_DUMP,
  ST_ARG_RUNTIME_SESSION,
  ST_ARG_TTF_FILE,
  ST_ARG_AF_XDP_ZC_DISABLE,
  ST_ARG_START_QUEUE,
  ST_ARG_P_START_QUEUE,
  ST_ARG_R_START_QUEUE,
  ST_ARG_TASKLET_TIME,
  ST_ARG_UTC_OFFSET,
  ST_ARG_NO_SYSTEM_RX_QUEUES,
  ST_ARG_TX_COPY_ONCE,
  ST_ARG_TASKLET_THREAD,
  ST_ARG_TASKLET_SLEEP,
  ST_ARG_TASKLET_SLEEP_US,
  ST_ARG_APP_THREAD,
  ST_ARG_RXTX_SIMD_512,
  ST_ARG_PTP_PI,
  ST_ARG_PTP_KP,
  ST_ARG_PTP_KI,
  ST_ARG_PTP_TSC,
  ST_ARG_RSS_MODE,
  ST_ARG_MAX,
};

/*
struct option {
   const char *name;
   int has_arg;
   int *flag;
   int val;
};
*/
static struct option st_app_args_options[] = {
    {"p_port", required_argument, 0, ST_ARG_P_PORT},
    {"r_port", required_argument, 0, ST_ARG_R_PORT},
    {"p_tx_ip", required_argument, 0, ST_ARG_P_TX_IP},
    {"r_tx_ip", required_argument, 0, ST_ARG_R_TX_IP},
    {"p_rx_ip", required_argument, 0, ST_ARG_P_RX_IP},
    {"r_rx_ip", required_argument, 0, ST_ARG_R_RX_IP},
    {"p_sip", required_argument, 0, ST_ARG_P_SIP},
    {"r_sip", required_argument, 0, ST_ARG_R_SIP},
    {"p_netmask", required_argument, 0, ST_ARG_P_NETMASK},
    {"r_netmask", required_argument, 0, ST_ARG_R_NETMASK},
    {"p_gateway", required_argument, 0, ST_ARG_P_GATEWAY},
    {"r_gateway", required_argument, 0, ST_ARG_R_GATEWAY},

    {"tx_video_url", required_argument, 0, ST_ARG_TX_VIDEO_URL},
    {"tx_video_sessions_count", required_argument, 0, ST_ARG_TX_VIDEO_SESSIONS_CNT},
    {"tx_video_rtp_ring_size", required_argument, 0, ST_ARG_TX_VIDEO_RTP_RING_SIZE},
    {"tx_audio_url", required_argument, 0, ST_ARG_TX_AUDIO_URL},
    {"tx_audio_sessions_count", required_argument, 0, ST_ARG_TX_AUDIO_SESSIONS_CNT},
    {"tx_audio_rtp_ring_size", required_argument, 0, ST_ARG_TX_AUDIO_RTP_RING_SIZE},
    {"tx_anc_url", required_argument, 0, ST_ARG_TX_ANC_URL},
    {"tx_anc_sessions_count", required_argument, 0, ST_ARG_TX_ANC_SESSIONS_CNT},
    {"tx_anc_rtp_ring_size", required_argument, 0, ST_ARG_TX_ANC_RTP_RING_SIZE},
    {"tx_st22_sessions_count", required_argument, 0, ST22_ARG_TX_SESSIONS_CNT},
    {"tx_st22_url", required_argument, 0, ST22_ARG_TX_URL},

    {"rx_video_sessions_count", required_argument, 0, ST_ARG_RX_VIDEO_SESSIONS_CNT},
    {"rx_video_file_frames", required_argument, 0, ST_ARG_RX_VIDEO_FLIE_FRAMES},
    {"rx_video_fb_cnt", required_argument, 0, ST_ARG_RX_VIDEO_FB_CNT},
    {"rx_video_rtp_ring_size", required_argument, 0, ST_ARG_RX_VIDEO_RTP_RING_SIZE},
    {"rx_audio_sessions_count", required_argument, 0, ST_ARG_RX_AUDIO_SESSIONS_CNT},
    {"rx_audio_rtp_ring_size", required_argument, 0, ST_ARG_RX_AUDIO_RTP_RING_SIZE},
    {"rx_anc_sessions_count", required_argument, 0, ST_ARG_RX_ANC_SESSIONS_CNT},
    {"rx_st22_sessions_count", required_argument, 0, ST22_ARG_RX_SESSIONS_CNT},
    {"hdr_split", no_argument, 0, ST_ARG_HDR_SPLIT},
    {"pacing_way", required_argument, 0, ST_ARG_PACING_WAY},

    {"config_file", required_argument, 0, ST_ARG_CONFIG_FILE},
    {"test_time", required_argument, 0, ST_ARG_TEST_TIME},
    {"ptp_unicast", no_argument, 0, ST_ARG_PTP_UNICAST_ADDR},
    {"cni_thread", no_argument, 0, ST_ARG_CNI_THREAD},
    {"ebu", no_argument, 0, ST_ARG_RX_EBU},
    {"lcores", required_argument, 0, ST_ARG_USER_LCORES},
    {"sch_data_quota", required_argument, 0, ST_ARG_SCH_DATA_QUOTA},
    {"sch_session_quota", required_argument, 0, ST_ARG_SCH_SESSION_QUOTA},
    {"p_tx_dst_mac", required_argument, 0, ST_ARG_P_TX_DST_MAC},
    {"r_tx_dst_mac", required_argument, 0, ST_ARG_R_TX_DST_MAC},
    {"promiscuous", no_argument, 0, ST_ARG_NIC_RX_PROMISCUOUS},
    {"log_level", required_argument, 0, ST_ARG_LOG_LEVEL},
    {"ptp", no_argument, 0, ST_ARG_LIB_PTP},
    {"rx_mono_pool", no_argument, 0, ST_ARG_RX_MONO_POOL},
    {"tx_mono_pool", no_argument, 0, ST_ARG_TX_MONO_POOL},
    {"mono_pool", no_argument, 0, ST_ARG_MONO_POOL},
    {"rx_pool_data_size", required_argument, 0, ST_ARG_RX_POOL_DATA_SIZE},
    {"rx_separate_lcore", no_argument, 0, ST_ARG_RX_SEPARATE_VIDEO_LCORE},
    {"rx_mix_lcore", no_argument, 0, ST_ARG_RX_MIX_VIDEO_LCORE},
    {"nb_tx_desc", required_argument, 0, ST_ARG_NB_TX_DESC},
    {"nb_rx_desc", required_argument, 0, ST_ARG_NB_RX_DESC},
    {"dma_dev", required_argument, 0, ST_ARG_DMA_DEV},
    {"tsc", no_argument, 0, ST_ARG_TSC_PACING},
    {"pcapng_dump", required_argument, 0, ST_ARG_PCAPNG_DUMP},
    {"runtime_session", no_argument, 0, ST_ARG_RUNTIME_SESSION},
    {"ttf_file", required_argument, 0, ST_ARG_TTF_FILE},
    {"afxdp_zc_disable", no_argument, 0, ST_ARG_AF_XDP_ZC_DISABLE},
    {"start_queue", required_argument, 0, ST_ARG_START_QUEUE},
    {"p_start_queue", required_argument, 0, ST_ARG_P_START_QUEUE},
    {"r_start_queue", required_argument, 0, ST_ARG_R_START_QUEUE},
    {"tasklet_time", no_argument, 0, ST_ARG_TASKLET_TIME},
    {"utc_offset", required_argument, 0, ST_ARG_UTC_OFFSET},
    {"no_srq", no_argument, 0, ST_ARG_NO_SYSTEM_RX_QUEUES},
    {"tx_copy_once", no_argument, 0, ST_ARG_TX_COPY_ONCE},
    {"tasklet_thread", no_argument, 0, ST_ARG_TASKLET_THREAD},
    {"tasklet_sleep", no_argument, 0, ST_ARG_TASKLET_SLEEP},
    {"tasklet_sleep_us", required_argument, 0, ST_ARG_TASKLET_SLEEP_US},
    {"app_thread", no_argument, 0, ST_ARG_APP_THREAD},
    {"rxtx_simd_512", no_argument, 0, ST_ARG_RXTX_SIMD_512},
    {"pi", no_argument, 0, ST_ARG_PTP_PI},
    {"kp", required_argument, 0, ST_ARG_PTP_KP},
    {"ki", required_argument, 0, ST_ARG_PTP_KI},
    {"ptp_tsc", no_argument, 0, ST_ARG_PTP_TSC},
    {"rss_mode", required_argument, 0, ST_ARG_RSS_MODE},

    {0, 0, 0, 0}};

static int app_args_parse_lcores(struct mtl_init_params* p, char* list) {
  if (!list) return -EIO;

  dbg("%s, lcore list %s\n", __func__, list);
  p->lcores = list;
  return 0;
}

static int app_args_parse_tx_mac(struct st_app_context* ctx, char* mac_str,
                                 enum mtl_port port) {
  int ret;
  uint8_t* mac;

  if (!mac_str) return -EIO;
  dbg("%s, tx dst mac %s\n", __func__, mac_str);

  mac = &ctx->tx_dst_mac[port][0];
  ret = sscanf(mac_str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx", &mac[0], &mac[1],
               &mac[2], &mac[3], &mac[4], &mac[5]);
  if (ret < 0) return ret;

  ctx->has_tx_dst_mac[port] = true;
  return 0;
}

static int app_args_dma_dev(struct mtl_init_params* p, char* in_dev) {
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

static int app_args_json(struct st_app_context* ctx, struct mtl_init_params* p,
                         char* json_file) {
  ctx->json_ctx = st_app_zmalloc(sizeof(st_json_context_t));
  if (!ctx->json_ctx) {
    err("%s, json_ctx alloc fail\n", __func__);
    return -ENOMEM;
  }
  int ret = st_app_parse_json(ctx->json_ctx, json_file);
  if (ret < 0) {
    err("%s, st_app_parse_json fail %d\n", __func__, ret);
    st_app_free(ctx->json_ctx);
    ctx->json_ctx = NULL;
    return ret;
  }
  ctx->tx_video_session_cnt = ctx->json_ctx->tx_video_session_cnt;
  ctx->tx_audio_session_cnt = ctx->json_ctx->tx_audio_session_cnt;
  ctx->tx_anc_session_cnt = ctx->json_ctx->tx_anc_session_cnt;
  ctx->tx_st22p_session_cnt = ctx->json_ctx->tx_st22p_session_cnt;
  ctx->tx_st20p_session_cnt = ctx->json_ctx->tx_st20p_session_cnt;
  ctx->rx_video_session_cnt = ctx->json_ctx->rx_video_session_cnt;
  ctx->rx_audio_session_cnt = ctx->json_ctx->rx_audio_session_cnt;
  ctx->rx_anc_session_cnt = ctx->json_ctx->rx_anc_session_cnt;
  ctx->rx_st22p_session_cnt = ctx->json_ctx->rx_st22p_session_cnt;
  ctx->rx_st20p_session_cnt = ctx->json_ctx->rx_st20p_session_cnt;
  ctx->rx_st20r_session_cnt = ctx->json_ctx->rx_st20r_session_cnt;
  for (int i = 0; i < ctx->json_ctx->num_interfaces; ++i) {
    snprintf(p->port[i], sizeof(p->port[i]), "%s", ctx->json_ctx->interfaces[i].name);
    memcpy(p->sip_addr[i], ctx->json_ctx->interfaces[i].ip_addr, sizeof(p->sip_addr[i]));
    memcpy(p->netmask[i], ctx->json_ctx->interfaces[i].netmask, sizeof(p->netmask[i]));
    memcpy(p->gateway[i], ctx->json_ctx->interfaces[i].gateway, sizeof(p->gateway[i]));
    p->num_ports++;
  }
  if (ctx->json_ctx->sch_quota) {
    p->data_quota_mbs_per_sch =
        ctx->json_ctx->sch_quota * st20_1080p59_yuv422_10bit_bandwidth_mps();
  }

  return 0;
}

int st_app_parse_args(struct st_app_context* ctx, struct mtl_init_params* p, int argc,
                      char** argv) {
  int cmd = -1, optIdx = 0;
  int nb;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", st_app_args_options, &optIdx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case ST_ARG_P_PORT:
        snprintf(p->port[MTL_PORT_P], sizeof(p->port[MTL_PORT_P]), "%s", optarg);
        p->num_ports++;
        break;
      case ST_ARG_R_PORT:
        snprintf(p->port[MTL_PORT_R], sizeof(p->port[MTL_PORT_R]), "%s", optarg);
        p->num_ports++;
        break;
      case ST_ARG_P_SIP:
        inet_pton(AF_INET, optarg, mtl_p_sip_addr(p));
        break;
      case ST_ARG_R_SIP:
        inet_pton(AF_INET, optarg, mtl_r_sip_addr(p));
        break;
      case ST_ARG_P_TX_IP:
        inet_pton(AF_INET, optarg, ctx->tx_dip_addr[MTL_PORT_P]);
        break;
      case ST_ARG_R_TX_IP:
        inet_pton(AF_INET, optarg, ctx->tx_dip_addr[MTL_PORT_R]);
        break;
      case ST_ARG_P_RX_IP:
        inet_pton(AF_INET, optarg, ctx->rx_sip_addr[MTL_PORT_P]);
        break;
      case ST_ARG_R_RX_IP:
        inet_pton(AF_INET, optarg, ctx->rx_sip_addr[MTL_PORT_R]);
        break;
      case ST_ARG_P_NETMASK:
        inet_pton(AF_INET, optarg, p->netmask[MTL_PORT_P]);
        break;
      case ST_ARG_R_NETMASK:
        inet_pton(AF_INET, optarg, p->netmask[MTL_PORT_R]);
        break;
      case ST_ARG_P_GATEWAY:
        inet_pton(AF_INET, optarg, p->gateway[MTL_PORT_P]);
        break;
      case ST_ARG_R_GATEWAY:
        inet_pton(AF_INET, optarg, p->gateway[MTL_PORT_R]);
        break;
      case ST_ARG_TX_VIDEO_URL:
        snprintf(ctx->tx_video_url, sizeof(ctx->tx_video_url), "%s", optarg);
        break;
      case ST_ARG_TX_VIDEO_RTP_RING_SIZE:
        ctx->tx_video_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_TX_VIDEO_SESSIONS_CNT:
        ctx->tx_video_session_cnt = atoi(optarg);
        break;
      case ST_ARG_TX_AUDIO_URL:
        snprintf(ctx->tx_audio_url, sizeof(ctx->tx_audio_url), "%s", optarg);
        break;
      case ST_ARG_TX_AUDIO_SESSIONS_CNT:
        ctx->tx_audio_session_cnt = atoi(optarg);
        break;
      case ST_ARG_TX_AUDIO_RTP_RING_SIZE:
        ctx->tx_audio_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_TX_ANC_URL:
        snprintf(ctx->tx_anc_url, sizeof(ctx->tx_anc_url), "%s", optarg);
        break;
      case ST_ARG_TX_ANC_RTP_RING_SIZE:
        ctx->tx_anc_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_TX_ANC_SESSIONS_CNT:
        ctx->tx_anc_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_SESSIONS_CNT:
        ctx->rx_video_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_FLIE_FRAMES:
        ctx->rx_video_file_frames = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_FB_CNT:
        ctx->rx_video_fb_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_RTP_RING_SIZE:
        ctx->rx_video_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_RX_AUDIO_SESSIONS_CNT:
        ctx->rx_audio_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_AUDIO_RTP_RING_SIZE:
        ctx->rx_audio_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_RX_ANC_SESSIONS_CNT:
        ctx->rx_anc_session_cnt = atoi(optarg);
        break;
      case ST22_ARG_TX_SESSIONS_CNT:
        ctx->tx_st22_session_cnt = atoi(optarg);
        break;
      case ST22_ARG_TX_URL:
        snprintf(ctx->tx_st22_url, sizeof(ctx->tx_st22_url), "%s", optarg);
        break;
      case ST22_ARG_RX_SESSIONS_CNT:
        ctx->rx_st22_session_cnt = atoi(optarg);
        break;
      case ST_ARG_HDR_SPLIT:
        ctx->enable_hdr_split = true;
        break;
      case ST_ARG_PACING_WAY:
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
      case ST_ARG_CONFIG_FILE:
        app_args_json(ctx, p, optarg);
        break;
      case ST_ARG_PTP_UNICAST_ADDR:
        p->flags |= MTL_FLAG_PTP_UNICAST_ADDR;
        break;
      case ST_ARG_CNI_THREAD:
        p->flags |= MTL_FLAG_CNI_THREAD;
        break;
      case ST_ARG_TEST_TIME:
        ctx->test_time_s = atoi(optarg);
        break;
      case ST_ARG_RX_EBU:
        p->flags |= MTL_FLAG_RX_VIDEO_EBU;
        break;
      case ST_ARG_RX_MONO_POOL:
        p->flags |= MTL_FLAG_RX_MONO_POOL;
        break;
      case ST_ARG_TX_MONO_POOL:
        p->flags |= MTL_FLAG_TX_MONO_POOL;
        break;
      case ST_ARG_MONO_POOL:
        p->flags |= MTL_FLAG_RX_MONO_POOL;
        p->flags |= MTL_FLAG_TX_MONO_POOL;
        break;
      case ST_ARG_RX_POOL_DATA_SIZE:
        p->rx_pool_data_size = atoi(optarg);
        break;
      case ST_ARG_RX_SEPARATE_VIDEO_LCORE:
        p->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
        break;
      case ST_ARG_RX_MIX_VIDEO_LCORE:
        p->flags &= ~MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
        break;
      case ST_ARG_TSC_PACING:
        p->pacing = ST21_TX_PACING_WAY_TSC;
        break;
      case ST_ARG_USER_LCORES:
        app_args_parse_lcores(p, optarg);
        break;
      case ST_ARG_SCH_DATA_QUOTA:
        p->data_quota_mbs_per_sch = atoi(optarg);
        break;
      case ST_ARG_SCH_SESSION_QUOTA: /* unit: 1080p tx */
        nb = atoi(optarg);
        if (nb > 0 && nb < 100) {
          p->data_quota_mbs_per_sch = nb * st20_1080p59_yuv422_10bit_bandwidth_mps();
        }
        break;
      case ST_ARG_P_TX_DST_MAC:
        app_args_parse_tx_mac(ctx, optarg, MTL_PORT_P);
        break;
      case ST_ARG_R_TX_DST_MAC:
        app_args_parse_tx_mac(ctx, optarg, MTL_PORT_R);
        break;
      case ST_ARG_NIC_RX_PROMISCUOUS:
        p->flags |= MTL_FLAG_NIC_RX_PROMISCUOUS;
        break;
      case ST_ARG_LIB_PTP:
        p->flags |= MTL_FLAG_PTP_ENABLE;
        p->ptp_get_time_fn = NULL; /* clear the user ptp func */
        break;
      case ST_ARG_LOG_LEVEL:
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
        app_set_log_level(p->log_level);
        break;
      case ST_ARG_NB_TX_DESC:
        p->nb_tx_desc = atoi(optarg);
        break;
      case ST_ARG_NB_RX_DESC:
        p->nb_rx_desc = atoi(optarg);
        break;
      case ST_ARG_DMA_DEV:
        app_args_dma_dev(p, optarg);
        break;
      case ST_ARG_PCAPNG_DUMP:
        ctx->pcapng_max_pkts = atoi(optarg);
        break;
      case ST_ARG_RUNTIME_SESSION:
        ctx->runtime_session = true;
        break;
      case ST_ARG_TTF_FILE:
        snprintf(ctx->ttf_file, sizeof(ctx->ttf_file), "%s", optarg);
        break;
      case ST_ARG_AF_XDP_ZC_DISABLE:
        p->flags |= MTL_FLAG_AF_XDP_ZC_DISABLE;
        break;
      case ST_ARG_START_QUEUE:
        p->xdp_info[MTL_PORT_P].start_queue = atoi(optarg);
        p->xdp_info[MTL_PORT_R].start_queue = atoi(optarg);
        break;
      case ST_ARG_P_START_QUEUE:
        p->xdp_info[MTL_PORT_P].start_queue = atoi(optarg);
        break;
      case ST_ARG_R_START_QUEUE:
        p->xdp_info[MTL_PORT_R].start_queue = atoi(optarg);
        break;
      case ST_ARG_TASKLET_TIME:
        p->flags |= MTL_FLAG_TASKLET_TIME_MEASURE;
        break;
      case ST_ARG_UTC_OFFSET:
        ctx->utc_offset = atoi(optarg);
        break;
      case ST_ARG_NO_SYSTEM_RX_QUEUES:
        p->flags |= MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES;
        break;
      case ST_ARG_TX_COPY_ONCE:
        ctx->tx_copy_once = true;
        break;
      case ST_ARG_TASKLET_SLEEP:
        p->flags |= MTL_FLAG_TASKLET_SLEEP;
        break;
      case ST_ARG_TASKLET_SLEEP_US:
        ctx->var_para.sch_force_sleep_us = atoi(optarg);
        break;
      case ST_ARG_TASKLET_THREAD:
        p->flags |= MTL_FLAG_TASKLET_THREAD;
        break;
      case ST_ARG_APP_THREAD:
        ctx->app_thread = true;
        break;
      case ST_ARG_RXTX_SIMD_512:
        p->flags |= MTL_FLAG_RXTX_SIMD_512;
        break;
      case ST_ARG_PTP_PI:
        p->flags |= MTL_FLAG_PTP_PI;
        break;
      case ST_ARG_PTP_KP:
        p->kp = strtod(optarg, NULL);
        break;
      case ST_ARG_PTP_KI:
        p->ki = strtod(optarg, NULL);
        break;
      case ST_ARG_PTP_TSC:
        p->flags |= MTL_FLAG_PTP_SOURCE_TSC;
        break;
      case ST_ARG_RSS_MODE:
        if (!strcmp(optarg, "l3"))
          p->rss_mode = MT_RSS_MODE_L3;
        else if (!strcmp(optarg, "l4"))
          p->rss_mode = MT_RSS_MODE_L4;
        else
          err("%s, unknow rss mode %s\n", __func__, optarg);
        break;
      case '?':
        break;
      default:
        break;
    }
  };

  return 0;
}
