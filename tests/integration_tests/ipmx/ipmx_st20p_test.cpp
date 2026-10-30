/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* st20p TX to RX with the IPMX flag on both sides: RGB 4:4:4 8-bit and 10-bit at the D6
 * rates, the TR-10-1 section 8.1 CMAX / VRX result of the RX timing parser, and the
 * session life cycle with the per-frame Sender Report on: a destination update, several
 * sessions, ST 2022-7, soaks, the block version, the ts-refclk, and the Sender Reports
 * as the RX reads them back (the sr_ cases). The noipmx_ cases are the same streams
 * without the flag, for wire comparison. Create/free cycles are
 * NoCtxTest.st20p_*ipmx_create_free_cycles, which need an instance without
 * MTL_FLAG_RANDOM_SRC_PORT. */

#ifdef MTL_HAS_IPMX

#include "ipmx/ipmx_api.h"
#include "log.h"
#include "st20p_test.h"

#define IPMX_TEST_SESSIONS_MAX (4)

static void ipmx_st20p_digest_para(enum st_frame_fmt fmt, enum st20_fmt transport_fmt,
                                   enum st_fps fps, int width, int height,
                                   struct st20p_rx_digest_test_para* para) {
  enum st_fps fps_arr[IPMX_TEST_SESSIONS_MAX];
  int width_arr[IPMX_TEST_SESSIONS_MAX];
  int height_arr[IPMX_TEST_SESSIONS_MAX];
  enum st_frame_fmt tx_fmt[IPMX_TEST_SESSIONS_MAX];
  enum st20_fmt t_fmt[IPMX_TEST_SESSIONS_MAX];

  for (int i = 0; i < IPMX_TEST_SESSIONS_MAX; i++) {
    fps_arr[i] = fps;
    width_arr[i] = width;
    height_arr[i] = height;
    tx_fmt[i] = fmt;
    t_fmt[i] = transport_fmt;
  }
  para->device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  st20p_rx_digest_test(fps_arr, width_arr, height_arr, tx_fmt, t_fmt, tx_fmt, para);
}

static void ipmx_st20p_digest(enum st_frame_fmt fmt, enum st20_fmt transport_fmt,
                              enum st_fps fps, int width, int height, bool ipmx = true) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = ipmx;
  ipmx_st20p_digest_para(fmt, transport_fmt, fps, width, height, &para);
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

TEST(Ipmx_st20p, yuv422p10_digest_1080p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, ST_FPS_P60,
                    1920, 1080);
}

TEST(Ipmx_st20p, noipmx_rgb8_digest_1080p59_94) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P59_94, 1920, 1080,
                    false);
}

TEST(Ipmx_st20p, noipmx_rgb8_digest_2160p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 3840, 2160, false);
}

TEST(Ipmx_st20p, noipmx_yuv422p10_digest_1080p60) {
  ipmx_st20p_digest(ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, ST_FPS_P60,
                    1920, 1080, false);
}

TEST(Ipmx_st20p, rgb8_digest_1080p60_s4) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.sessions = 4;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

TEST(Ipmx_st20p, rgb8_digest_1080p60_redundant) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  if (ctx->para.num_ports < 4)
    GTEST_SKIP() << "ST 2022-7 needs 4 ports, have " << ctx->para.num_ports;

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.redundant = true;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

#define IPMX_TEST_UPDATED_UDP_PORT (20100)

static uint64_t ipmx_rx_frames_before_update[IPMX_TEST_SESSIONS_MAX];
static st20p_rx_handle ipmx_rx_updated[IPMX_TEST_SESSIONS_MAX];

static uint64_t ipmx_st20p_rx_frames(st20p_rx_handle handle) {
  struct st20_rx_user_stats stats;

  if (st20p_rx_get_session_stats(handle, &stats) < 0) return 0;
  return stats.common.stat_frames_received;
}

/* both ends move to the R multicast group and another port */
static void ipmx_st20p_update_destination(st20p_tx_handle* tx, st20p_rx_handle* rx,
                                          int sessions) {
  auto ctx = (struct st_tests_context*)st_test_ctx();

  for (int i = 0; i < sessions; i++) {
    struct st_tx_dest_info dst;
    struct st_rx_source_info src;
    uint16_t udp_port = IPMX_TEST_UPDATED_UDP_PORT + i * 2;

    memset(&dst, 0, sizeof(dst));
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memset(&src, 0, sizeof(src));
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;

    EXPECT_EQ(st20p_rx_update_source(rx[i], &src), 0);
    EXPECT_EQ(st20p_tx_update_destination(tx[i], &dst), 0);
    ipmx_rx_updated[i] = rx[i];
    ipmx_rx_frames_before_update[i] = ipmx_st20p_rx_frames(rx[i]);
  }
}

