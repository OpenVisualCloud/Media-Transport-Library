/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20P_RX_FLAG_PKT_CONVERT: the pipeline converts each packet into a
 * framebuffer as it arrives, from the uframe callback of the RX session.
 *
 * connect() wires the production ST 2110-20 RX session (session harness) to the
 * pipeline with the ops the pipeline passed to st20_rx_create(), and routes the
 * pipeline's st20_rx_put_framebuff() back to that session.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "pipeline/st20p_harness.h"
#include "session/st20_harness.h"
#include "st_convert_api.h"

namespace {

constexpr uint32_t kWidth = 16; /* the session harness width */
constexpr int kRows = 4;        /* one row per packet */
constexpr int kSessionFrames = 2;

/* Repeated over a 5-byte pgroup, these bytes unpack to one 10-bit value in
 * every sample: 0x155 and 0x2aa. */
constexpr uint8_t kFillA = 0x55;
constexpr uint8_t kFillB = 0xaa;
constexpr uint16_t kSampleA = 0x155;
constexpr uint16_t kSampleB = 0x2aa;

int put_to_session(void* session, void* frame) {
  return ut20_put_framebuff(static_cast<ut20_test_ctx*>(session), frame);
}

/* Samples of `row` in the three planes of a 422 planar 16-bit container that are
 * not `value`. */
int row_mismatches(const struct st_frame* f, int row, uint16_t value) {
  int bad = 0;
  for (int plane = 0; plane < 3; plane++) {
    const uint16_t* s = reinterpret_cast<const uint16_t*>(
        static_cast<const uint8_t*>(f->addr[plane]) + f->linesize[plane] * row);
    uint32_t samples = plane ? f->width / 2 : f->width;
    for (uint32_t i = 0; i < samples; i++) bad += s[i] != value;
  }
  return bad;
}

}  // namespace

class St20pRxPktConvertTest : public ::testing::Test {
 protected:
  ut20p_ctx* pipe_ = nullptr;
  ut20_test_ctx* session_ = nullptr;

  virtual int num_port() const {
    return 1;
  }
  virtual int framebuff_cnt() const {
    return 3;
  }

  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0);
    pipe_ = ut20p_ctx_create(framebuff_cnt());
    ASSERT_NE(pipe_, nullptr);
    session_ = ut20_ctx_create_geom(num_port(), kRows);
    ASSERT_NE(session_, nullptr);
  }

  void TearDown() override {
    ut20_ctx_destroy(session_);
    ut20p_ctx_destroy(pipe_);
  }

  int enable(enum st_frame_fmt output_fmt, uint32_t width, uint32_t height,
             enum st_fps fps, uint32_t flags) {
    struct st20_rx_ops ops_rx;
    return ut20p_ctx_enable_pkt_convert(pipe_, output_fmt, width, height, fps, flags,
                                        &ops_rx);
  }

  void connect(enum st_frame_fmt output_fmt, uint32_t flags = 0) {
    struct st20_rx_ops ops_rx;
    ASSERT_EQ(ut20p_ctx_enable_pkt_convert(pipe_, output_fmt, kWidth, kRows, ST_FPS_P30,
                                           flags, &ops_rx),
              0);
    ut20_ctx_set_rx_ops(session_, &ops_rx);
    ut20p_set_put_framebuff(put_to_session, session_);
  }

  void feed(int row, uint32_t ts, uint8_t fill,
            enum mtl_session_port port = MTL_SESSION_PORT_P) {
    ut20_feed_frame_pkt_fill(session_, row, ts, port, fill);
  }

  void feed_full(uint32_t ts, uint8_t fill) {
    for (int row = 0; row < kRows; row++) feed(row, ts, fill);
  }

  /* Get the next frame, which must be the complete frame at ts with every
   * sample `value`, and put it back. */
  void expect_frame(uint32_t ts, uint16_t value) {
    struct st_frame* f = ut20p_get_frame(pipe_);
    ASSERT_NE(f, nullptr) << "no frame for ts " << ts;
    EXPECT_EQ(f->timestamp, ts);
    EXPECT_TRUE(st_is_frame_complete(f->status)) << "status " << f->status;
    for (int row = 0; row < kRows; row++)
      EXPECT_EQ(row_mismatches(f, row, value), 0) << "ts " << ts << " row " << row;
    EXPECT_EQ(ut20p_put_frame(pipe_, f), 0);
  }
};

/* Each lossy frame is dropped without being delivered; more of them than
 * framebuffers must not leave the next complete frame without one. */
