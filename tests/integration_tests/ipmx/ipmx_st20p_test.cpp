/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* RGB 4:4:4 8-bit st20p TX to RX with the IPMX flag on both sides, at the D6 rates. */

#ifdef MTL_HAS_IPMX

#include "st20p_test.h"

static void ipmx_st20p_rgb8_digest(enum st_fps fps, int width, int height) {
  enum st_fps fps_arr[1] = {fps};
  int width_arr[1] = {width};
  int height_arr[1] = {height};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_RGB8};
  enum st20_fmt t_fmt[1] = {ST20_FMT_RGB_8BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_RGB8};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.ipmx = true;

  st20p_rx_digest_test(fps_arr, width_arr, height_arr, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(Ipmx_st20p, rgb8_digest_1080p50) {
  ipmx_st20p_rgb8_digest(ST_FPS_P50, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_1080p59_94) {
  ipmx_st20p_rgb8_digest(ST_FPS_P59_94, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_1080p60) {
  ipmx_st20p_rgb8_digest(ST_FPS_P60, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_2160p59_94) {
  ipmx_st20p_rgb8_digest(ST_FPS_P59_94, 3840, 2160);
}

TEST(Ipmx_st20p, rgb8_digest_2160p60) {
  ipmx_st20p_rgb8_digest(ST_FPS_P60, 3840, 2160);
}

#endif
