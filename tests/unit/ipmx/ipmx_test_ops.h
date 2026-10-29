/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "st20_api.h"

namespace ut_ipmx {

constexpr uint16_t kIpmxPort = 20000;

inline int TxNextFrame(void*, uint16_t*, struct st20_tx_frame_meta*) {
  return -EBUSY;
}
inline int TxLinesReady(void*, uint16_t, struct st20_tx_slice_meta*) {
  return -EBUSY;
}
inline int TxRtpDone(void*) {
  return 0;
}
inline int RxFrameReady(void*, void*, struct st20_rx_frame_meta*) {
  return 0;
}
inline int RxSliceReady(void*, void*, struct st20_rx_slice_meta*) {
  return 0;
}
inline int RxRtpReady(void*) {
  return 0;
}

/* 1080p59.94 YUV 4:2:2 10-bit frame level, one port, IPMX on, passes tv_ops_check() */
inline struct st20_tx_ops TxOps(int num_port = 1) {
  struct st20_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = "ipmx_tx";
  ops.num_port = num_port;
  for (int i = 0; i < num_port; i++) {
    uint8_t ip[MTL_IP_ADDR_LEN] = {239, 0, 0, static_cast<uint8_t>(1 + i)};
    memcpy(ops.dip_addr[i], ip, sizeof(ip));
    ops.udp_port[i] = kIpmxPort;
  }
  ops.flags = ST20_TX_FLAG_IPMX;
  ops.pacing = ST21_PACING_NARROW;
  ops.packing = ST20_PACKING_BPM;
  ops.type = ST20_TYPE_FRAME_LEVEL;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.payload_type = 96;
  ops.framebuff_cnt = 2;
  ops.get_next_frame = TxNextFrame;
  ops.query_frame_lines_ready = TxLinesReady;
  ops.rtp_ring_size = 1024;
  ops.rtp_frame_total_pkts = 4320;
  ops.rtp_pkt_size = 1200;
  ops.notify_rtp_done = TxRtpDone;
  return ops;
}

/* RX counterpart of TxOps(), passes rv_ops_check() */
inline struct st20_rx_ops RxOps(int num_port = 1) {
  struct st20_rx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = "ipmx_rx";
  ops.num_port = num_port;
  for (int i = 0; i < num_port; i++) {
    uint8_t ip[MTL_IP_ADDR_LEN] = {239, 0, 0, static_cast<uint8_t>(1 + i)};
    memcpy(ops.ip_addr[i], ip, sizeof(ip));
    ops.udp_port[i] = kIpmxPort;
  }
  ops.flags = ST20_RX_FLAG_IPMX;
  ops.pacing = ST21_PACING_NARROW;
  ops.packing = ST20_PACKING_BPM;
  ops.type = ST20_TYPE_FRAME_LEVEL;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.payload_type = 96;
  ops.framebuff_cnt = 2;
  ops.notify_frame_ready = RxFrameReady;
  ops.notify_slice_ready = RxSliceReady;
  ops.rtp_ring_size = 1024;
  ops.notify_rtp_ready = RxRtpReady;
  return ops;
}

struct IpmxFormat {
  const char* name;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  enum st20_fmt fmt;
  enum st20_type type;
  enum st20_packing packing;
};

/* progressive frame and slice level, BPM / GPM / GPM_SL */
const IpmxFormat kIpmxFormats[] = {
    {"Yuv422p10_1080p5994_Frame_Bpm", 1920, 1080, ST_FPS_P59_94, ST20_FMT_YUV_422_10BIT,
     ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM},
    {"Yuv422p10_1080p5994_Slice_Gpm", 1920, 1080, ST_FPS_P59_94, ST20_FMT_YUV_422_10BIT,
     ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM},
    {"Rgb8_1080p60_Frame_Gpm", 1920, 1080, ST_FPS_P60, ST20_FMT_RGB_8BIT,
     ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM},
    {"Rgb8_1080p60_Slice_Bpm", 1920, 1080, ST_FPS_P60, ST20_FMT_RGB_8BIT,
     ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM},
    {"Rgb10_2160p60_Frame_Bpm", 3840, 2160, ST_FPS_P60, ST20_FMT_RGB_10BIT,
     ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM},
    {"Rgb10_2160p60_Slice_GpmSl", 3840, 2160, ST_FPS_P60, ST20_FMT_RGB_10BIT,
     ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL},
};

inline int CountOf(const std::string& log, const std::string& what) {
  int n = 0;
  for (size_t pos = log.find(what); pos != std::string::npos;
       pos = log.find(what, pos + 1))
    n++;
  return n;
}

constexpr const char* kPortWarning = "recommends above 5000";

inline std::string FormatName(const ::testing::TestParamInfo<IpmxFormat>& info) {
  return info.param.name;
}

}  // namespace ut_ipmx
