/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

#include <getopt.h>
#include <inttypes.h>

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#include <mtl/mudp_sockfd_internal.h>
// clang-format on

enum sample_args_cmd {
  SAMPLE_ARG_UNKNOWN = 0,

  SAMPLE_ARG_P_PORT = 0x100, /* start from end of ascii */
  SAMPLE_ARG_R_PORT,
  SAMPLE_ARG_P_TX_IP,
  SAMPLE_ARG_R_TX_IP,
  SAMPLE_ARG_P_RX_IP,
  SAMPLE_ARG_R_RX_IP,
  SAMPLE_ARG_P_SIP,
  SAMPLE_ARG_R_SIP,
  SAMPLE_ARG_P_FWD_IP,
  SAMPLE_ARG_LOG_LEVEL,
  SAMPLE_ARG_DEV_AUTO_START,
  SAMPLE_ARG_DMA_PORT,
  SAMPLE_ARG_SHARED_QUEUES,
  SAMPLE_ARG_QUEUES_CNT,
  SAMPLE_ARG_P_TX_DST_MAC,
  SAMPLE_ARG_R_TX_DST_MAC,
  SAMPLE_ARG_P_NETMASK,
  SAMPLE_ARG_R_NETMASK,
  SAMPLE_ARG_P_GATEWAY,
  SAMPLE_ARG_R_GATEWAY,
  SAMPLE_ARG_PTP_TSC,
  SAMPLE_ARG_UDP_LCORE,
  SAMPLE_ARG_RSS_MODE,

  SAMPLE_ARG_TX_VIDEO_URL = 0x200,
  SAMPLE_ARG_RX_VIDEO_URL,
  SAMPLE_ARG_LOGO_URL,
  SAMPLE_ARG_WIDTH,
  SAMPLE_ARG_HEIGHT,
  SAMPLE_ARG_SESSIONS_CNT,
  SAMPLE_ARG_EXT_FRAME,
  SAMPLE_ARG_ST22_CODEC,
  SAMPLE_ARG_ST22_FRAME_FMT,

  SAMPLE_ARG_UDP_MODE = 0x300,
  SAMPLE_ARG_UDP_LEN,
  SAMPLE_ARG_UDP_TX_BPS_G,

  SAMPLE_ARG_MAX,
};

static struct option sample_args_options[] = {
    {"p_port", required_argument, 0, SAMPLE_ARG_P_PORT},
    {"r_port", required_argument, 0, SAMPLE_ARG_R_PORT},
    {"p_tx_ip", required_argument, 0, SAMPLE_ARG_P_TX_IP},
    {"r_tx_ip", required_argument, 0, SAMPLE_ARG_R_TX_IP},
    {"p_rx_ip", required_argument, 0, SAMPLE_ARG_P_RX_IP},
    {"r_rx_ip", required_argument, 0, SAMPLE_ARG_R_RX_IP},
    {"p_sip", required_argument, 0, SAMPLE_ARG_P_SIP},
    {"r_sip", required_argument, 0, SAMPLE_ARG_R_SIP},
    {"p_fwd_ip", required_argument, 0, SAMPLE_ARG_P_FWD_IP},
    {"sessions_cnt", required_argument, 0, SAMPLE_ARG_SESSIONS_CNT},
    {"log_level", required_argument, 0, SAMPLE_ARG_LOG_LEVEL},
    {"dev_auto_start", no_argument, 0, SAMPLE_ARG_DEV_AUTO_START},
    {"dma_port", required_argument, 0, SAMPLE_ARG_DMA_PORT},
    {"shared_queues", no_argument, 0, SAMPLE_ARG_SHARED_QUEUES},
    {"queues_cnt", required_argument, 0, SAMPLE_ARG_QUEUES_CNT},
    {"p_tx_dst_mac", required_argument, 0, SAMPLE_ARG_P_TX_DST_MAC},
    {"r_tx_dst_mac", required_argument, 0, SAMPLE_ARG_R_TX_DST_MAC},
    {"p_netmask", required_argument, 0, SAMPLE_ARG_P_NETMASK},
    {"r_netmask", required_argument, 0, SAMPLE_ARG_R_NETMASK},
    {"p_gateway", required_argument, 0, SAMPLE_ARG_P_GATEWAY},
    {"r_gateway", required_argument, 0, SAMPLE_ARG_R_GATEWAY},
    {"ptp_tsc", no_argument, 0, SAMPLE_ARG_PTP_TSC},
    {"udp_lcore", no_argument, 0, SAMPLE_ARG_UDP_LCORE},
    {"rss_mode", required_argument, 0, SAMPLE_ARG_RSS_MODE},

