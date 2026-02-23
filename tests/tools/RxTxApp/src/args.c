/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>

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
  ST_ARG_P_RX_MCAST_SIP,
  ST_ARG_R_RX_MCAST_SIP,

  ST_ARG_TX_VIDEO_URL = 0x200,
  ST_ARG_TX_VIDEO_SESSIONS_CNT,
  ST_ARG_TX_VIDEO_RTP_RING_SIZE,
  ST_ARG_TX_AUDIO_URL,
  ST_ARG_TX_AUDIO_SESSIONS_CNT,
  ST_ARG_TX_AUDIO_RTP_RING_SIZE,
  ST_ARG_TX_ANC_URL,
  ST_ARG_TX_ANC_SESSIONS_CNT,
  ST_ARG_TX_ANC_RTP_RING_SIZE,
  ST_ARG_TX_FMD_URL,
  ST_ARG_TX_FMD_SESSIONS_CNT,
  ST_ARG_TX_FMD_RTP_RING_SIZE,
  ST22_ARG_TX_SESSIONS_CNT,
  ST22_ARG_TX_URL,
  ST_ARG_RX_VIDEO_SESSIONS_CNT,
  ST_ARG_RX_VIDEO_FILE_FRAMES,
  ST_ARG_RX_VIDEO_FB_CNT,
  ST_ARG_RX_VIDEO_RTP_RING_SIZE,
  ST_ARG_RX_VIDEO_MULTI_THREADS,
  ST_ARG_RX_AUDIO_SESSIONS_CNT,
  ST_ARG_RX_AUDIO_RTP_RING_SIZE,
  ST_ARG_RX_AUDIO_DUMP_TIME_S,
  ST_ARG_RX_ANC_SESSIONS_CNT,
  ST_ARG_RX_FMD_SESSIONS_CNT,
  ST22_ARG_RX_SESSIONS_CNT,
  ST_ARG_HDR_SPLIT,
  ST_ARG_PACING_WAY,
  ST_ARG_START_VRX,
  ST_ARG_PAD_INTERVAL,
  ST_ARG_PAD_STATIC,
  ST_ARG_SHAPING,
  ST_ARG_EXACT_USER_PACING,
  ST_ARG_TIMESTAMP_EPOCH,
  ST_ARG_TIMESTAMP_DELTA_US,
  ST_ARG_NO_BULK,
  ST_ARG_TX_DISPLAY,
  ST_ARG_RX_DISPLAY,
  ST_ARG_DISABLE_MIGRATE,
  ST_ARG_BIND_NUMA,
  ST_ARG_NOT_BIND_NUMA,
  ST_ARG_FORCE_NUMA,
  ST_ARG_FORCE_TX_VIDEO_NUMA,
  ST_ARG_FORCE_RX_VIDEO_NUMA,
  ST_ARG_FORCE_TX_AUDIO_NUMA,
  ST_ARG_FORCE_RX_AUDIO_NUMA,

  ST_ARG_CONFIG_FILE = 0x300,
  ST_ARG_TEST_TIME,
  ST_ARG_PTP_UNICAST_ADDR,
  ST_ARG_CNI_THREAD,
  ST_ARG_CNI_TASKLET,
  ST_ARG_RX_TIMING_PARSER_STAT,
  ST_ARG_RX_TIMING_PARSER_META,
  ST_ARG_RX_BURST_SZ,
  ST_ARG_USER_LCORES,
  ST_ARG_SCH_DATA_QUOTA,
  ST_ARG_SCH_SESSION_QUOTA,
  ST_ARG_P_TX_DST_MAC,
  ST_ARG_R_TX_DST_MAC,
  ST_ARG_NIC_RX_PROMISCUOUS,
  ST_ARG_LIB_PTP,
  ST_ARG_LIB_PHC2SYS,
  ST_ARG_LIB_PTP_SYNC_SYS,
  ST_ARG_RX_MONO_POOL,
  ST_ARG_TX_MONO_POOL,
  ST_ARG_MONO_POOL,
  ST_ARG_RX_POOL_DATA_SIZE,
  ST_ARG_LOG_LEVEL,
  ST_ARG_LOG_FILE,
  ST_ARG_LOG_TIME_MS,
  ST_ARG_LOG_PRINTER,
  ST_ARG_NB_TX_DESC,
  ST_ARG_NB_RX_DESC,
  ST_ARG_DMA_DEV,
  ST_ARG_RX_SEPARATE_VIDEO_LCORE,
  ST_ARG_RX_MIX_VIDEO_LCORE,
  ST_ARG_DEDICATE_SYS_LCORE,
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
  ST_ARG_APP_BIND_THREAD,
  ST_ARG_APP_BIND_LCORE,
  ST_ARG_RXTX_SIMD_512,
  ST_ARG_PTP_PI,
  ST_ARG_PTP_KP,
  ST_ARG_PTP_KI,
  ST_ARG_PTP_TSC,
  ST_ARG_RSS_MODE,
  ST_ARG_RANDOM_SRC_PORT,
  ST_ARG_TX_NO_CHAIN,
  ST_ARG_MULTI_SRC_PORT,
  ST_ARG_AUDIO_BUILD_PACING,
  ST_ARG_AUDIO_DEDICATE_QUEUE,
  ST_ARG_AUDIO_TX_PACING,
  ST_ARG_AUDIO_RL_ACCURACY_US,
  ST_ARG_AUDIO_RL_OFFSET_US,
  ST_ARG_AUDIO_FIFO_SIZE,
  ST_ARG_ANC_DEDICATE_QUEUE,
  ST_ARG_FMD_DEDICATE_QUEUE,
  ST_ARG_TX_NO_BURST_CHECK,
  ST_ARG_DHCP,
  ST_ARG_IOVA_MODE,
  ST_ARG_SHARED_TX_QUEUES,
  ST_ARG_SHARED_RX_QUEUES,
  ST_ARG_RX_USE_CNI,
  ST_ARG_RX_UDP_PORT_ONLY,
  ST_ARG_VIRTIO_USER,
  ST_ARG_VIDEO_SHA_CHECK,
  ST_ARG_ARP_TIMEOUT_S,
  ST_ARG_RSS_SCH_NB,
  ST_ARG_ALLOW_ACROSS_NUMA_CORE,
  ST_ARG_NO_MULTICAST,
  ST_ARG_TX_USER_CLOCK_OFFSET,
  ST_ARG_AUTO_STOP,
  ST_ARG_RX_MAX_FILE_SIZE,
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
    {"p_rx_mcast_sip", required_argument, 0, ST_ARG_P_RX_MCAST_SIP},
    {"r_rx_mcast_sip", required_argument, 0, ST_ARG_R_RX_MCAST_SIP},

    {"tx_video_url", required_argument, 0, ST_ARG_TX_VIDEO_URL},
    {"tx_video_sessions_count", required_argument, 0, ST_ARG_TX_VIDEO_SESSIONS_CNT},
    {"tx_video_rtp_ring_size", required_argument, 0, ST_ARG_TX_VIDEO_RTP_RING_SIZE},
    {"tx_audio_url", required_argument, 0, ST_ARG_TX_AUDIO_URL},
    {"tx_audio_sessions_count", required_argument, 0, ST_ARG_TX_AUDIO_SESSIONS_CNT},
    {"tx_audio_rtp_ring_size", required_argument, 0, ST_ARG_TX_AUDIO_RTP_RING_SIZE},
    {"tx_anc_url", required_argument, 0, ST_ARG_TX_ANC_URL},
    {"tx_anc_sessions_count", required_argument, 0, ST_ARG_TX_ANC_SESSIONS_CNT},
    {"tx_anc_rtp_ring_size", required_argument, 0, ST_ARG_TX_ANC_RTP_RING_SIZE},
    {"tx_fmd_url", required_argument, 0, ST_ARG_TX_FMD_URL},
    {"tx_fmd_sessions_count", required_argument, 0, ST_ARG_TX_FMD_SESSIONS_CNT},
    {"tx_fmd_rtp_ring_size", required_argument, 0, ST_ARG_TX_FMD_RTP_RING_SIZE},
    {"tx_st22_sessions_count", required_argument, 0, ST22_ARG_TX_SESSIONS_CNT},
    {"tx_st22_url", required_argument, 0, ST22_ARG_TX_URL},

    {"rx_video_sessions_count", required_argument, 0, ST_ARG_RX_VIDEO_SESSIONS_CNT},
    {"rx_video_file_frames", required_argument, 0, ST_ARG_RX_VIDEO_FILE_FRAMES},
    {"rx_video_fb_cnt", required_argument, 0, ST_ARG_RX_VIDEO_FB_CNT},
    {"rx_video_rtp_ring_size", required_argument, 0, ST_ARG_RX_VIDEO_RTP_RING_SIZE},
    {"rx_video_multi_thread", no_argument, 0, ST_ARG_RX_VIDEO_MULTI_THREADS},
    {"rx_audio_sessions_count", required_argument, 0, ST_ARG_RX_AUDIO_SESSIONS_CNT},
    {"rx_audio_rtp_ring_size", required_argument, 0, ST_ARG_RX_AUDIO_RTP_RING_SIZE},
    {"rx_audio_dump_time_s", required_argument, 0, ST_ARG_RX_AUDIO_DUMP_TIME_S},
    {"rx_anc_sessions_count", required_argument, 0, ST_ARG_RX_ANC_SESSIONS_CNT},
    {"rx_fmd_sessions_count", required_argument, 0, ST_ARG_RX_FMD_SESSIONS_CNT},
    {"rx_st22_sessions_count", required_argument, 0, ST22_ARG_RX_SESSIONS_CNT},
    {"hdr_split", no_argument, 0, ST_ARG_HDR_SPLIT},
    {"pacing_way", required_argument, 0, ST_ARG_PACING_WAY},
    {"start_vrx", required_argument, 0, ST_ARG_START_VRX},
    {"pad_interval", required_argument, 0, ST_ARG_PAD_INTERVAL},
    {"static_pad", no_argument, 0, ST_ARG_PAD_STATIC},
    {"shaping", required_argument, 0, ST_ARG_SHAPING},
    {"exact_pacing", no_argument, 0, ST_ARG_EXACT_USER_PACING},
    {"ts_delta_us", required_argument, 0, ST_ARG_TIMESTAMP_DELTA_US},
    {"no_bulk", no_argument, 0, ST_ARG_NO_BULK},
    {"tx_display", no_argument, 0, ST_ARG_TX_DISPLAY},
    {"rx_display", no_argument, 0, ST_ARG_RX_DISPLAY},
    {"disable_migrate", no_argument, 0, ST_ARG_DISABLE_MIGRATE},
    {"bind_numa", no_argument, 0, ST_ARG_BIND_NUMA},
    {"not_bind_numa", no_argument, 0, ST_ARG_NOT_BIND_NUMA},
    {"force_numa", required_argument, 0, ST_ARG_FORCE_NUMA},
    {"force_tx_video_numa", required_argument, 0, ST_ARG_FORCE_TX_VIDEO_NUMA},
    {"force_rx_video_numa", required_argument, 0, ST_ARG_FORCE_RX_VIDEO_NUMA},
    {"force_tx_audio_numa", required_argument, 0, ST_ARG_FORCE_TX_AUDIO_NUMA},
    {"force_rx_audio_numa", required_argument, 0, ST_ARG_FORCE_RX_AUDIO_NUMA},

    {"config_file", required_argument, 0, ST_ARG_CONFIG_FILE},
    {"test_time", required_argument, 0, ST_ARG_TEST_TIME},
    {"ptp_unicast", no_argument, 0, ST_ARG_PTP_UNICAST_ADDR},
    {"cni_thread", no_argument, 0, ST_ARG_CNI_THREAD},
    {"cni_tasklet", no_argument, 0, ST_ARG_CNI_TASKLET},
    {"rx_timing_parser", no_argument, 0, ST_ARG_RX_TIMING_PARSER_STAT},
    {"rx_timing_parser_meta", no_argument, 0, ST_ARG_RX_TIMING_PARSER_META},
    {"rx_burst_size", required_argument, 0, ST_ARG_RX_BURST_SZ},
    {"lcores", required_argument, 0, ST_ARG_USER_LCORES},
    {"sch_data_quota", required_argument, 0, ST_ARG_SCH_DATA_QUOTA},
    {"sch_session_quota", required_argument, 0, ST_ARG_SCH_SESSION_QUOTA},
    {"p_tx_dst_mac", required_argument, 0, ST_ARG_P_TX_DST_MAC},
    {"r_tx_dst_mac", required_argument, 0, ST_ARG_R_TX_DST_MAC},
    {"promiscuous", no_argument, 0, ST_ARG_NIC_RX_PROMISCUOUS},
    {"log_level", required_argument, 0, ST_ARG_LOG_LEVEL},
    {"log_file", required_argument, 0, ST_ARG_LOG_FILE},
    {"log_time_ms", no_argument, 0, ST_ARG_LOG_TIME_MS},
    {"log_printer", no_argument, 0, ST_ARG_LOG_PRINTER},
    {"ptp", no_argument, 0, ST_ARG_LIB_PTP},
    {"phc2sys", no_argument, 0, ST_ARG_LIB_PHC2SYS},
    {"ptp_sync_sys", no_argument, 0, ST_ARG_LIB_PTP_SYNC_SYS},
    {"rx_mono_pool", no_argument, 0, ST_ARG_RX_MONO_POOL},
    {"tx_mono_pool", no_argument, 0, ST_ARG_TX_MONO_POOL},
    {"mono_pool", no_argument, 0, ST_ARG_MONO_POOL},
    {"rx_pool_data_size", required_argument, 0, ST_ARG_RX_POOL_DATA_SIZE},
    {"rx_separate_lcore", no_argument, 0, ST_ARG_RX_SEPARATE_VIDEO_LCORE},
    {"rx_mix_lcore", no_argument, 0, ST_ARG_RX_MIX_VIDEO_LCORE},
    {"dedicated_sys_lcore", no_argument, 0, ST_ARG_DEDICATE_SYS_LCORE},
    {"nb_tx_desc", required_argument, 0, ST_ARG_NB_TX_DESC},
    {"nb_rx_desc", required_argument, 0, ST_ARG_NB_RX_DESC},
    {"dma_dev", required_argument, 0, ST_ARG_DMA_DEV},
    {"tsc", no_argument, 0, ST_ARG_TSC_PACING},
    {"pcapng_dump", required_argument, 0, ST_ARG_PCAPNG_DUMP},
    {"runtime_session", no_argument, 0, ST_ARG_RUNTIME_SESSION},
    {"ttf_file", required_argument, 0, ST_ARG_TTF_FILE},
    {"afxdp_zc_disable", no_argument, 0, ST_ARG_AF_XDP_ZC_DISABLE},
    {"tasklet_time", no_argument, 0, ST_ARG_TASKLET_TIME},
    {"utc_offset", required_argument, 0, ST_ARG_UTC_OFFSET},
    {"no_srq", no_argument, 0, ST_ARG_NO_SYSTEM_RX_QUEUES},
    {"tx_copy_once", no_argument, 0, ST_ARG_TX_COPY_ONCE},
    {"tasklet_thread", no_argument, 0, ST_ARG_TASKLET_THREAD},
    {"tasklet_sleep", no_argument, 0, ST_ARG_TASKLET_SLEEP},
    {"tasklet_sleep_us", required_argument, 0, ST_ARG_TASKLET_SLEEP_US},
    {"app_bind_thread", no_argument, 0, ST_ARG_APP_BIND_THREAD},
    {"app_bind_lcore", no_argument, 0, ST_ARG_APP_BIND_LCORE},
    {"rxtx_simd_512", no_argument, 0, ST_ARG_RXTX_SIMD_512},
    {"pi", no_argument, 0, ST_ARG_PTP_PI},
    {"kp", required_argument, 0, ST_ARG_PTP_KP},
    {"ki", required_argument, 0, ST_ARG_PTP_KI},
    {"ptp_tsc", no_argument, 0, ST_ARG_PTP_TSC},
    {"rss_mode", required_argument, 0, ST_ARG_RSS_MODE},
    {"random_src_port", no_argument, 0, ST_ARG_RANDOM_SRC_PORT},
    {"tx_no_chain", no_argument, 0, ST_ARG_TX_NO_CHAIN},
    {"multi_src_port", no_argument, 0, ST_ARG_MULTI_SRC_PORT},
    {"audio_build_pacing", no_argument, 0, ST_ARG_AUDIO_BUILD_PACING},
    {"audio_dedicate_queue", no_argument, 0, ST_ARG_AUDIO_DEDICATE_QUEUE},
    {"audio_tx_pacing", required_argument, 0, ST_ARG_AUDIO_TX_PACING},
    {"audio_rl_accuracy", required_argument, 0, ST_ARG_AUDIO_RL_ACCURACY_US},
    {"audio_rl_offset", required_argument, 0, ST_ARG_AUDIO_RL_OFFSET_US},
    {"audio_fifo_size", required_argument, 0, ST_ARG_AUDIO_FIFO_SIZE},
    {"anc_dedicate_queue", no_argument, 0, ST_ARG_ANC_DEDICATE_QUEUE},
    {"fmd_dedicate_queue", no_argument, 0, ST_ARG_FMD_DEDICATE_QUEUE},
    {"tx_no_burst_check", no_argument, 0, ST_ARG_TX_NO_BURST_CHECK},
    {"dhcp", no_argument, 0, ST_ARG_DHCP},
    {"iova_mode", required_argument, 0, ST_ARG_IOVA_MODE},
    {"shared_tx_queues", no_argument, 0, ST_ARG_SHARED_TX_QUEUES},
    {"shared_rx_queues", no_argument, 0, ST_ARG_SHARED_RX_QUEUES},
    {"rx_use_cni", no_argument, 0, ST_ARG_RX_USE_CNI},
    {"rx_udp_port_only", no_argument, 0, ST_ARG_RX_UDP_PORT_ONLY},
    {"virtio_user", no_argument, 0, ST_ARG_VIRTIO_USER},
    {"video_sha_check", no_argument, 0, ST_ARG_VIDEO_SHA_CHECK},
    {"arp_timeout_s", required_argument, 0, ST_ARG_ARP_TIMEOUT_S},
    {"rss_sch_nb", required_argument, 0, ST_ARG_RSS_SCH_NB},
    {"allow_across_numa_core", no_argument, 0, ST_ARG_ALLOW_ACROSS_NUMA_CORE},
    {"no_multicast", no_argument, 0, ST_ARG_NO_MULTICAST},
    {"tx_user_time_offset", required_argument, 0, ST_ARG_TX_USER_CLOCK_OFFSET},
    {"timestamp_epoch", no_argument, 0, ST_ARG_TIMESTAMP_EPOCH},
    {"auto_stop", no_argument, 0, ST_ARG_AUTO_STOP},
    {"rx_max_file_size", required_argument, 0, ST_ARG_RX_MAX_FILE_SIZE},

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
  ctx->tx_fmd_session_cnt = ctx->json_ctx->tx_fmd_session_cnt;
  ctx->tx_st22p_session_cnt = ctx->json_ctx->tx_st22p_session_cnt;
  ctx->tx_st20p_session_cnt = ctx->json_ctx->tx_st20p_session_cnt;
  ctx->tx_st30p_session_cnt = ctx->json_ctx->tx_st30p_session_cnt;
  ctx->rx_video_session_cnt = ctx->json_ctx->rx_video_session_cnt;
  ctx->rx_audio_session_cnt = ctx->json_ctx->rx_audio_session_cnt;
  ctx->rx_anc_session_cnt = ctx->json_ctx->rx_anc_session_cnt;
  ctx->rx_fmd_session_cnt = ctx->json_ctx->rx_fmd_session_cnt;
  ctx->rx_st22p_session_cnt = ctx->json_ctx->rx_st22p_session_cnt;
  ctx->rx_st20p_session_cnt = ctx->json_ctx->rx_st20p_session_cnt;
  ctx->rx_st30p_session_cnt = ctx->json_ctx->rx_st30p_session_cnt;
  ctx->rx_st20r_session_cnt = ctx->json_ctx->rx_st20r_session_cnt;
  for (int i = 0; i < ctx->json_ctx->num_interfaces; ++i) {
    snprintf(p->port[i], sizeof(p->port[i]), "%s", ctx->json_ctx->interfaces[i].name);
    memcpy(p->sip_addr[i], ctx->json_ctx->interfaces[i].ip_addr, sizeof(p->sip_addr[i]));
    memcpy(p->netmask[i], ctx->json_ctx->interfaces[i].netmask, sizeof(p->netmask[i]));
    memcpy(p->gateway[i], ctx->json_ctx->interfaces[i].gateway, sizeof(p->gateway[i]));
    p->net_proto[i] = ctx->json_ctx->interfaces[i].net_proto;
    p->tx_queues_cnt[i] = ctx->json_ctx->interfaces[i].tx_queues_cnt;
    p->rx_queues_cnt[i] = ctx->json_ctx->interfaces[i].rx_queues_cnt;
    if (ctx->json_ctx->interfaces[i].allow_down_init)
      p->port_params[i].flags |= MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION;
    p->num_ports++;
  }
  if (ctx->json_ctx->sch_quota) {
    p->data_quota_mbs_per_sch =
        ctx->json_ctx->sch_quota * st20_1080p59_yuv422_10bit_bandwidth_mps();
  }
  if (ctx->json_ctx->tx_audio_sessions_max_per_sch) {
    p->tx_audio_sessions_max_per_sch = ctx->json_ctx->tx_audio_sessions_max_per_sch;
  }
  if (ctx->json_ctx->rx_audio_sessions_max_per_sch) {
    p->rx_audio_sessions_max_per_sch = ctx->json_ctx->rx_audio_sessions_max_per_sch;
  }
  if (ctx->json_ctx->shared_tx_queues) p->flags |= MTL_FLAG_SHARED_TX_QUEUE;
  if (ctx->json_ctx->shared_rx_queues) p->flags |= MTL_FLAG_SHARED_RX_QUEUE;
  if (ctx->json_ctx->tx_no_chain) p->flags |= MTL_FLAG_TX_NO_CHAIN;
  if (ctx->json_ctx->rss_mode) p->rss_mode = ctx->json_ctx->rss_mode;
  if (ctx->json_ctx->log_file) st_set_mtl_log_file(ctx, ctx->json_ctx->log_file);

  info("%s, json_file %s succ\n", __func__, json_file);
  return 0;
}

