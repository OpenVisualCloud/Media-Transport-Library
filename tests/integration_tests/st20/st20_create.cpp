/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"
#include "tests.hpp"

TEST(St20_tx, create_free_single) {
  create_free_test(st20_tx, 0, 1, 1);
}
TEST(St20_tx, create_free_multi) {
  create_free_test(st20_tx, 0, 1, 6);
}
TEST(St20_tx, create_free_mix) {
  create_free_test(st20_tx, 2, 3, 4);
}
TEST(St20_tx, create_free_max) {
  create_free_max(st20_tx, TEST_CREATE_FREE_MAX);
}
TEST(St20_tx, create_expect_fail) {
  expect_fail_test(st20_tx);
}
TEST(St20_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
}
TEST(St20_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
}
TEST(St20_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}

TEST(St20_rx, create_free_single) {
  create_free_test(st20_rx, 0, 1, 1);
}
TEST(St20_rx, create_free_multi) {
  create_free_test(st20_rx, 0, 1, 6);
}
TEST(St20_rx, create_free_mix) {
  create_free_test(st20_rx, 2, 3, 4);
}
TEST(St20_rx, create_free_max) {
  create_free_max(st20_rx, TEST_CREATE_FREE_MAX);
}
TEST(St20_rx, create_expect_fail) {
  expect_fail_test(st20_rx);
}
TEST(St20_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 0;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
}
TEST(St20_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
}