    {"tx_url", required_argument, 0, SAMPLE_ARG_TX_VIDEO_URL},
    {"rx_url", required_argument, 0, SAMPLE_ARG_RX_VIDEO_URL},
    {"logo_url", required_argument, 0, SAMPLE_ARG_LOGO_URL},
    {"width", required_argument, 0, SAMPLE_ARG_WIDTH},
    {"height", required_argument, 0, SAMPLE_ARG_HEIGHT},
    {"ext_frame", no_argument, 0, SAMPLE_ARG_EXT_FRAME},
    {"st22_codec", required_argument, 0, SAMPLE_ARG_ST22_CODEC},
    {"st22_fmt", required_argument, 0, SAMPLE_ARG_ST22_FRAME_FMT},

    {"udp_mode", required_argument, 0, SAMPLE_ARG_UDP_MODE},
    {"udp_len", required_argument, 0, SAMPLE_ARG_UDP_LEN},
    {"udp_tx_bps_g", required_argument, 0, SAMPLE_ARG_UDP_TX_BPS_G},

    {0, 0, 0, 0}};

static int sample_args_parse_tx_mac(struct st_sample_context* ctx, char* mac_str,
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

static int _sample_parse_args(struct st_sample_context* ctx, int argc, char** argv) {
  int cmd = -1, optIdx = 0;
  struct mtl_init_params* p = &ctx->param;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", sample_args_options, &optIdx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case SAMPLE_ARG_P_PORT:
        snprintf(p->port[MTL_PORT_P], sizeof(p->port[MTL_PORT_P]), "%s", optarg);
        p->num_ports++;
        break;
      case SAMPLE_ARG_R_PORT:
        snprintf(p->port[MTL_PORT_R], sizeof(p->port[MTL_PORT_R]), "%s", optarg);
        p->num_ports++;
        break;
      case SAMPLE_ARG_DMA_PORT:
        snprintf(p->dma_dev_port[0], sizeof(p->dma_dev_port[0]), "%s", optarg);
        p->num_dma_dev_port = 1;
        break;
      case SAMPLE_ARG_P_SIP:
        inet_pton(AF_INET, optarg, mtl_p_sip_addr(p));
        break;
      case SAMPLE_ARG_R_SIP:
        inet_pton(AF_INET, optarg, mtl_r_sip_addr(p));
        break;
      case SAMPLE_ARG_P_TX_IP:
        inet_pton(AF_INET, optarg, ctx->tx_dip_addr[MTL_PORT_P]);
        break;
      case SAMPLE_ARG_R_TX_IP:
        inet_pton(AF_INET, optarg, ctx->tx_dip_addr[MTL_PORT_R]);
        break;
      case SAMPLE_ARG_P_RX_IP:
        inet_pton(AF_INET, optarg, ctx->rx_sip_addr[MTL_PORT_P]);
        break;
      case SAMPLE_ARG_R_RX_IP:
        inet_pton(AF_INET, optarg, ctx->rx_sip_addr[MTL_PORT_R]);
        break;
      case SAMPLE_ARG_P_FWD_IP:
        inet_pton(AF_INET, optarg, ctx->fwd_dip_addr[MTL_PORT_P]);
        break;
      case SAMPLE_ARG_P_NETMASK:
        inet_pton(AF_INET, optarg, p->netmask[MTL_PORT_P]);
        break;
      case SAMPLE_ARG_R_NETMASK:
        inet_pton(AF_INET, optarg, p->netmask[MTL_PORT_R]);
        break;
      case SAMPLE_ARG_P_GATEWAY:
        inet_pton(AF_INET, optarg, p->gateway[MTL_PORT_P]);
        break;
      case SAMPLE_ARG_R_GATEWAY:
        inet_pton(AF_INET, optarg, p->gateway[MTL_PORT_R]);
        break;
      case SAMPLE_ARG_LOG_LEVEL:
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
      case SAMPLE_ARG_DEV_AUTO_START:
        p->flags |= MTL_FLAG_DEV_AUTO_START_STOP;
        break;
      case SAMPLE_ARG_SHARED_QUEUES:
        p->flags |= MTL_FLAG_SHARED_QUEUE;
        break;
      case SAMPLE_ARG_PTP_TSC:
        p->flags |= MTL_FLAG_PTP_SOURCE_TSC;
        break;
      case SAMPLE_ARG_UDP_LCORE:
        p->flags |= MTL_FLAG_UDP_LCORE;
        break;
      case SAMPLE_ARG_RSS_MODE:
        if (!strcmp(optarg, "l3"))
          p->rss_mode = MTL_RSS_MODE_L3;
        else if (!strcmp(optarg, "l3_l4"))
          p->rss_mode = MTL_RSS_MODE_L3_L4;
        else if (!strcmp(optarg, "l3_l4_dst_port_only"))
          p->rss_mode = MTL_RSS_MODE_L3_L4_DP_ONLY;
        else if (!strcmp(optarg, "l3_da_l4_dst_port_only"))
          p->rss_mode = MTL_RSS_MODE_L3_DA_L4_DP_ONLY;
        else if (!strcmp(optarg, "l4_dst_port_only"))
          p->rss_mode = MTL_RSS_MODE_L4_DP_ONLY;
        else if (!strcmp(optarg, "none"))
          p->rss_mode = MTL_RSS_MODE_NONE;
        else
          err("%s, unknow rss mode %s\n", __func__, optarg);
        break;
      case SAMPLE_ARG_QUEUES_CNT:
        p->rx_queues_cnt_max = atoi(optarg);
        p->tx_queues_cnt_max = p->rx_queues_cnt_max;
        break;
      case SAMPLE_ARG_P_TX_DST_MAC:
        sample_args_parse_tx_mac(ctx, optarg, MTL_PORT_P);
        break;
      case SAMPLE_ARG_R_TX_DST_MAC:
        sample_args_parse_tx_mac(ctx, optarg, MTL_PORT_R);
        break;
      case SAMPLE_ARG_TX_VIDEO_URL:
        snprintf(ctx->tx_url, sizeof(ctx->tx_url), "%s", optarg);
        break;
      case SAMPLE_ARG_RX_VIDEO_URL:
        snprintf(ctx->rx_url, sizeof(ctx->rx_url), "%s", optarg);
        break;
      case SAMPLE_ARG_LOGO_URL:
        snprintf(ctx->logo_url, sizeof(ctx->rx_url), "%s", optarg);
        break;
      case SAMPLE_ARG_WIDTH:
        ctx->width = atoi(optarg);
        break;
      case SAMPLE_ARG_HEIGHT:
        ctx->height = atoi(optarg);
        break;
      case SAMPLE_ARG_SESSIONS_CNT:
        ctx->sessions = atoi(optarg);
        break;
      case SAMPLE_ARG_EXT_FRAME:
        ctx->ext_frame = true;
        break;
      case SAMPLE_ARG_ST22_CODEC:
        if (!strcmp(optarg, "jpegxs"))
          ctx->st22p_codec = ST22_CODEC_JPEGXS;
        else if (!strcmp(optarg, "h264_cbr"))
          ctx->st22p_codec = ST22_CODEC_H264_CBR;
        else
          err("%s, unknown codec %s\n", __func__, optarg);
        break;
      case SAMPLE_ARG_ST22_FRAME_FMT: {
        enum st_frame_fmt fmt = st_frame_name_to_fmt(optarg);
        if (fmt < ST_FRAME_FMT_MAX) {
          ctx->st22p_input_fmt = fmt;
          ctx->st22p_output_fmt = fmt;
        } else {
          err("%s, unknown fmt %s\n", __func__, optarg);
        }
        break;
      }
      case SAMPLE_ARG_UDP_MODE:
        if (!strcmp(optarg, "default"))
          ctx->udp_mode = SAMPLE_UDP_DEFAULT;
        else if (!strcmp(optarg, "transport"))
          ctx->udp_mode = SAMPLE_UDP_TRANSPORT;
        else if (!strcmp(optarg, "transport_poll"))
          ctx->udp_mode = SAMPLE_UDP_TRANSPORT_POLL;
        else if (!strcmp(optarg, "transport_unify_poll"))
          ctx->udp_mode = SAMPLE_UDP_TRANSPORT_UNIFY_POLL;
        else
          err("%s, unknow udp_mode %s\n", __func__, optarg);
        break;
      case SAMPLE_ARG_UDP_TX_BPS_G:
        ctx->udp_tx_bps = ((uint64_t)atoi(optarg)) * 1024 * 1024 * 1024;
        break;
      case SAMPLE_ARG_UDP_LEN:
        ctx->udp_len = atoi(optarg);
        break;
      case '?':
        break;
      default:
        break;
    }
  };

  return 0;
}