static void log_prefix_time_ms(char* buf, size_t sz) {
  struct timespec ts;
  struct tm tm;
  char time_s_buf[64];

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  strftime(time_s_buf, sizeof(time_s_buf), "%Y-%m-%d %H:%M:%S", &tm);
  snprintf(buf, sz, "%s.%u, ", time_s_buf, (uint32_t)(ts.tv_nsec / NS_PER_MS));
}

static void log_user_printer(enum mtl_log_level level, const char* format, ...) {
  MTL_MAY_UNUSED(level);
  va_list args;

  /* Init variadic argument list */
  va_start(args, format);
  /* Use vprintf to pass the variadic arguments to printf */
  vprintf(format, args);
  /* End variadic argument list */
  va_end(args);
}

static int app_args_parse_port(struct st_app_context* ctx, struct mtl_init_params* p,
                               char* str, enum mtl_port port) {
  st_json_context_t* json_ctx = ctx->json_ctx;
  if (json_ctx) {
    int json_num_interfaces = json_ctx->num_interfaces;
    st_json_interface_t* json_interfaces = &json_ctx->interfaces[port];
    if (port < json_num_interfaces) {
      info("%s, override json interface for port: %d to %s\n", __func__, port, str);
      snprintf(json_interfaces->name, sizeof(json_interfaces->name), "%s", str);
      snprintf(p->port[port], sizeof(p->port[port]), "%s", str);
    }
  } else {
    snprintf(p->port[port], sizeof(p->port[port]), "%s", str);
    p->num_ports++;
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
        app_args_parse_port(ctx, p, optarg, MTL_PORT_P);
        break;
      case ST_ARG_R_PORT:
        app_args_parse_port(ctx, p, optarg, MTL_PORT_R);
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
        inet_pton(AF_INET, optarg, ctx->rx_ip_addr[MTL_PORT_P]);
        break;
      case ST_ARG_R_RX_IP:
        inet_pton(AF_INET, optarg, ctx->rx_ip_addr[MTL_PORT_R]);
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
      case ST_ARG_P_RX_MCAST_SIP:
        inet_pton(AF_INET, optarg, ctx->rx_mcast_sip_addr[MTL_PORT_P]);
        break;
      case ST_ARG_R_RX_MCAST_SIP:
        inet_pton(AF_INET, optarg, ctx->rx_mcast_sip_addr[MTL_PORT_R]);
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
      case ST_ARG_TX_FMD_URL:
        snprintf(ctx->tx_fmd_url, sizeof(ctx->tx_fmd_url), "%s", optarg);
        break;
      case ST_ARG_TX_FMD_RTP_RING_SIZE:
        ctx->tx_fmd_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_TX_FMD_SESSIONS_CNT:
        ctx->tx_fmd_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_SESSIONS_CNT:
        ctx->rx_video_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_FILE_FRAMES:
        ctx->rx_video_file_frames = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_FB_CNT:
        ctx->rx_video_fb_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_RTP_RING_SIZE:
        ctx->rx_video_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_RX_VIDEO_MULTI_THREADS:
        ctx->rx_video_multi_thread = true;
        break;
      case ST_ARG_RX_AUDIO_SESSIONS_CNT:
        ctx->rx_audio_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_AUDIO_RTP_RING_SIZE:
        ctx->rx_audio_rtp_ring_size = atoi(optarg);
        break;
      case ST_ARG_RX_AUDIO_DUMP_TIME_S:
        ctx->rx_audio_dump_time_s = atoi(optarg);
        break;
      case ST_ARG_RX_ANC_SESSIONS_CNT:
        ctx->rx_anc_session_cnt = atoi(optarg);
        break;
      case ST_ARG_RX_FMD_SESSIONS_CNT:
        ctx->rx_fmd_session_cnt = atoi(optarg);
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
        else if (!strcmp(optarg, "tsc_narrow"))
          p->pacing = ST21_TX_PACING_WAY_TSC_NARROW;
        else if (!strcmp(optarg, "ptp"))
          p->pacing = ST21_TX_PACING_WAY_PTP;
        else if (!strcmp(optarg, "be"))
          p->pacing = ST21_TX_PACING_WAY_BE;
        else
          err("%s, unknow pacing way %s\n", __func__, optarg);
        break;
      case ST_ARG_START_VRX:
        ctx->tx_start_vrx = atoi(optarg);
        break;
      case ST_ARG_PAD_INTERVAL:
        ctx->tx_pad_interval = atoi(optarg);
        break;
      case ST_ARG_PAD_STATIC:
        ctx->tx_static_pad = true;
        break;
      case ST_ARG_EXACT_USER_PACING:
        ctx->tx_exact_user_pacing = true;
        break;
      case ST_ARG_TIMESTAMP_EPOCH:
        ctx->tx_ts_epoch = true;
        break;
      case ST_ARG_TIMESTAMP_DELTA_US:
        ctx->tx_ts_delta_us = atoi(optarg);
        break;
      case ST_ARG_NO_BULK:
        ctx->tx_no_bulk = true;
        break;
      case ST_ARG_TX_DISPLAY:
        ctx->tx_display = true;
        break;
      case ST_ARG_RX_DISPLAY:
        ctx->rx_display = true;
        break;
      case ST_ARG_DISABLE_MIGRATE:
        p->flags &= ~MTL_FLAG_TX_VIDEO_MIGRATE;
        p->flags &= ~MTL_FLAG_RX_VIDEO_MIGRATE;
        break;
      case ST_ARG_BIND_NUMA:
        p->flags |= MTL_FLAG_BIND_NUMA;
        break;
      case ST_ARG_NOT_BIND_NUMA:
        p->flags |= MTL_FLAG_NOT_BIND_NUMA;
        break;
      case ST_ARG_FORCE_NUMA:
        for (int port = 0; port < MTL_PORT_MAX; port++) {
          p->port_params[port].flags |= MTL_PORT_FLAG_FORCE_NUMA;
          p->port_params[port].socket_id = atoi(optarg);
        }
        break;
      case ST_ARG_FORCE_TX_VIDEO_NUMA:
        ctx->force_tx_video_numa = atoi(optarg);
        break;
      case ST_ARG_FORCE_RX_VIDEO_NUMA:
        ctx->force_rx_video_numa = atoi(optarg);
        break;
      case ST_ARG_FORCE_TX_AUDIO_NUMA:
        ctx->force_tx_audio_numa = atoi(optarg);
        break;
      case ST_ARG_FORCE_RX_AUDIO_NUMA:
        ctx->force_rx_audio_numa = atoi(optarg);
        break;
      case ST_ARG_SHAPING:
        if (!strcmp(optarg, "narrow"))
          ctx->tx_pacing_type = ST21_PACING_NARROW;
        else if (!strcmp(optarg, "wide"))
          ctx->tx_pacing_type = ST21_PACING_WIDE;
        else if (!strcmp(optarg, "linear"))
          ctx->tx_pacing_type = ST21_PACING_LINEAR;
        else
          err("%s, unknow shaping way %s\n", __func__, optarg);
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
      case ST_ARG_CNI_TASKLET:
        p->flags |= MTL_FLAG_CNI_TASKLET;
        break;
      case ST_ARG_TEST_TIME:
        ctx->test_time_s = atoi(optarg);
        break;
      case ST_ARG_RX_TIMING_PARSER_STAT:
        ctx->enable_timing_parser = true;
        p->flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;
        break;
      case ST_ARG_RX_TIMING_PARSER_META:
        ctx->enable_timing_parser_meta = true;
        p->flags |= MTL_FLAG_ENABLE_HW_TIMESTAMP;
        break;
      case ST_ARG_RX_BURST_SZ:
        ctx->rx_burst_size = atoi(optarg);
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
      case ST_ARG_DEDICATE_SYS_LCORE:
        p->flags |= MTL_FLAG_DEDICATED_SYS_LCORE;
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
      case ST_ARG_LIB_PTP_SYNC_SYS:
        p->flags |= MTL_FLAG_PTP_ENABLE; /* enable built-in ptp */
        p->ptp_get_time_fn = NULL;       /* clear the user ptp func */
        ctx->ptp_systime_sync = true;
        break;
      case ST_ARG_LIB_PHC2SYS:
        p->flags |= MTL_FLAG_PHC2SYS_ENABLE;
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
          p->log_level = MTL_LOG_LEVEL_ERR;
        else
          err("%s, unknow log level %s\n", __func__, optarg);
        app_set_log_level(p->log_level);
        break;
      case ST_ARG_LOG_FILE:
        st_set_mtl_log_file(ctx, optarg);
        break;
      case ST_ARG_LOG_TIME_MS:
        mtl_set_log_prefix_formatter(log_prefix_time_ms);
        break;
      case ST_ARG_LOG_PRINTER:
        mtl_set_log_printer(log_user_printer);
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
      case ST_ARG_APP_BIND_THREAD:
        ctx->app_bind_lcore = false;
        break;
      case ST_ARG_APP_BIND_LCORE:
        ctx->app_bind_lcore = true;
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
      case ST_ARG_RANDOM_SRC_PORT:
        p->flags |= MTL_FLAG_RANDOM_SRC_PORT;
        break;
      case ST_ARG_RSS_MODE:
        if (!strcmp(optarg, "l3"))
          p->rss_mode = MTL_RSS_MODE_L3;
        else if (!strcmp(optarg, "l3_l4"))
          p->rss_mode = MTL_RSS_MODE_L3_L4;
        else if (!strcmp(optarg, "none"))
          p->rss_mode = MTL_RSS_MODE_NONE;
        else
          err("%s, unknow rss mode %s\n", __func__, optarg);
        break;
      case ST_ARG_TX_NO_CHAIN:
        p->flags |= MTL_FLAG_TX_NO_CHAIN;
        break;
      case ST_ARG_TX_NO_BURST_CHECK:
        p->flags |= MTL_FLAG_TX_NO_BURST_CHK;
        break;
      case ST_ARG_MULTI_SRC_PORT:
        p->flags |= MTL_FLAG_MULTI_SRC_PORT;
        break;
      case ST_ARG_AUDIO_BUILD_PACING:
        ctx->tx_audio_build_pacing = true;
        break;
      case ST_ARG_AUDIO_DEDICATE_QUEUE:
        ctx->tx_audio_dedicate_queue = true;
        break;
      case ST_ARG_AUDIO_TX_PACING:
        if (!strcmp(optarg, "auto"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_AUTO;
        else if (!strcmp(optarg, "rl"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_RL;
        else if (!strcmp(optarg, "tsc"))
          ctx->tx_audio_pacing_way = ST30_TX_PACING_WAY_TSC;
        else
          err("%s, unknow audio tx pacing %s\n", __func__, optarg);
        break;
      case ST_ARG_AUDIO_RL_ACCURACY_US:
        ctx->tx_audio_rl_accuracy_us = atoi(optarg);
        break;
      case ST_ARG_AUDIO_RL_OFFSET_US:
        ctx->tx_audio_rl_offset_us = atoi(optarg);
        break;
      case ST_ARG_AUDIO_FIFO_SIZE:
        ctx->tx_audio_fifo_size = atoi(optarg);
        break;
      case ST_ARG_ANC_DEDICATE_QUEUE:
        ctx->tx_anc_dedicate_queue = true;
        break;
      case ST_ARG_FMD_DEDICATE_QUEUE:
        ctx->tx_fmd_dedicate_queue = true;
        break;
      case ST_ARG_DHCP:
        for (int port = 0; port < MTL_PORT_MAX; ++port)
          p->net_proto[port] = MTL_PROTO_DHCP;
        break;
      case ST_ARG_IOVA_MODE:
        if (!strcmp(optarg, "va"))
          p->iova_mode = MTL_IOVA_MODE_VA;
        else if (!strcmp(optarg, "pa"))
          p->iova_mode = MTL_IOVA_MODE_PA;
        else
          err("%s, unknow iova mode %s\n", __func__, optarg);
        break;
      case ST_ARG_SHARED_TX_QUEUES:
        p->flags |= MTL_FLAG_SHARED_TX_QUEUE;
        break;
      case ST_ARG_SHARED_RX_QUEUES:
        p->flags |= MTL_FLAG_SHARED_RX_QUEUE;
        break;
      case ST_ARG_RX_USE_CNI:
        p->flags |= MTL_FLAG_RX_USE_CNI;
        break;
      case ST_ARG_RX_UDP_PORT_ONLY:
        p->flags |= MTL_FLAG_RX_UDP_PORT_ONLY;
        break;
      case ST_ARG_VIRTIO_USER:
        p->flags |= MTL_FLAG_VIRTIO_USER;
        break;
      case ST_ARG_VIDEO_SHA_CHECK:
        ctx->video_sha_check = true;
        break;
      case ST_ARG_ARP_TIMEOUT_S:
        p->arp_timeout_s = atoi(optarg);
        break;
      case ST_ARG_RSS_SCH_NB:
        for (enum mtl_port port = MTL_PORT_P; port < MTL_PORT_MAX; port++) {
          p->rss_sch_nb[port] = atoi(optarg);
        }
        break;
      case ST_ARG_ALLOW_ACROSS_NUMA_CORE:
        p->flags |= MTL_FLAG_ALLOW_ACROSS_NUMA_CORE;
        break;
      case ST_ARG_NO_MULTICAST:
        p->flags |= MTL_FLAG_NO_MULTICAST;
        break;
      case ST_ARG_TX_USER_CLOCK_OFFSET:
        ctx->user_time.user_time_offset = atoi(optarg);
        break;
      case ST_ARG_AUTO_STOP:
        ctx->auto_stop = true;
        break;
      case ST_ARG_RX_MAX_FILE_SIZE:
        ctx->rx_max_file_size = strtoul(optarg, NULL, 0);
        break;
      case '?':
        break;
      default:
        break;
    }
  };

  return 0;
}
