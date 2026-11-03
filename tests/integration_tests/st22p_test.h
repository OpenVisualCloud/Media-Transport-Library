/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* The st22p TX to RX SHA digest test, shared with the IPMX st22p cases in ipmx/. */

#pragma once

#include "tests.hpp"

struct st22p_rx_digest_test_para {
  int sessions;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
  int rand_ratio;
  bool check_fps;
  enum st_test_level level;
  bool user_timestamp;
  bool vsync;
  bool rtcp;
  bool tx_ext;
  bool rx_ext;
  bool interlace;
  uint32_t ssrc;
  bool block_get;
  bool codec_block_get;
  bool derive;
  bool ipmx;
  enum st_plugin_device device;
  bool no_digest; /* a real, lossy codec: RX checks no SHA */
};

void test_st22p_init_rx_digest_para(struct st22p_rx_digest_test_para* para);

void st22p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                          enum st_frame_fmt fmt[], enum st22_codec codec[],
                          int compress_ratio[], struct st22p_rx_digest_test_para* para);