TEST_F(St20pRxPktConvertTest, LossyFramesDoNotLeakFramebuffs) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  for (uint32_t ts = 1000; ts < 1000u + 4 * framebuff_cnt(); ts++) {
    feed(0, ts, kFillB);
    feed(1, ts, kFillB);
    feed(3, ts, kFillB);
  }

  feed_full(2000, kFillA);
  expect_frame(2000, kSampleA);
  EXPECT_EQ(ut20p_get_frame(pipe_), nullptr) << "a lossy frame was delivered";
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

TEST_F(St20pRxPktConvertTest, IncompleteFrameDeliveredWhenAsked) {
  ASSERT_NO_FATAL_FAILURE(
      connect(ST_FRAME_FMT_YUV422PLANAR10LE, ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME));
  feed(0, 1000, kFillA);
  feed(1, 2000, kFillA); /* evicts the frame at 1000 */

  struct st_frame* f = ut20p_get_frame(pipe_);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->timestamp, 1000u);
  EXPECT_EQ(f->status, ST_FRAME_STATUS_CORRUPTED);
  EXPECT_EQ(ut20p_put_frame(pipe_, f), 0);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames - 1) << "ts 2000 holds one";
}

TEST_F(St20pRxPktConvertTest, Yuv422Planar16leConvertsEachPacket) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR16LE));
  feed_full(1000, kFillB);
  expect_frame(1000, kSampleB << 6);
}

TEST_F(St20pRxPktConvertTest, OutputWithoutPerPacketConverterRefusedAtCreate) {
  for (enum st_frame_fmt fmt : {ST_FRAME_FMT_YUV422PLANAR8, ST_FRAME_FMT_YUV420PLANAR8})
    EXPECT_LT(enable(fmt, kWidth, kRows, ST_FPS_P30, 0), 0) << st_frame_fmt_name(fmt);
}

/* A dynamic ext frame is only queried at frame end, after the packets were
 * converted into its still unset dst. */
TEST_F(St20pRxPktConvertTest, DynamicExtFramesRefusedAtCreate) {
  EXPECT_LT(enable(ST_FRAME_FMT_YUV422PLANAR10LE, kWidth, kRows, ST_FPS_P30,
                   ST20P_RX_FLAG_EXT_FRAME),
            0);
}

/* The session hands packets to a pkt lcore and, when its ring is full, also to
 * the tasklet; the uframe callback is not safe on both at once. */
TEST_F(St20pRxPktConvertTest, SessionsWithPktLcoreRefusedAtCreate) {
  EXPECT_LT(enable(ST_FRAME_FMT_YUV422PLANAR10LE, kWidth, kRows, ST_FPS_P30,
                   ST20P_RX_FLAG_USE_MULTI_THREADS),
            0);
  EXPECT_LT(enable(ST_FRAME_FMT_YUV422PLANAR10LE, 7680, 4320, ST_FPS_P119_88, 0), 0)
      << "8K at 119.88 fps is above the session's pkt lcore rate";
}

/* The session sizes its frames and may add a pkt lcore for the detected format. */
TEST_F(St20pRxPktConvertTest, AutoDetectRefusedAtCreate) {
  EXPECT_LT(enable(ST_FRAME_FMT_YUV422PLANAR10LE, kWidth, kRows, ST_FPS_P30,
                   ST20P_RX_FLAG_AUTO_DETECT),
            0);
}

class St20pRxPktConvertOneFramebuffTest : public St20pRxPktConvertTest {
 protected:
  int framebuff_cnt() const override {
    return 1;
  }
};

/* The app holds the only framebuffer at the first packet of the next frame, so
 * that whole frame is refused: one pipeline drop and no wire loss. */
