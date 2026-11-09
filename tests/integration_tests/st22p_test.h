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
  /* the IPMX flag on RX only: a plain ST 2110 sender into an IPMX receiver */
  bool ipmx_rx_only;
  bool redundant; /* ST 2022-7: TX on ports P and R, RX on ports 2 and 3 */
  enum st_plugin_device device;
  bool no_digest; /* a real, lossy codec: RX checks no SHA */
  int duration_s; /* streaming time, 0 is 10 s */
  /* RX framebuffers, 0 is ST22_TEST_SHA_HIST_NUM; each widens the IPMX frame budget */
  int rx_fb_cnt;
  /* fills each TX framebuffer instead of random data and its SHA; needs no_digest */
  void (*tx_fill)(uint8_t* fb, uint32_t width, uint32_t height);
  /* called on each RX frame before st22p_rx_put_frame() */
  void (*rx_frame)(struct st_frame* frame);
  /* called halfway through the stream with every TX and RX session */
  void (*mid_stream)(st22p_tx_handle* tx, st22p_rx_handle* rx, int sessions);
  /* called on each TX session after mtl_stop(), before st22p_tx_free() */
  void (*tx_check)(st22p_tx_handle handle);
  /* called on each RX session after mtl_stop(), before st22p_rx_free() */
  void (*rx_check)(st22p_rx_handle handle);
};

void test_st22p_init_rx_digest_para(struct st22p_rx_digest_test_para* para);

void st22p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                          enum st_frame_fmt fmt[], enum st22_codec codec[],
                          int compress_ratio[], struct st22p_rx_digest_test_para* para);