/* the half after the update at 60 fps, 5 s, delivers well over one second of frames */
static void ipmx_st20p_check_rx_after_update(st20p_rx_handle handle) {
  for (int i = 0; i < IPMX_TEST_SESSIONS_MAX; i++) {
    if (ipmx_rx_updated[i] != handle) continue;
    uint64_t after = ipmx_st20p_rx_frames(handle);
    info("%s(%d), frames %" PRIu64 " before the update, %" PRIu64 " at the end\n",
         __func__, i, ipmx_rx_frames_before_update[i], after);
    EXPECT_GT(after, ipmx_rx_frames_before_update[i] + 60);
    return;
  }
  ADD_FAILURE() << "rx session was not updated";
}

#define IPMX_TEST_BLOCK_VERSION (5)

/* API plumbing only, rgb8_digest_1080p60_sr_block_version reads the version back on RX */
static void ipmx_st20p_set_block_version(st20p_tx_handle handle) {
  struct ipmx_video_info info;

  ASSERT_EQ(ipmx_st20p_tx_set_block_version(handle, IPMX_TEST_BLOCK_VERSION), 0);
  ASSERT_EQ(ipmx_st20p_tx_get_info(handle, &info), 0);
  EXPECT_EQ(info.version, IPMX_TEST_BLOCK_VERSION);
  EXPECT_STREQ(info.sampling, "RGB");
  EXPECT_EQ(info.depth, 8);
  EXPECT_EQ(info.width, 1920);
  EXPECT_EQ(info.height, 1080);
  EXPECT_EQ(ipmx_st20p_tx_get_info(handle, NULL), -EINVAL);
}

TEST(Ipmx_st20p, rgb8_digest_1080p60_block_version) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.tx_created = ipmx_st20p_set_block_version;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

static void ipmx_st20p_no_block_version(st20p_tx_handle handle) {
  struct ipmx_video_info info;

  EXPECT_EQ(ipmx_st20p_tx_set_block_version(handle, IPMX_TEST_BLOCK_VERSION), -EIO);
  EXPECT_EQ(ipmx_st20p_tx_get_info(handle, &info), -EIO);
}

TEST(Ipmx_st20p, noipmx_rgb8_digest_1080p60_block_version) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.tx_created = ipmx_st20p_no_block_version;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

/* what an app with its own PTP time builds from pmc PARENT_DATA_SET grandmasterIdentity
 * 444988.fffe.0695e0 and the domain */
static const char* const ipmx_ts_refclk_domain127 =
    "ptp=IEEE1588-2008:44-49-88-FF-FE-06-95-E0:127";
static const char* const ipmx_ts_refclk_domain0 =
    "ptp=IEEE1588-2008:44-49-88-FF-FE-06-95-E0:0";

static void ipmx_st20p_set_ts_refclk(st20p_tx_handle handle) {
  ASSERT_EQ(ipmx_st20p_tx_set_ts_refclk(handle, ipmx_ts_refclk_domain127), 0);
}

static void ipmx_st20p_expect_ts_refclk(st20p_tx_handle handle, const char* ts_refclk,
                                        uint8_t version) {
  struct ipmx_video_info info;

  ASSERT_EQ(ipmx_st20p_tx_get_info(handle, &info), 0);
  EXPECT_STREQ(info.ts_refclk, ts_refclk);
  EXPECT_EQ(info.version, version);
}

/* the first SR took the string set before the first frame without a bump; a new one
 * bumps the version in the SR that first carries it */
static void ipmx_st20p_change_ts_refclk(st20p_tx_handle* tx, st20p_rx_handle*,
                                        int sessions) {
  for (int i = 0; i < sessions; i++) {
    ipmx_st20p_expect_ts_refclk(tx[i], ipmx_ts_refclk_domain127, 0);
    EXPECT_EQ(ipmx_st20p_tx_set_ts_refclk(tx[i], ipmx_ts_refclk_domain0), 0);
  }
}

static void ipmx_st20p_check_ts_refclk_changed(st20p_tx_handle handle) {
  ipmx_st20p_expect_ts_refclk(handle, ipmx_ts_refclk_domain0, 1);
}

TEST(Ipmx_st20p, rgb8_digest_1080p59_94_ts_refclk) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.tx_created = ipmx_st20p_set_ts_refclk;
  para.mid_stream = ipmx_st20p_change_ts_refclk;
  para.tx_check = ipmx_st20p_check_ts_refclk_changed;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P59_94, 1920, 1080,
                         &para);
}

/* ---- the RX side of the Sender Report, ipmx_st20p_rx_get_sr_info() ---- */

static struct ipmx_video_info ipmx_sr_tx_info;
static bool ipmx_sr_tx_info_valid;
static uint8_t ipmx_sr_expect_version_changes;