TEST_F(St20pRxPktConvertOneFramebuffTest, BackPressureIsOneDropWithoutWireLoss) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  feed_full(1000, kFillA);
  struct st_frame* held = ut20p_get_frame(pipe_);
  ASSERT_NE(held, nullptr);

  feed(0, 2000, kFillB);
  feed(1, 2000, kFillB);
  EXPECT_EQ(ut20p_put_frame(pipe_, held), 0);
  feed(2, 2000, kFillB);
  feed(3, 2000, kFillB);

  feed_full(3000, kFillA);
  expect_frame(3000, kSampleA);
  EXPECT_EQ(ut20p_stat_frames_dropped(pipe_), 1u);
  EXPECT_EQ(ut20p_stat_busy(pipe_), 1u);
  EXPECT_EQ(ut20_stat_frames_incomplete(session_), 0u);
  EXPECT_EQ(ut20_stat_pkts_unrecovered(session_), 0u);
  EXPECT_EQ(ut20_stat_lost_pkts(session_), 0u);
  EXPECT_EQ(ut20_stat_port_lost(session_, MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

/* A refused frame that also lost packets reaches frame_ready incomplete, which
 * makes its session frame the pipeline's to put back. */
TEST_F(St20pRxPktConvertOneFramebuffTest, BackPressuredLossyFramePutBackOnce) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  feed_full(1000, kFillA);
  struct st_frame* held = ut20p_get_frame(pipe_);
  ASSERT_NE(held, nullptr);
  feed(0, 2000, kFillB);
  EXPECT_EQ(ut20p_put_frame(pipe_, held), 0);

  feed_full(3000, kFillA); /* evicts the frame at 2000 */
  expect_frame(3000, kSampleA);
  EXPECT_EQ(ut20p_stat_frames_dropped(pipe_), 1u);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

/* st20p_rx_create() sets the pipeline ready only after st20_rx_create() has
 * returned, while the session may already hand over packets and frames. */
TEST_F(St20pRxPktConvertTest, FrameStartedBeforeReadyRefusedWhole) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  ut20p_set_ready(pipe_, false);
  feed(0, 1000, kFillB);
  feed(1, 1000, kFillB);
  ut20p_set_ready(pipe_, true);
  feed(2, 1000, kFillB);
  feed(3, 1000, kFillB);

  feed_full(2000, kFillA);
  expect_frame(2000, kSampleA);
  EXPECT_EQ(ut20p_get_frame(pipe_), nullptr) << "the frame at 1000 was delivered";
  EXPECT_EQ(ut20p_stat_frames_dropped(pipe_), 1u);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

/* Each parked frame holds a session frame, so with both of the harness's parked
 * the session has none left and only create's own flush can return them. */
TEST_F(St20pRxPktConvertTest, IncompleteFramesParkedBeforeReadyPutBackOnce) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  ut20p_set_ready(pipe_, false);
  feed(0, 1000, kFillB);
  feed(0, 2000, kFillB); /* parks the frame at 1000 */
  feed(0, 3000, kFillB); /* parks the frame at 2000 */
  EXPECT_EQ(ut20_free_frames(session_), 0);

  ut20p_set_ready(pipe_, true);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
  feed_full(4000, kFillA);
  expect_frame(4000, kSampleA);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

/* The session can park a frame between create's flush and its store of ready
 * becoming visible; the next frame_ready has to put it back. */
TEST_F(St20pRxPktConvertTest, FrameParkedPastCreatesFlushPutBackByFrameReady) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  ut20p_set_ready(pipe_, false);
  feed(0, 1000, kFillB);
  feed(0, 2000, kFillB); /* parks the frame at 1000 */
  ut20p_set_ready_past_flush(pipe_);

  feed_full(3000, kFillA); /* evicts the frame at 2000 */
  expect_frame(3000, kSampleA);
  EXPECT_EQ(ut20_free_frames(session_), kSessionFrames);
}

class St20pRxPktConvertRedundantTest : public St20pRxPktConvertTest {
 protected:
  int num_port() const override {
    return 2;
  }
};

/* With two paths the session assembles two frames at once: the next frame is
 * under way on P while the last packet of this one is still to come on R. */
TEST_F(St20pRxPktConvertRedundantTest, InterleavedFramesConvertIntoTheirOwnFramebuffs) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  for (int row = 0; row < kRows - 1; row++) feed(row, 1000, kFillA);
  feed(0, 2000, kFillB);
  feed(1, 2000, kFillB);
  feed(kRows - 1, 1000, kFillA, MTL_SESSION_PORT_R);
  for (int row = 2; row < kRows; row++) feed(row, 2000, kFillB);

  expect_frame(1000, kSampleA);
  expect_frame(2000, kSampleB);
}

TEST_F(St20pRxPktConvertRedundantTest, InterleavedFrameWithRowZeroLast) {
  ASSERT_NO_FATAL_FAILURE(connect(ST_FRAME_FMT_YUV422PLANAR10LE));
  for (int row = 1; row < kRows; row++) feed(row, 1000, kFillA);
  feed(1, 2000, kFillB);
  feed(0, 1000, kFillA, MTL_SESSION_PORT_R);
  feed(0, 2000, kFillB);
  for (int row = 2; row < kRows; row++) feed(row, 2000, kFillB);

  expect_frame(1000, kSampleA);
  expect_frame(2000, kSampleB);
}

/* 720p, 1080p and 2160p rows split over packets of 1260-byte payloads, at pixel offsets
 * no cache line divides: each plane matches converting the whole row at once, so the
 * whole lines streamed and the partial ones at the packet ends land where they belong. */
