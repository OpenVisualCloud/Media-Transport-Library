/*
 * Kahawai common struct and functions
 * Copyright (c) 2023 Intel
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

#include "kahawai_common.h"

#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>

#include "libavutil/common.h"

static mtl_handle shared_st_handle = NULL;
unsigned int active_session_cnt = 0;
static struct mtl_init_params param = {0};

enum st_fps kahawai_fps_to_st_fps(AVRational framerate) {
  double fps = (double)framerate.num / (double)framerate.den;

  return st_frame_rate_to_st_fps(fps);
}

mtl_handle kahawai_init(char* port, char* local_addr, int enc_session_cnt,
                        int dec_session_cnt, char* dma_dev) {
  param.num_ports = 1;

  strncpy(param.port[MTL_PORT_P], port, MTL_PORT_MAX_LEN);

  if (NULL == local_addr) {
    av_log(NULL, AV_LOG_ERROR, "Invalid local IP address\n");
    return NULL;
  } else if (sscanf(local_addr, "%hhu.%hhu.%hhu.%hhu", &param.sip_addr[MTL_PORT_P][0],
                    &param.sip_addr[MTL_PORT_P][1], &param.sip_addr[MTL_PORT_P][2],
                    &param.sip_addr[MTL_PORT_P][3]) != MTL_IP_ADDR_LEN) {
    av_log(NULL, AV_LOG_ERROR, "Failed to parse local IP address: %s\n", local_addr);
    return NULL;
  }

  if (enc_session_cnt > 0) param.tx_sessions_cnt_max = enc_session_cnt;
  if (dec_session_cnt > 0) param.rx_sessions_cnt_max = dec_session_cnt;
  param.flags = MTL_FLAG_BIND_NUMA | MTL_FLAG_DEV_AUTO_START_STOP;
  param.log_level = MTL_LOG_LEVEL_DEBUG;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                      // usr crx pointer
  param.ptp_get_time_fn = NULL;
  param.lcores = NULL;

  if (dma_dev) {
    param.num_dma_dev_port = 1;
    strncpy(param.dma_dev_port[0], dma_dev, MTL_PORT_MAX_LEN);
    av_log(NULL, AV_LOG_VERBOSE, "DMA enabled on %s\n", dma_dev);
  }

  return mtl_init(&param);
}

mtl_handle kahawai_get_handle() { return shared_st_handle; }

void kahawai_set_handle(mtl_handle handle) { shared_st_handle = handle; }