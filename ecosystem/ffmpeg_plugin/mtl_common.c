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

enum st_fps framerate_to_st_fps(AVRational framerate) {
  double fps = (double)framerate.num / (double)framerate.den;

  return st_frame_rate_to_st_fps(fps);
}

mtl_handle mtl_instance_get(char* port, char* local_addr, int enc_session_cnt,
                            int dec_session_cnt, char* dma_dev, int* idx) {
  struct mtl_init_params p;
  mtl_handle handle = NULL;

  if (g_mtl_shared_handle) {
    *idx = g_mtl_ref_cnt;
    g_mtl_ref_cnt++;
    info(NULL, "%s, reuse shared, handle %p ref cnt %d\n", __func__, g_mtl_shared_handle,
         g_mtl_ref_cnt);
    return g_mtl_shared_handle;
  }

  memset(&p, 0, sizeof(p));

  p.num_ports = 1;
  snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port);
  p.pmd[MTL_PORT_P] = mtl_pmd_by_port_name(p.port[MTL_PORT_P]);

  if (p.pmd[MTL_PORT_P] == MTL_PMD_DPDK_USER) {
    /* check ip for dpdk based pmd */
    if (NULL == local_addr) {
      err(NULL, "%s, NULL local IP address\n", __func__);
      return NULL;
    } else if (sscanf(local_addr, "%hhu.%hhu.%hhu.%hhu", &p.sip_addr[MTL_PORT_P][0],
                      &p.sip_addr[MTL_PORT_P][1], &p.sip_addr[MTL_PORT_P][2],
                      &p.sip_addr[MTL_PORT_P][3]) != MTL_IP_ADDR_LEN) {
      err(NULL, "%s, failed to parse local IP address: %s\n", __func__, local_addr);
      return NULL;
    }
  }

  if (enc_session_cnt > 0) {
    p.tx_queues_cnt[MTL_PORT_P] = enc_session_cnt;
    p.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
  }
  if (dec_session_cnt > 0) {
    p.rx_queues_cnt[MTL_PORT_P] = dec_session_cnt;
    p.flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
    p.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  }
  p.flags |= MTL_FLAG_BIND_NUMA;
  p.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  p.log_level = MTL_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING

  if (dma_dev) {
    p.num_dma_dev_port = 1;
    snprintf(p.dma_dev_port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", dma_dev);
    info(NULL, "%s, DMA enabled on %s\n", __func__, dma_dev);
  }

  handle = mtl_init(&p);
  if (!handle) {
    err(NULL, "%s, mtl_init fail\n", __func__);
    return NULL;
  }

  g_mtl_shared_handle = handle;
  *idx = 0;
  g_mtl_ref_cnt++;
  info(NULL, "%s, get succ, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  return handle;
}

int mtl_instance_put(mtl_handle handle) {
  if (handle != g_mtl_shared_handle) {
    err(NULL, "%s, error handle %p %p\n", __func__, handle, g_mtl_shared_handle);
    return AVERROR(EIO);
  }

  g_mtl_ref_cnt--;
  info(NULL, "%s, put succ, handle %p ref cnt %d\n", __func__, handle, g_mtl_ref_cnt);
  if (g_mtl_ref_cnt <= 0) {
    info(NULL, "%s, ref cnt reach zero, uninit mtl instance\n", __func__);
    mtl_uninit(handle);
    g_mtl_shared_handle = NULL;
  }

  return 0;
}