static void ipmx_st20p_sr_save_tx_info(st20p_tx_handle handle) {
  ipmx_sr_tx_info_valid = ipmx_st20p_tx_get_info(handle, &ipmx_sr_tx_info) == 0;
  EXPECT_TRUE(ipmx_sr_tx_info_valid);
}

/* the last SR of one port against the TX info: Info Block, SSRC check, RTP and PTP
 * timestamps of the same frame (TR-10-1 section 8.7); counts are plumbing only */
static void ipmx_st20p_sr_check_port(st20p_rx_handle handle, enum mtl_session_port port,
                                     uint64_t frames_per_sr_max = 2) {
  struct ipmx_rx_sr_info sr;
  struct st20_rx_user_stats stats;

  ASSERT_EQ(ipmx_st20p_rx_get_sr_info(handle, port, &sr), 0) << port;
  ASSERT_EQ(st20p_rx_get_session_stats(handle, &stats), 0);
  info("%s(%d), received %" PRIu64 " malformed %" PRIu64 " no info %" PRIu64
       " ssrc mismatch %" PRIu64 " version changes %" PRIu64 " frames %" PRIu64
       ", ssrc %u rtp %u ptp %u.%09u cname %s version %u\n",
       __func__, port, sr.received, sr.malformed, sr.no_info_block, sr.ssrc_mismatch,
       sr.version_changes, stats.common.stat_frames_received, sr.ssrc, sr.rtp_timestamp,
       sr.ptp_sec, sr.ptp_ns, sr.cname, sr.info.version);
  EXPECT_GT(sr.received * frames_per_sr_max, stats.common.stat_frames_received);
  EXPECT_EQ(sr.malformed, 0u);
  EXPECT_EQ(sr.info_malformed, 0u);
  EXPECT_EQ(sr.no_info_block, 0u);
  EXPECT_EQ(sr.ssrc_mismatch, 0u);
  EXPECT_EQ(sr.unknown_mib, 0u);
  EXPECT_EQ(sr.version_changes, ipmx_sr_expect_version_changes);
  EXPECT_GT(sr.pkt_count, 0u);
  EXPECT_GT(sr.octet_count, 0u);
  EXPECT_GT(sr.arrival_ns, 0u);
  EXPECT_GT(strlen(sr.cname), 0u);
  uint64_t ptp_ns = (uint64_t)sr.ptp_sec * NS_PER_S + sr.ptp_ns;
  EXPECT_EQ((uint32_t)((__uint128_t)ptp_ns * 90000 / NS_PER_S), sr.rtp_timestamp);

  ASSERT_TRUE(sr.has_info);
  ASSERT_TRUE(sr.has_video_mib);
  ASSERT_TRUE(ipmx_sr_tx_info_valid);
  const struct ipmx_video_info* tx = &ipmx_sr_tx_info;
  EXPECT_EQ(sr.info.version, tx->version);
  EXPECT_EQ(0, memcmp(sr.info.ts_refclk, tx->ts_refclk, sizeof(tx->ts_refclk)));
  EXPECT_EQ(0, memcmp(sr.info.mediaclk, tx->mediaclk, sizeof(tx->mediaclk)));
  EXPECT_EQ(0, memcmp(sr.info.sampling, tx->sampling, sizeof(tx->sampling)));
  EXPECT_EQ(sr.info.depth, tx->depth);
  EXPECT_EQ(sr.info.gpm, tx->gpm);
  EXPECT_EQ(sr.info.interlaced, tx->interlaced);
  EXPECT_EQ(0, memcmp(sr.info.range, tx->range, sizeof(tx->range)));
  EXPECT_EQ(0, memcmp(sr.info.colorimetry, tx->colorimetry, sizeof(tx->colorimetry)));
  EXPECT_EQ(0, memcmp(sr.info.tcs, tx->tcs, sizeof(tx->tcs)));
  EXPECT_EQ(sr.info.width, tx->width);
  EXPECT_EQ(sr.info.height, tx->height);
  EXPECT_EQ(sr.info.rate_num, tx->rate_num);
  EXPECT_EQ(sr.info.rate_den, tx->rate_den);
  EXPECT_EQ(sr.info.pixclk, tx->pixclk);
  EXPECT_EQ(sr.info.htotal, tx->htotal);
  EXPECT_EQ(sr.info.vtotal, tx->vtotal);
}

static void ipmx_st20p_sr_check(st20p_rx_handle handle) {
  ipmx_st20p_sr_check_port(handle, MTL_SESSION_PORT_P);
}

static void ipmx_st20p_sr_check_redundant(st20p_rx_handle handle) {
  ipmx_st20p_sr_check_port(handle, MTL_SESSION_PORT_P);
  ipmx_st20p_sr_check_port(handle, MTL_SESSION_PORT_R);
}

