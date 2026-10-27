/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* st20p TX to RX with the IPMX flag on both sides: RGB 4:4:4 8-bit and 10-bit at the D6
 * rates, and the TR-10-1 section 8.1 CMAX / VRX result of the RX timing parser. */

#ifdef MTL_HAS_IPMX

#include "ipmx/ipmx_api.h"
#include "log.h"
#include "st20p_test.h"

static void ipmx_st20p_digest(enum st_frame_fmt fmt, enum st20_fmt transport_fmt,
                              enum st_fps fps, int width, int height) {
  enum st_fps fps_arr[1] = {fps};
  int width_arr[1] = {width};
  int height_arr[1] = {height};
  enum st_frame_fmt tx_fmt[1] = {fmt};
  enum st20_fmt t_fmt[1] = {transport_fmt};
  enum st_frame_fmt rx_fmt[1] = {fmt};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.ipmx = true;

  st20p_rx_digest_test(fps_arr, width_arr, height_arr, tx_fmt, t_fmt, rx_fmt, &para);
}

static int32_t ipmx_tp_expect_cmax;

/* Plumbing only: the verdict is host and load sensitive with software RX times, as in
 * test_st20p_rx_check_tp(). The deterministic guard is tests/unit/ipmx and the strict
 * check NoCtxTest.st20p_ipmx_tp_*. */
static void ipmx_st20p_check_tp(st20p_rx_handle handle) {
  struct ipmx_rx_tp_stat stat;

  ASSERT_EQ(ipmx_st20p_rx_get_tp_stat(handle, MTL_SESSION_PORT_P, &stat), 0);
  uint32_t measured = stat.frames_compliant + stat.frames_failed;
  info(
      "%s, compliant %u failed %u unmeasured %u cinst max %d cmax %d vrx min %d max %d "
      "full %d, last failed cause %s\n",
      __func__, stat.frames_compliant, stat.frames_failed, stat.frames_unmeasured,
      stat.cinst_max, stat.cmax, stat.vrx_min, stat.vrx_max, stat.vrx_full,
      stat.failed_cause);
  EXPECT_EQ(stat.cmax, ipmx_tp_expect_cmax);
  EXPECT_EQ(stat.vrx_full, ipmx_tp_expect_cmax * 2);
  EXPECT_GT(measured, 0u);
  EXPECT_GE(measured * 2, measured + stat.frames_unmeasured);
}

static void ipmx_st20p_tp(enum st_frame_fmt fmt, enum st20_fmt transport_fmt,
                          enum st_fps fps, int width, int height, int32_t cmax) {
  enum st_fps fps_arr[1] = {fps};
  int width_arr[1] = {width};
  int height_arr[1] = {height};
  enum st_frame_fmt tx_fmt[1] = {fmt};
  enum st20_fmt t_fmt[1] = {transport_fmt};
  enum st_frame_fmt rx_fmt[1] = {fmt};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.ipmx = true;
  para.rx_timing_parser = true;
  para.rx_check = ipmx_st20p_check_tp;
  ipmx_tp_expect_cmax = cmax;

  st20p_rx_digest_test(fps_arr, width_arr, height_arr, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(Ipmx_st20p, rgb8_digest_1080p50) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P50, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_1080p59_94) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P59_94, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_1080p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080);
}

TEST(Ipmx_st20p, rgb8_digest_2160p59_94) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P59_94, 3840, 2160);
}

TEST(Ipmx_st20p, rgb8_digest_2160p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 3840, 2160);
}

TEST(Ipmx_st20p, rgb10_digest_1080p50) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P50, 1920,
                    1080);
}

TEST(Ipmx_st20p, rgb10_digest_1080p59_94) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P59_94,
                    1920, 1080);
}

TEST(Ipmx_st20p, rgb10_digest_1080p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P60, 1920,
                    1080);
}

TEST(Ipmx_st20p, rgb10_digest_2160p59_94) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P59_94,
                    3840, 2160);
}

TEST(Ipmx_st20p, rgb10_digest_2160p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P60, 3840,
                    2160);
}

TEST(Ipmx_st20p, yuv422p10_tp_1080p60) {
  ipmx_st20p_tp(ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, ST_FPS_P60,
                1920, 1080, 16);
}

TEST(Ipmx_st20p, yuv422p10_tp_2160p60) {
  ipmx_st20p_tp(ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, ST_FPS_P60,
                3840, 2160, 45);
}

TEST(Ipmx_st20p, rgb10_tp_1080p60) {
  ipmx_st20p_tp(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P60, 1920,
                1080, 17);
}

TEST(Ipmx_st20p, rgb10_tp_2160p60) {
  ipmx_st20p_tp(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P60, 3840,
                2160, 68);
}

#endif