static struct st_sample_context* g_sample_ctx;
static void sample_sig_handler(int signo) {
  struct st_sample_context* ctx = g_sample_ctx;
  info("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      ctx->exit = true;
      if (ctx->st) mtl_abort(ctx->st);
      if (ctx->sig_handler) ctx->sig_handler(signo);
      break;
  }

  return;
}

int sample_parse_args(struct st_sample_context* ctx, int argc, char** argv, bool tx,
                      bool rx, bool unicast) {
  struct mtl_init_params* p = &ctx->param;

  g_sample_ctx = ctx;
  signal(SIGINT, sample_sig_handler);

  p->flags |= MTL_FLAG_BIND_NUMA; /* default bind to numa */
  // p->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  p->log_level = MTL_LOG_LEVEL_INFO; /* default to info */
  /* use different default port/ip for tx and rx */
  if (rx) {
    strncpy(p->port[MTL_PORT_P], "0000:af:01.0", MTL_PORT_MAX_LEN);
    inet_pton(AF_INET, "192.168.85.80", mtl_p_sip_addr(p));
    strncpy(p->port[MTL_PORT_R], "0000:af:01.1", MTL_PORT_MAX_LEN);
    inet_pton(AF_INET, "192.168.85.81", mtl_r_sip_addr(p));
  } else {
    strncpy(p->port[MTL_PORT_P], "0000:af:01.1", MTL_PORT_MAX_LEN);
    inet_pton(AF_INET, "192.168.85.60", mtl_p_sip_addr(p));
    strncpy(p->port[MTL_PORT_R], "0000:af:01.0", MTL_PORT_MAX_LEN);
    inet_pton(AF_INET, "192.168.85.61", mtl_r_sip_addr(p));
  }
  if (unicast) {
    inet_pton(AF_INET, "192.168.85.80", ctx->tx_dip_addr[MTL_PORT_P]);
    inet_pton(AF_INET, "192.168.85.81", ctx->tx_dip_addr[MTL_PORT_R]);
    inet_pton(AF_INET, "192.168.85.60", ctx->rx_sip_addr[MTL_PORT_P]);
    inet_pton(AF_INET, "192.168.85.61", ctx->rx_sip_addr[MTL_PORT_R]);
  } else {
    inet_pton(AF_INET, "239.168.85.20", ctx->tx_dip_addr[MTL_PORT_P]);
    inet_pton(AF_INET, "239.168.85.21", ctx->tx_dip_addr[MTL_PORT_R]);
    inet_pton(AF_INET, "239.168.85.20", ctx->rx_sip_addr[MTL_PORT_P]);
    inet_pton(AF_INET, "239.168.85.21", ctx->rx_sip_addr[MTL_PORT_R]);
  }
  inet_pton(AF_INET, "239.168.86.20", ctx->fwd_dip_addr[MTL_PORT_P]);
  inet_pton(AF_INET, "239.168.86.21", ctx->fwd_dip_addr[MTL_PORT_R]);

  strncpy(p->dma_dev_port[0], "0000:80:04.0", MTL_PORT_MAX_LEN);

  if (!ctx->sessions) ctx->sessions = 1;
  ctx->framebuff_cnt = 3;
  ctx->width = 1920;
  ctx->height = 1080;
  ctx->fps = ST_FPS_P59_94;
  ctx->fmt = ST20_FMT_YUV_422_10BIT;
  ctx->input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ctx->output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ctx->udp_port = 20000;
  ctx->payload_type = 112;
  strncpy(ctx->tx_url, "test.yuv", sizeof(ctx->tx_url));
  strncpy(ctx->rx_url, "rx.yuv", sizeof(ctx->rx_url));

  strncpy(ctx->logo_url, "logo.yuv", sizeof(ctx->rx_url));
  ctx->logo_width = 200;
  ctx->logo_height = 200;

  ctx->st22p_input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ctx->st22p_output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ctx->st22p_codec = ST22_CODEC_JPEGXS;

  p->tx_queues_cnt_max = 8;
  p->rx_queues_cnt_max = 8;

  _sample_parse_args(ctx, argc, argv);

  p->tx_sessions_cnt_max = ctx->sessions;
  p->rx_sessions_cnt_max = ctx->sessions;
  /* always enable 1 port */
  if (!p->num_ports) p->num_ports = 1;

  return 0;
}

