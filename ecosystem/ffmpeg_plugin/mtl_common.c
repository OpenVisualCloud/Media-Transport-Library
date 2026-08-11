/*
 * MTL common struct and functions
 * Copyright (c) 2024 Intel
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "mtl_common.h"

static mtl_handle g_mtl_shared_handle = NULL;
static int g_mtl_ref_cnt;
static int g_mtl_ptp_enable;
static int g_mtl_ptp_pi;
static int g_mtl_ptp_unicast;

enum st_fps framerate_to_st_fps(AVRational framerate) {
  double fps = (double)framerate.num / (double)framerate.den;

  return st_frame_rate_to_st_fps(fps);
}

mtl_handle mtl_dev_get(AVFormatContext* ctx, const struct StDevArgs* args, int* idx) {
  struct mtl_init_params p;
  mtl_handle handle = NULL;

  if (!args->ptp_enable && (args->ptp_pi || args->ptp_unicast)) {
    err(ctx, "%s, ptp_pi and ptp_unicast require ptp_enable\n", __func__);
    return NULL;
  }

  if (g_mtl_shared_handle) {
    if ((args->ptp_enable && !g_mtl_ptp_enable) || (args->ptp_pi && !g_mtl_ptp_pi) ||
        (args->ptp_unicast && !g_mtl_ptp_unicast)) {
      err(ctx, "%s, shared handle does not support requested PTP options\n", __func__);
      return NULL;
    }
    *idx = g_mtl_ref_cnt;
    g_mtl_ref_cnt++;
    info(ctx, "%s, shared handle %p ref cnt %d\n", __func__, g_mtl_shared_handle,
         g_mtl_ref_cnt);
    return g_mtl_shared_handle;
  }

  memset(&p, 0, sizeof(p));

  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (!args->port[i]) continue;
    int port = p.num_ports;
    snprintf(p.port[port], sizeof(p.port[port]), "%s", args->port[i]);
    p.pmd[port] = mtl_pmd_by_port_name(p.port[port]);
    if (args->sip[i]) {
      int ret = inet_pton(AF_INET, args->sip[i], p.sip_addr[port]);
      if (ret != 1) {
        err(ctx, "%s, %d sip %s is not valid ip address\n", __func__, i, args->sip[i]);
        return NULL;
      }
    }
    p.tx_queues_cnt[port] = args->tx_queues_cnt[i];
    p.rx_queues_cnt[port] = args->rx_queues_cnt[i];
    p.num_ports++;
  }

  p.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
  p.flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
  p.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  p.flags |= MTL_FLAG_BIND_NUMA;
  p.log_level = MTL_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING

  if (args->ptp_enable) {
    p.flags |= MTL_FLAG_PTP_ENABLE;
    p.pacing = ST21_TX_PACING_WAY_PTP;
    if (args->ptp_pi) p.flags |= MTL_FLAG_PTP_PI;
    if (args->ptp_unicast) p.flags |= MTL_FLAG_PTP_UNICAST_ADDR;
    info(ctx, "%s, PTP enabled (pi=%d unicast=%d)\n", __func__, args->ptp_pi,
         args->ptp_unicast);
  }

  if (args->dma_dev) {
    char devs[128] = {0};
    char* next_dev;

    snprintf(devs, sizeof(devs), "%s", args->dma_dev);

    next_dev = strtok(devs, ",");
    while (next_dev && (p.num_dma_dev_port < MTL_DMA_DEV_MAX)) {
      err(ctx, "%s, append dma dev: %s\n", __func__, next_dev);
      snprintf(p.dma_dev_port[p.num_dma_dev_port],
               sizeof(p.dma_dev_port[p.num_dma_dev_port]), "%s", next_dev);
      p.num_dma_dev_port++;
      next_dev = strtok(NULL, ",");
    }
  }

  handle = mtl_init(&p);
  if (!handle) {
    err(ctx, "%s, mtl_init fail\n", __func__);
    return NULL;
  }

  g_mtl_shared_handle = handle;
  g_mtl_ptp_enable = args->ptp_enable;
  g_mtl_ptp_pi = args->ptp_pi;
  g_mtl_ptp_unicast = args->ptp_unicast;
  *idx = 0;
  g_mtl_ref_cnt++;
  info(ctx, "%s, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  return handle;
}

int mtl_instance_put(AVFormatContext* ctx, mtl_handle handle) {
  if (handle != g_mtl_shared_handle) {
    err(ctx, "%s, error handle %p %p\n", __func__, handle, g_mtl_shared_handle);
    return AVERROR(EIO);
  }

  g_mtl_ref_cnt--;
  info(ctx, "%s, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  if (g_mtl_ref_cnt <= 0) {
    info(ctx, "%s, ref cnt reach zero, uninit mtl device\n", __func__);
    mtl_uninit(handle);
    g_mtl_shared_handle = NULL;
    g_mtl_ptp_enable = 0;
    g_mtl_ptp_pi = 0;
    g_mtl_ptp_unicast = 0;
  }

  return 0;
}

int mtl_parse_rx_port(AVFormatContext* ctx, const struct StDevArgs* devArgs,
                      const StRxSessionPortArgs* args, struct st_rx_port* port) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    /* if no special port in StRxSessionPortArgs, get from StDevArgs */
    if (!args->port[i] && !devArgs->port[i]) break;
    dbg(ctx, "%s, port on %d\n", __func__, i);
    snprintf(port->port[i], sizeof(port->port[i]), "%s",
             args->port[i] ? args->port[i] : devArgs->port[i]);
    if (args->sip[i]) {
      int ret = inet_pton(AF_INET, args->sip[i], port->ip_addr[i]);
      if (ret != 1) {
        err(ctx, "%s, %d sip %s is not valid ip address\n", __func__, i, args->sip[i]);
        return AVERROR(EINVAL);
      }
    }
    if ((args->udp_port < 0) || (args->udp_port > 0xFFFF)) {
      err(ctx, "%s, invalid UDP port: %d\n", __func__, args->udp_port);
      return AVERROR(EINVAL);
    }
    if ((args->payload_type < 0) || (args->payload_type > 0x7F)) {
      err(ctx, "%s, invalid payload_type: %d\n", __func__, args->payload_type);
      return AVERROR(EINVAL);
    }
    port->udp_port[i] = args->udp_port;
    port->payload_type = args->payload_type;
    port->num_port++;
  }

  return 0;
}

