/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include "tests.hpp"

struct st20p_rx_digest_test_para {
  enum st_plugin_device device;
  int sessions;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
  bool tx_ext;
  bool rx_ext;
  bool rx_dedicated_ext;
  bool check_fps;
  enum st_test_level level;
  int fb_cnt;
  bool user_timestamp;
  bool vsync;
  bool pkt_convert;
  size_t line_padding_size;
  bool send_done_check;
  bool interlace;
  bool user_meta;
  bool rtcp;
  enum st20_packing packing;
  enum st21_pacing pacing;
  uint32_t ssrc;
  bool block_get;
  bool rx_timing_parser;
  bool rx_auto_detect;
  bool zero_payload_type;
  bool ipmx;
  /* TX on ports P and R, RX on ports 2 and 3, all multicast; needs 4 ports */
  bool redundant;
  /* streaming time, 0 for 10 s */
  int duration_s;
  /* called halfway through the streaming time with every TX and RX session */
  void (*mid_stream)(st20p_tx_handle* tx, st20p_rx_handle* rx, int sessions);
  /* called on each RX session after mtl_stop(), before st20p_rx_free() */
  void (*rx_check)(st20p_rx_handle handle);
};

void test_st20p_init_rx_digest_para(struct st20p_rx_digest_test_para* para);

void st20p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                          enum st_frame_fmt tx_fmt[], enum st20_fmt t_fmt[],
                          enum st_frame_fmt rx_fmt[],
                          struct st20p_rx_digest_test_para* para);