int tx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv) {
  return sample_parse_args(ctx, argc, argv, true, false, false);
};

int rx_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv) {
  return sample_parse_args(ctx, argc, argv, false, true, false);
};

int fwd_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv) {
  return sample_parse_args(ctx, argc, argv, true, true, false);
};

int dma_sample_parse_args(struct st_sample_context* ctx, int argc, char** argv) {
  /* init sample(st) dev */
  sample_parse_args(ctx, argc, argv, false, false, false);
  /* enable dma port */
  ctx->param.num_dma_dev_port = 1;
  return 0;
};

void fill_rfc4175_422_10_pg2_data(struct st20_rfc4175_422_10_pg2_be* data, int w, int h) {
  int pg_size = w * h / 2;
  uint16_t cb, y0, cr, y1; /* 10 bit */

  y0 = 0x111;

  cb = 0x222;
  cr = 0x333;
  y1 = y0 + 1;

  for (int pg = 0; pg < pg_size; pg++) {
    data->Cb00 = cb >> 2;
    data->Cb00_ = cb;
    data->Y00 = y0 >> 4;
    data->Y00_ = y0;
    data->Cr00 = cr >> 6;
    data->Cr00_ = cr;
    data->Y01 = y1 >> 8;
    data->Y01_ = y1;
    data++;

    cb++;
    y0 += 2;
    cr++;
    y1 += 2;
  }
}

