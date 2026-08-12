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

#include <pthread.h>

static mtl_handle g_mtl_shared_handle = NULL;
static int g_mtl_ref_cnt;
static struct mtl_init_params g_mtl_shared_params;
static pthread_mutex_t g_mtl_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MTL_FFMPEG_PTP_FLAGS \
  (MTL_FLAG_PTP_ENABLE | MTL_FLAG_PTP_PI | MTL_FLAG_PTP_UNICAST_ADDR)

enum st_fps framerate_to_st_fps(AVRational framerate) {
  double fps = (double)framerate.num / (double)framerate.den;

  return st_frame_rate_to_st_fps(fps);
}

static int mtl_dev_build_params(AVFormatContext* ctx, const struct StDevArgs* args,
                                struct mtl_init_params* p) {
  memset(p, 0, sizeof(*p));

  if (!args->ptp_enable && (args->ptp_pi || args->ptp_unicast)) {
    err(ctx, "%s, ptp_pi and ptp_unicast require ptp_enable\n", __func__);
    return AVERROR(EINVAL);
  }

  for (int i = 0; i < MTL_PORT_MAX; i++) {
    if (!args->port[i]) continue;
    int port = p->num_ports;
    if (strlen(args->port[i]) >= sizeof(p->port[port])) {
      err(ctx, "%s, port name on %d is too long\n", __func__, i);
      return AVERROR(EINVAL);
    }
    if ((args->tx_queues_cnt[i] < 0) || (args->tx_queues_cnt[i] > UINT16_MAX) ||
        (args->rx_queues_cnt[i] < 0) || (args->rx_queues_cnt[i] > UINT16_MAX)) {
      err(ctx, "%s, invalid queue count on port %d\n", __func__, i);
      return AVERROR(EINVAL);
    }
    snprintf(p->port[port], sizeof(p->port[port]), "%s", args->port[i]);
    p->pmd[port] = mtl_pmd_by_port_name(p->port[port]);
    if (args->sip[i]) {
      int ret = inet_pton(AF_INET, args->sip[i], p->sip_addr[port]);
      if (ret != 1) {
        err(ctx, "%s, %d sip %s is not valid ip address\n", __func__, i, args->sip[i]);
        return AVERROR(EINVAL);
      }
    }
    p->tx_queues_cnt[port] = args->tx_queues_cnt[i];
    p->rx_queues_cnt[port] = args->rx_queues_cnt[i];
    p->num_ports++;
  }

  p->flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
  p->flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
  p->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  p->flags |= MTL_FLAG_BIND_NUMA;
  p->log_level = MTL_LOG_LEVEL_INFO;

  if (args->ptp_enable) {
    p->flags |= MTL_FLAG_PTP_ENABLE;
    if (args->ptp_pi) p->flags |= MTL_FLAG_PTP_PI;
    if (args->ptp_unicast) p->flags |= MTL_FLAG_PTP_UNICAST_ADDR;
    info(ctx, "%s, PTP enabled (pi=%d unicast=%d)\n", __func__, args->ptp_pi,
         args->ptp_unicast);
  }

  if (args->ptp_pacing) p->pacing = ST21_TX_PACING_WAY_PTP;

  if (args->dma_dev) {
    char devs[128] = {0};
    char* next_dev;
    char* saveptr = NULL;
    size_t devs_len = strlen(args->dma_dev);

    if (devs_len >= sizeof(devs)) {
      err(ctx, "%s, dma device list is too long\n", __func__);
      return AVERROR(EINVAL);
    }
    snprintf(devs, sizeof(devs), "%s", args->dma_dev);
    if (!devs_len || (devs[0] == ',') || (devs[devs_len - 1] == ',') ||
        strstr(devs, ",,")) {
      err(ctx, "%s, dma device list contains an empty entry\n", __func__);
      return AVERROR(EINVAL);
    }

    next_dev = strtok_r(devs, ",", &saveptr);
    while (next_dev) {
      if (p->num_dma_dev_port >= MTL_DMA_DEV_MAX) {
        err(ctx, "%s, too many dma devices\n", __func__);
        return AVERROR(EINVAL);
      }
      if (strlen(next_dev) >= sizeof(p->dma_dev_port[p->num_dma_dev_port])) {
        err(ctx, "%s, dma device name is too long\n", __func__);
        return AVERROR(EINVAL);
      }
      info(ctx, "%s, append dma dev: %s\n", __func__, next_dev);
      snprintf(p->dma_dev_port[p->num_dma_dev_port],
               sizeof(p->dma_dev_port[p->num_dma_dev_port]), "%s", next_dev);
      p->num_dma_dev_port++;
      next_dev = strtok_r(NULL, ",", &saveptr);
    }
  }

  return 0;
}

static bool mtl_dev_params_compatible(const struct mtl_init_params* requested,
                                      const struct mtl_init_params* active) {
  struct mtl_init_params requested_resources = *requested;
  struct mtl_init_params active_resources = *active;
  uint64_t requested_ptp = requested->flags & MTL_FFMPEG_PTP_FLAGS;
  uint64_t active_ptp = active->flags & MTL_FFMPEG_PTP_FLAGS;

  if ((requested_ptp & MTL_FLAG_PTP_ENABLE) && (requested_ptp != active_ptp))
    return false;

  requested_resources.flags &= ~MTL_FFMPEG_PTP_FLAGS;
  active_resources.flags &= ~MTL_FFMPEG_PTP_FLAGS;
  return !memcmp(&requested_resources, &active_resources, sizeof(requested_resources));
}

mtl_handle mtl_dev_get(AVFormatContext* ctx, const struct StDevArgs* args, int* idx) {
  struct mtl_init_params p;
  mtl_handle handle;

  if (mtl_dev_build_params(ctx, args, &p) < 0) return NULL;

  pthread_mutex_lock(&g_mtl_lifecycle_mutex);
  if (g_mtl_shared_handle) {
    if (!mtl_dev_params_compatible(&p, &g_mtl_shared_params)) {
      err(ctx, "%s, shared handle configuration mismatch\n", __func__);
      pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
      return NULL;
    }
    *idx = g_mtl_ref_cnt;
    g_mtl_ref_cnt++;
    handle = g_mtl_shared_handle;
    info(ctx, "%s, shared handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
    pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
    return handle;
  }

  handle = mtl_init(&p);
  if (!handle) {
    err(ctx, "%s, mtl_init fail\n", __func__);
    pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
    return NULL;
  }

  g_mtl_shared_handle = handle;
  g_mtl_shared_params = p;
  *idx = 0;
  g_mtl_ref_cnt = 1;
  info(ctx, "%s, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
  return handle;
}

int mtl_instance_put(AVFormatContext* ctx, mtl_handle handle) {
  pthread_mutex_lock(&g_mtl_lifecycle_mutex);
  if (handle != g_mtl_shared_handle) {
    err(ctx, "%s, error handle %p %p\n", __func__, handle, g_mtl_shared_handle);
    pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
    return AVERROR(EIO);
  }

  g_mtl_ref_cnt--;
  info(ctx, "%s, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  if (g_mtl_ref_cnt <= 0) {
    info(ctx, "%s, ref cnt reach zero, uninit mtl device\n", __func__);
    mtl_uninit(handle);
    g_mtl_shared_handle = NULL;
    memset(&g_mtl_shared_params, 0, sizeof(g_mtl_shared_params));
  }

  pthread_mutex_unlock(&g_mtl_lifecycle_mutex);
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