int mtl_parse_tx_port(AVFormatContext* ctx, const struct StDevArgs* devArgs,
                      const StTxSessionPortArgs* args, struct st_tx_port* port) {
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    /* if no special port in StTxSessionPortArgs, get from StDevArgs */
    if (!args->port[i] && !devArgs->port[i]) break;
    dbg(ctx, "%s, port on %d\n", __func__, i);
    snprintf(port->port[i], sizeof(port->port[i]), "%s",
             args->port[i] ? args->port[i] : devArgs->port[i]);
    if (args->dip[i]) {
      int ret = inet_pton(AF_INET, args->dip[i], port->dip_addr[i]);
      if (ret != 1) {
        err(ctx, "%s, %d dip %s is not valid ip address\n", __func__, i, args->dip[i]);
        return AVERROR(EINVAL);
      }
    }
    if ((args->udp_port < 0) || (args->udp_port > 0xFFFF)) {
      err(ctx, "%s, invalid UDP port: %d\n", __func__, args->udp_port);
      return AVERROR(EINVAL);
    }
    if ((args->payload_type < 0) || (args->payload_type > 0x7F)) {
      err(ctx, "%s, invalid payload_type: %d\n", __func__, args->payload_type);
      return AVERROR(EINVAL);
    }
    port->udp_port[i] = args->udp_port;
    port->payload_type = args->payload_type;
    port->num_port++;
  }

  return 0;
}

int mtl_parse_st30_sample_rate(enum st30_sampling* sample_rate, int value) {
  switch (value) {
    case 48000:
      *sample_rate = ST30_SAMPLING_48K;
      return 0;
    case 96000:
      *sample_rate = ST30_SAMPLING_96K;
      return 0;
    case 44100:
      *sample_rate = ST31_SAMPLING_44K;
      return 0;
    default:
      return AVERROR(EINVAL);
  }
}