class St20pRxPktConvertRowTest
    : public ::testing::TestWithParam<std::tuple<enum st_frame_fmt, uint32_t>> {
 protected:
  static constexpr int kRowCnt = 2;
  static constexpr uint32_t kTs = 3000;
  static constexpr uint16_t kPktPg = 252; /* a 1260-byte payload of 5-byte pgroups */
  ut20p_ctx* pipe_ = nullptr;

  enum st_frame_fmt fmt() const {
    return std::get<0>(GetParam());
  }
  uint32_t width() const {
    return std::get<1>(GetParam());
  }

  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0);
    pipe_ = ut20p_ctx_create(3);
    ASSERT_NE(pipe_, nullptr);
    struct st20_rx_ops ops_rx;
    ASSERT_EQ(ut20p_ctx_enable_pkt_convert(pipe_, fmt(), width(), kRowCnt, ST_FPS_P59_94,
                                           0, &ops_rx),
              0);
    ut20p_set_ready(pipe_, true);
  }
  void TearDown() override {
    ut20p_ctx_destroy(pipe_);
  }

  /* the whole row src converted at once into plane rows dst */
  int convert_row(struct st20_rfc4175_422_10_pg2_be* src, uint8_t* const dst[3]) const {
    uint32_t pg = width() / 2;
    switch (fmt()) {
      case ST_FRAME_FMT_YUV422PLANAR10LE:
        return st20_rfc4175_422be10_to_yuv422p10le(
            src, (uint16_t*)dst[0], (uint16_t*)dst[1], (uint16_t*)dst[2], pg, 2);
      case ST_FRAME_FMT_YUV422PLANAR16LE:
        return st20_rfc4175_422be10_to_yuv422p16le(
            src, (uint16_t*)dst[0], (uint16_t*)dst[1], (uint16_t*)dst[2], pg, 2);
      case ST_FRAME_FMT_Y210:
        return st20_rfc4175_422be10_to_y210(src, (uint16_t*)dst[0], pg, 2);
      default:
        return st20_rfc4175_422be10_to_422le8(
            src, (struct st20_rfc4175_422_8_pg2_le*)dst[0], pg, 2);
    }
  }
};

TEST_P(St20pRxPktConvertRowTest, PacketsAtUnalignedOffsetsMatchTheWholeRow) {
  const uint16_t row_pg = (uint16_t)(width() / 2);
  const size_t row_bytes = row_pg * sizeof(struct st20_rfc4175_422_10_pg2_be);
  std::vector<uint8_t> payload(row_bytes * kRowCnt);
  for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 131 + 7);

  for (int row = 0; row < kRowCnt; row++) {
    for (uint16_t pg_off = 0; pg_off < row_pg; pg_off += kPktPg) {
      uint16_t cnt = std::min<uint16_t>(kPktPg, row_pg - pg_off);
      uint8_t* p = payload.data() + row * row_bytes +
                   pg_off * sizeof(struct st20_rfc4175_422_10_pg2_be);
      ASSERT_EQ(ut20p_pkt_convert(pipe_, p, row, pg_off * 2, cnt, kTs), 0);
    }
  }
  struct st_frame* f = ut20p_pkt_convert_dst(pipe_, kTs);
  ASSERT_NE(f, nullptr);

  const int planes = st_frame_fmt_planes(fmt());
  for (int row = 0; row < kRowCnt; row++) {
    std::vector<std::vector<uint8_t>> want(3);
    uint8_t* dst[3] = {nullptr};
    for (int p = 0; p < planes; p++) {
      want[p].assign(f->linesize[p], 0);
      dst[p] = want[p].data();
    }
    ASSERT_EQ(convert_row(reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(
                              payload.data() + row * row_bytes),
                          dst),
              0);
    for (int p = 0; p < planes; p++) {
      size_t n = st_frame_least_linesize(fmt(), width(), p);
      const uint8_t* got = static_cast<const uint8_t*>(f->addr[p]) + f->linesize[p] * row;
      EXPECT_EQ(memcmp(got, want[p].data(), n), 0) << "row " << row << " plane " << p;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    FmtsWidths, St20pRxPktConvertRowTest,
    ::testing::Combine(::testing::Values(ST_FRAME_FMT_YUV422PLANAR10LE,
                                         ST_FRAME_FMT_YUV422PLANAR16LE, ST_FRAME_FMT_Y210,
                                         ST_FRAME_FMT_UYVY),
                       ::testing::Values(1280u, 1920u, 3840u)),
    [](const ::testing::TestParamInfo<std::tuple<enum st_frame_fmt, uint32_t>>& info) {
      return std::string(st_frame_fmt_name(std::get<0>(info.param))) + "_" +
             std::to_string(std::get<1>(info.param));
    });