void fill_rfc4175_422_12_pg2_data(struct st20_rfc4175_422_12_pg2_be* data, int w, int h) {
  int pg_size = w * h / 2;
  uint16_t cb, y0, cr, y1; /* 12 bit */

  y0 = 0x111;

  cb = 0x222;
  cr = 0x333;
  y1 = y0 + 1;

  for (int pg = 0; pg < pg_size; pg++) {
    data->Cb00 = cb >> 4;
    data->Cb00_ = cb;
    data->Y00 = y0 >> 8;
    data->Y00_ = y0;
    data->Cr00 = cr >> 4;
    data->Cr00_ = cr;
    data->Y01 = y1 >> 8;
    data->Y01_ = y1;
    data++;

    cb++;
    y0 += 2;
    cr++;
    y1 += 2;
  }
}

int ufd_override_check(struct st_sample_context* ctx) {
  struct mufd_override_params override;
  bool has_override = false;

  memset(&override, 0, sizeof(override));
  override.log_level = MTL_LOG_LEVEL_INFO;
  /* check if user has assigned extra arguments */
  if (ctx->param.log_level != MTL_LOG_LEVEL_INFO) {
    has_override = true;
    override.log_level = ctx->param.log_level;
  }
  if (ctx->param.flags & MTL_FLAG_UDP_LCORE) {
    has_override = true;
    override.lcore_mode = true;
  }
  if (ctx->param.flags & MTL_FLAG_SHARED_QUEUE) {
    has_override = true;
    override.shared_queue = true;
  }
  if (has_override) {
    mufd_commit_override_params(&override);
  }

  return 0;
}