/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_MT_HEAD_H_
#define _MT_LIB_MT_HEAD_H_

enum mt_handle_type {
  MT_HANDLE_UNKNOWN = 0,
  MT_HANDLE_MAIN = 1,

  MT_HANDLE_TX_VIDEO = 8,
  MT_HANDLE_TX_AUDIO = 9,
  MT_HANDLE_TX_ANC = 10,
  MT_HANDLE_RX_VIDEO = 11,
  MT_HANDLE_RX_AUDIO = 12,
  MT_HANDLE_RX_ANC = 13,
  MT_HANDLE_RX_VIDEO_R = 14,
  MT_ST22_HANDLE_TX_VIDEO = 15,
  MT_ST22_HANDLE_RX_VIDEO = 16,
  MT_ST22_HANDLE_PIPELINE_TX = 20,
  MT_ST22_HANDLE_PIPELINE_RX = 21,
  MT_ST22_HANDLE_PIPELINE_ENCODE = 22,
  MT_ST22_HANDLE_PIPELINE_DECODE = 23,
  MT_ST20_HANDLE_PIPELINE_TX = 24,
  MT_ST20_HANDLE_PIPELINE_RX = 25,
  MT_ST20_HANDLE_PIPELINE_CONVERT = 26,
  MT_ST22_HANDLE_DEV_ENCODE = 27,
  MT_ST22_HANDLE_DEV_DECODE = 28,
  MT_ST20_HANDLE_DEV_CONVERT = 29,
  MT_ST30_HANDLE_PIPELINE_TX = 30,
  MT_ST30_HANDLE_PIPELINE_RX = 31,

  MT_HANDLE_UDMA = 40,
  MT_HANDLE_UDP = 41,

  MT_HANDLE_MAX,
};

/* total size: 42 */
struct mt_udp_hdr {
  struct rte_ether_hdr eth; /* size: 14 */
  struct rte_ipv4_hdr ipv4; /* size: 20 */
  struct rte_udp_hdr udp;   /* size: 8 */
} __attribute__((__packed__)) __rte_aligned(2);

struct mt_stat_u64 {
  uint64_t max;
  uint64_t min;
  uint64_t cnt;
  uint64_t sum;
};

struct mt_rx_pcap {
  struct mt_pcap *pcap;
  uint32_t dumped_pkts;
  uint32_t dropped_pkts;
  uint32_t required_pkts;
  char file_name[MTL_PCAP_FILE_MAX_LEN];
  bool usdt_dump;
};

#endif