static void ipmx_st20p_sr_para(struct st20p_rx_digest_test_para* para) {
  test_st20p_init_rx_digest_para(para);
  para->ipmx = true;
  para->tx_check = ipmx_st20p_sr_save_tx_info;
  para->rx_check = ipmx_st20p_sr_check;
  ipmx_sr_tx_info_valid = false;
  ipmx_sr_expect_version_changes = 0;
}

/* TX to RX: the SR the RX reads back carries what the TX reports */
TEST(Ipmx_st20p, rgb8_digest_1080p60_sr) {
  struct st20p_rx_digest_test_para para;
  ipmx_st20p_sr_para(&para);
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

static void ipmx_st20p_check_rx_and_sr_after_update(st20p_rx_handle handle) {
  ipmx_st20p_check_rx_after_update(handle);
  /* the source update halfway restarts the SR counters */
  ipmx_st20p_sr_check_port(handle, MTL_SESSION_PORT_P, 4);
}

/* both ends move to another group and port; the RX re-creates its SR queue and reads the
 * SRs of the new destination */
TEST(Ipmx_st20p, rgb8_digest_1080p60_update_destination) {
  struct st20p_rx_digest_test_para para;
  ipmx_st20p_sr_para(&para);
  para.check_fps = false; /* the frames lost in the switch */
  para.mid_stream = ipmx_st20p_update_destination;
  para.rx_check = ipmx_st20p_check_rx_and_sr_after_update;
  memset(ipmx_rx_updated, 0, sizeof(ipmx_rx_updated));
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

static void ipmx_st20p_bump_block_version(st20p_tx_handle* tx, st20p_rx_handle*,
                                          int sessions) {
  for (int i = 0; i < sessions; i++)
    EXPECT_EQ(ipmx_st20p_tx_set_block_version(tx[i], IPMX_TEST_BLOCK_VERSION + 1), 0);
}

/* a block version set mid stream reaches the RX, which counts the change */
TEST(Ipmx_st20p, rgb8_digest_1080p60_sr_block_version) {
  struct st20p_rx_digest_test_para para;
  ipmx_st20p_sr_para(&para);
  para.tx_created = ipmx_st20p_set_block_version;
  para.mid_stream = ipmx_st20p_bump_block_version;
  ipmx_sr_expect_version_changes = 1;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

static void ipmx_st20p_sr_none(st20p_rx_handle handle) {
  struct ipmx_rx_sr_info sr;

  memset(&sr, 0xee, sizeof(sr));

  EXPECT_EQ(ipmx_st20p_rx_get_sr_info(handle, MTL_SESSION_PORT_P, &sr), -EBUSY);
  EXPECT_EQ(sr.received, 0u);
  EXPECT_EQ(sr.malformed, 0u);
}

/* TP-1 section 14.3.3: an IPMX receiver takes a plain ST 2110 sender, without SRs; the
 * digest test checks the frames */
TEST(Ipmx_st20p, noipmx_tx_rgb8_digest_1080p60_sr_none) {
  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx_rx_only = true;
  para.rx_check = ipmx_st20p_sr_none;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

/* ST 2022-7: each RX leg reads the SRs of its own TX leg */
TEST(Ipmx_st20p, rgb8_digest_1080p60_sr_redundant) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  if (ctx->para.num_ports < 4)
    GTEST_SKIP() << "ST 2022-7 needs 4 ports, have " << ctx->para.num_ports;

  struct st20p_rx_digest_test_para para;
  ipmx_st20p_sr_para(&para);
  para.redundant = true;
  para.rx_check = ipmx_st20p_sr_check_redundant;
  ipmx_st20p_digest_para(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P60, 1920, 1080,
                         &para);
}

/* soak for IPMX_SOAK_S seconds, skipped when unset */
static void ipmx_st20p_soak(enum st_frame_fmt fmt, enum st20_fmt transport_fmt,
                            enum st_fps fps, int width, int height) {
  const char* soak_s = getenv("IPMX_SOAK_S");
  if (!soak_s || atoi(soak_s) <= 0) GTEST_SKIP() << "set IPMX_SOAK_S to the soak seconds";

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.duration_s = atoi(soak_s);
  ipmx_st20p_digest_para(fmt, transport_fmt, fps, width, height, &para);
}

TEST(Ipmx_st20p, rgb8_soak_1080p59_94) {
  ipmx_st20p_soak(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT, ST_FPS_P59_94, 1920, 1080);
}

TEST(Ipmx_st20p, rgb10_soak_2160p60) {
  ipmx_st20p_soak(ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, ST_FPS_P60, 3840,
                  2160);
}

#endif
