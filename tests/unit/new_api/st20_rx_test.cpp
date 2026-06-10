/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20 NEW-API (unified session) RX frame-count contract tests.
 *
 * Parallel copy of tests/unit/pipeline/st20p_test.cpp, adapted to the unified
 * session API in lib/src/new_api. Old → new mapping:
 *
 *   pipeline stat_frames_received  ≙ s->stats.buffers_processed
 *     (bumped in buffer_get, NOT at datapath ingress).
 *   pipeline stat_frames_dropped   ≙ s->stats.buffers_dropped
 *     (bumped in notify_frame_ready when the ready_ring is full).
 *   pipeline stat_busy             → no equivalent. The unified API has no
 *     separate busy atomic; back-pressure is counted solely by buffers_dropped,
 *     which bumps 1:1 with each ready_ring overflow.
 *   pipeline stat_frames_corrupted → no equivalent. A non-COMPLETE frame is
 *     still delivered by buffer_get with status MTL_FRAME_STATUS_INCOMPLETE and
 *     the MTL_BUF_FLAG_INCOMPLETE flag, and still counts as processed.
 *   pipeline get_session_stats overlay → io_stats_get is pure transport
 *     passthrough; the abstract counters live on stats_get instead.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <set>
#include <thread>

#include "new_api/st20_rx_harness.h"

class St20NewApiRxTest : public ::testing::Test {
 protected:
  ut20rx_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20rx_init(), 0) << "EAL init failed";
    ctx_ = ut20rx_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20rx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int inject_complete(uint32_t ts) {
    return ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, ts);
  }
  int inject_corrupted(uint32_t ts) {
    return ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_CORRUPTED, ts);
  }
  mtl_buffer_t* get_buffer() {
    return ut20rx_buffer_get(ctx_);
  }
  int put_buffer(mtl_buffer_t* b) {
    return ut20rx_buffer_put(ctx_, b);
  }

  uint64_t buffers_processed() {
    return ut20rx_buffers_processed(ctx_);
  }
  uint64_t buffers_dropped() {
    return ut20rx_buffers_dropped(ctx_);
  }
};

/* The unified session counts a buffer as "processed" only when the application
 * calls buffer_get. Inject N frames but consume only M < N → counter == M.
 * (pipeline analog: FramesReceivedOnlyOnGetFrame.) */
TEST_F(St20NewApiRxTest, BuffersProcessedOnlyOnBufferGet) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  EXPECT_EQ(buffers_processed(), 0u)
      << "notify_frame_ready must not bump buffers_processed — only buffer_get does";

  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(buffers_processed(), 1u);
  EXPECT_EQ(put_buffer(b), 0);

  /* second consume */
  b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(buffers_processed(), 2u);
  EXPECT_EQ(put_buffer(b), 0);

  /* third frame still pending in the ready_ring — not yet counted */
  EXPECT_EQ(buffers_processed(), 2u);
}

/* When the ready_ring is full (no buffer_get draining), every further
 * notify_frame_ready must bump buffers_dropped 1:1 with the overflow event.
 * (pipeline analog: FramesDroppedWhenFramebufsFull. The pipeline's
 * stat_busy 1:1 invariant is dropped — the unified API has no busy atomic.) */
TEST_F(St20NewApiRxTest, BuffersDroppedWhenReadyRingFull) {
  /* fill all 3 ready_ring slots */
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  EXPECT_EQ(buffers_dropped(), 0u);

  /* further frames have nowhere to go */
  EXPECT_EQ(inject_complete(4000), -ENOSPC);
  EXPECT_EQ(inject_complete(5000), -ENOSPC);
  EXPECT_EQ(inject_complete(6000), -ENOSPC);

  EXPECT_EQ(buffers_dropped(), 3u);
  EXPECT_EQ(buffers_processed(), 0u) << "dropped frames must not be counted as processed";
}

/* A non-COMPLETE (CORRUPTED) frame is still delivered to the application.
 * Unlike the pipeline (which has a dedicated stat_frames_corrupted), the
 * unified API surfaces corruption only through buffer status/flags: the buffer
 * carries MTL_FRAME_STATUS_INCOMPLETE + MTL_BUF_FLAG_INCOMPLETE and still
 * counts toward buffers_processed. COMPLETE frames stay COMPLETE/no-flag.
 * (pipeline analog: CorruptedDeliveredAndCounted.) */
TEST_F(St20NewApiRxTest, IncompleteDeliveredWithStatusFlag) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  mtl_buffer_t* b0 = get_buffer();
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->status, MTL_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(b0->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b0), 0);

  mtl_buffer_t* b1 = get_buffer();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->status, MTL_FRAME_STATUS_INCOMPLETE)
      << "a non-COMPLETE frame must still be delivered, marked incomplete";
  EXPECT_NE(b1->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b1), 0);

  mtl_buffer_t* b2 = get_buffer();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(b2->status, MTL_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(put_buffer(b2), 0);

  EXPECT_EQ(buffers_processed(), 3u)
      << "every delivered frame, complete or not, counts as processed";
}

/* stats_reset clears every cumulative unified-session counter.
 * (pipeline analog: ResetClearsAllPipelineCounters.) */
TEST_F(St20NewApiRxTest, ResetClearsAllCounters) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  mtl_buffer_t* b1 = get_buffer();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(put_buffer(b1), 0);
  mtl_buffer_t* b2 = get_buffer();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(put_buffer(b2), 0);

  /* fill up and overflow to exercise buffers_dropped */
  ASSERT_EQ(inject_complete(3000), 0);
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  EXPECT_EQ(inject_complete(6000), -ENOSPC);

  ASSERT_GT(buffers_processed(), 0u);
  ASSERT_GT(buffers_dropped(), 0u);
  ASSERT_GT(ut20rx_bytes_processed(ctx_), 0u);

  EXPECT_EQ(ut20rx_reset_stats(ctx_), 0);

  EXPECT_EQ(buffers_processed(), 0u);
  EXPECT_EQ(buffers_dropped(), 0u);
  EXPECT_EQ(ut20rx_bytes_processed(ctx_), 0u);
}

/* The unified API exposes two distinct stats surfaces:
 *   - stats_get   → the abstract counters this suite drives.
 *   - io_stats_get → a pure passthrough of the transport's own stats.
 * Unlike the pipeline (which OVERLAYS the frame counters onto the transport
 * stats), the two surfaces are independent here. Assert that stats_get matches
 * what the test drove, while the passthrough io_stats returns the (stubbed)
 * zeroed transport stats. (pipeline analog: GetSessionStatsOverlay.) */
TEST_F(St20NewApiRxTest, StatsGetVsIoStatsPassthrough) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  for (int i = 0; i < 3; i++) {
    mtl_buffer_t* b = get_buffer();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(put_buffer(b), 0);
  }
  /* fill + one overflow */
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  ASSERT_EQ(inject_complete(6000), 0);
  EXPECT_EQ(inject_complete(7000), -ENOSPC);

  mtl_session_stats_t abstract{};
  ASSERT_EQ(ut20rx_stats_get(ctx_, &abstract), 0);
  EXPECT_EQ(abstract.buffers_processed, buffers_processed());
  EXPECT_EQ(abstract.buffers_dropped, buffers_dropped());

  struct st20_rx_user_stats io {};
  ASSERT_EQ(ut20rx_io_stats_get(ctx_, &io), 0);
  EXPECT_EQ(io.common.stat_frames_received, 0u)
      << "io_stats_get is transport passthrough, not an overlay of the "
         "abstract counters";
}

/* Invariant: buffers_dropped advances monotonically, exactly once per
 * ready_ring overflow. Replaces the pipeline's BusyEqualsDroppedInvariant —
 * the unified API has no busy atomic, so the 1:1 relationship is between the
 * overflow event and buffers_dropped itself. */
TEST_F(St20NewApiRxTest, DroppedMonotonicWithOverflow) {
  /* fill */
  for (int i = 0; i < 3; i++) ASSERT_EQ(inject_complete(1000 + i), 0);
  /* overflow burst */
  for (uint64_t i = 0; i < 5; i++) {
    EXPECT_EQ(inject_complete(2000 + i), -ENOSPC);
    EXPECT_EQ(buffers_dropped(), i + 1)
        << "after overflow " << (i + 1)
        << ": buffers_dropped must bump exactly once per event";
  }
}

/* rx_fill_buffer_status collapses both COMPLETE and RECONSTRUCTED transport
 * statuses onto MTL_FRAME_STATUS_COMPLETE (no INCOMPLETE flag); any other
 * status (CORRUPTED) surfaces MTL_FRAME_STATUS_INCOMPLETE + the flag. This is a
 * deliberate divergence from the pipeline, which passes the raw status through
 * (RECONSTRUCTED stays RECONSTRUCTED — see St20PipelineRxTest). */
TEST_F(St20NewApiRxTest, StatusCompleteVsReconstructed) {
  ASSERT_EQ(ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, 1000), 0);
  ASSERT_EQ(ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_RECONSTRUCTED, 2000), 0);
  ASSERT_EQ(ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_CORRUPTED, 3000), 0);

  mtl_buffer_t* b0 = get_buffer();
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->status, MTL_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(b0->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b0), 0);

  mtl_buffer_t* b1 = get_buffer();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->status, MTL_FRAME_STATUS_COMPLETE)
      << "RECONSTRUCTED must collapse onto COMPLETE in the unified API";
  EXPECT_EQ(b1->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b1), 0);

  mtl_buffer_t* b2 = get_buffer();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(b2->status, MTL_FRAME_STATUS_INCOMPLETE);
  EXPECT_NE(b2->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b2), 0);
}

/* rx_convert_and_fill_buffer: derive mode hands the app the transport
 * framebuffer with no converter call; a transport-fmt != app-fmt session runs
 * the converter and the buffer carries the app fmt/size. */
TEST_F(St20NewApiRxTest, ConvertTransportToApp) {
  /* derive (default): no converter, zero-copy transport buffer */
  ASSERT_EQ(inject_complete(1000), 0);
  mtl_buffer_t* d = get_buffer();
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ut20rx_convert_calls(ctx_), 0) << "derive mode must not convert";
  EXPECT_EQ(put_buffer(d), 0);

  /* fresh ctx switched into convert mode */
  ut20rx_ctx_destroy(ctx_);
  ctx_ = ut20rx_ctx_create(/*framebuff_cnt=*/3);
  ASSERT_NE(ctx_, nullptr);

  static uint8_t app_buf[64];
  ut20rx_enable_convert(ctx_, ST_FRAME_FMT_YUV422PLANAR10LE, app_buf, sizeof(app_buf));

  ASSERT_EQ(inject_complete(2000), 0);
  mtl_buffer_t* c = get_buffer();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(ut20rx_convert_calls(ctx_), 1) << "convert mode must run the converter once";
  EXPECT_EQ(c->video.fmt, ST_FRAME_FMT_YUV422PLANAR10LE)
      << "converted buffer must carry the app fmt";
  EXPECT_EQ(c->data, app_buf) << "converted buffer must point at the app destination";
  EXPECT_EQ(c->size, sizeof(app_buf));
  EXPECT_EQ(put_buffer(c), 0);
}

/* rx_fill_user_metadata copies the frame_trans user_meta pointer/size into the
 * delivered buffer (RX pass-through). */
TEST_F(St20NewApiRxTest, UserMetaPassthrough) {
  static uint8_t meta_blob[16] = {0xDE, 0xAD, 0xBE, 0xEF};
  ut20rx_set_frame_user_meta(ctx_, /*idx=*/0, meta_blob, sizeof(meta_blob));

  ASSERT_EQ(inject_complete(1000), 0);
  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->user_meta, meta_blob);
  EXPECT_EQ(b->user_meta_size, sizeof(meta_blob));
  EXPECT_EQ(put_buffer(b), 0);
}

/* rx_fill_buffer_video_fields reflects the synthetic st20_rx_frame_meta into the
 * buffer's video sub-struct (pkts_total/pkts_recv/second_field/dimensions);
 * interlaced is carried from the convert ctx (false by default here). */
TEST_F(St20NewApiRxTest, VideoMetaFieldsFilled) {
  struct st20_rx_frame_meta meta {};
  meta.status = ST_FRAME_STATUS_COMPLETE;
  meta.timestamp = 4242;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.width = 1920;
  meta.height = 1080;
  meta.pkts_total = 100;
  meta.pkts_recv[0] = 90;
  meta.pkts_recv[1] = 80;
  meta.second_field = true;
  ASSERT_EQ(ut20rx_inject_meta(ctx_, &meta), 0);

  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->video.width, 1920u);
  EXPECT_EQ(b->video.height, 1080u);
  EXPECT_EQ(b->video.pkts_total, 100u);
  EXPECT_EQ(b->video.pkts_recv[0], 90u);
  EXPECT_EQ(b->video.pkts_recv[1], 80u);
  EXPECT_TRUE(b->video.second_field);
  EXPECT_FALSE(b->video.interlaced) << "interlaced is convert-ctx driven, false here";
  EXPECT_EQ(put_buffer(b), 0);
}

/* rx_fill_buffer_status forwards rtp_timestamp/tfmt/timestamp from the meta. */
TEST_F(St20NewApiRxTest, RtpTimestampAndTfmt) {
  struct st20_rx_frame_meta meta {};
  meta.status = ST_FRAME_STATUS_COMPLETE;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.pkts_total = 1;
  meta.rtp_timestamp = 0xCAFEu;
  meta.tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta.timestamp = 0x123456789ULL;
  ASSERT_EQ(ut20rx_inject_meta(ctx_, &meta), 0);

  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->rtp_timestamp, 0xCAFEu);
  EXPECT_EQ(b->tfmt, ST10_TIMESTAMP_FMT_TAI);
  EXPECT_EQ(b->timestamp, 0x123456789ULL);
  EXPECT_EQ(put_buffer(b), 0);
}

/* mtl_video_rx_session_init clamps a requested framebuff_cnt < 2 up to 2 (the
 * pipeline does the same — mirrored in St20PipelineRxTest). Counts >= 2 pass
 * through unchanged. */
TEST_F(St20NewApiRxTest, FramebuffCntClampedToTwo) {
  EXPECT_EQ(ut20rx_clamp_framebuff_cnt(0), 2u);
  EXPECT_EQ(ut20rx_clamp_framebuff_cnt(1), 2u);
  EXPECT_EQ(ut20rx_clamp_framebuff_cnt(2), 2u);
  EXPECT_EQ(ut20rx_clamp_framebuff_cnt(5), 5u);
}

/* video_rx_buffer_get(timeout_ms=0) on an empty ready_ring returns -ETIMEDOUT
 * immediately (the harness wrapper surfaces that as NULL); a queued frame is
 * returned. Divergence from the pipeline: the unified RX get is a bounded poll,
 * not a condvar wait — timeout_ms=0 never blocks. */
TEST_F(St20NewApiRxTest, BlockGetTimeoutSemantics) {
  EXPECT_EQ(get_buffer(), nullptr) << "empty ready_ring + timeout 0 must not block";

  ASSERT_EQ(inject_complete(1000), 0);
  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr) << "a queued frame is returned immediately";
  EXPECT_EQ(put_buffer(b), 0);

  EXPECT_EQ(get_buffer(), nullptr) << "ring drained again → -ETIMEDOUT";
}

/* Characterization of divergence #4: the buffers_processed bump site depends on
 * ownership. LIBRARY_OWNED bumps in buffer_get (app consumes); USER_OWNED via
 * buffer_post bumps in notify_frame_ready (the lib copies into the user buffer
 * and returns the frame immediately, before any buffer_get). Exactly once per
 * delivered frame in each mode. */
TEST_F(St20NewApiRxTest, BuffersProcessedSiteCrossMode) {
  /* LIBRARY_OWNED: no bump at ingress, one bump at buffer_get */
  ASSERT_EQ(inject_complete(1000), 0);
  EXPECT_EQ(buffers_processed(), 0u) << "library-owned: ingress must not bump";
  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(buffers_processed(), 1u) << "library-owned: buffer_get bumps once";
  EXPECT_EQ(put_buffer(b), 0);

  /* USER_OWNED post: the bump moves to ingress (notify_frame_ready) */
  ut20rx_ctx_destroy(ctx_);
  ctx_ = ut20rx_ctx_create(/*framebuff_cnt=*/3);
  ASSERT_NE(ctx_, nullptr);
  ASSERT_EQ(ut20rx_enable_user_owned_post(ctx_), 0);

  static uint8_t user_buf[64];
  ASSERT_EQ(ut20rx_mem_register(ctx_, user_buf, sizeof(user_buf)), 0);
  ASSERT_EQ(ut20rx_post_user_buffer(ctx_, user_buf, sizeof(user_buf), (void*)0x77), 0);

  ASSERT_EQ(inject_complete(2000), 0);
  EXPECT_EQ(buffers_processed(), 1u)
      << "user-owned post: notify_frame_ready bumps once, no buffer_get needed";
}

/* video_rx_notify_detected posts MTL_EVENT_FORMAT_DETECTED carrying the detected
 * geometry. Reachable without a NIC by driving the static callback directly. */
TEST_F(St20NewApiRxTest, AutoDetectPostsFormatEvent) {
  ASSERT_EQ(
      ut20rx_notify_detected(ctx_, 1920, 1080, ST_FPS_P59_94, ST20_PACKING_BPM, false),
      0);

  mtl_event_t ev{};
  ASSERT_EQ(ut20rx_poll_event(ctx_, &ev), 0) << "a FORMAT_DETECTED event must be posted";
  EXPECT_EQ(ev.type, MTL_EVENT_FORMAT_DETECTED);
  EXPECT_EQ(ev.format_detected.width, 1920u);
  EXPECT_EQ(ev.format_detected.height, 1080u);
}

/* integration-only: MTL_EVENT_TIMING_REPORT (case 10, TimingParserPostsReport)
 * is not posted anywhere in lib/src/new_api — the timing-parser report path is
 * not yet wired into the unified session, so there is no production code to
 * exercise at the unit tier. Route to Phase 4 once the parser → event bridge
 * lands. */

/* The query_ext_frame wrapper (USER_OWNED, no app callback) binds a buffer
 * posted via buffer_post to the next transport ext_frame slot: addr/len/opaque
 * come straight off the posted user buffer. Exercises mem_register + buffer_post
 * + the wrapper end to end. */
TEST_F(St20NewApiRxTest, ExtFrameQueryWrapperBinds) {
  ut20rx_ctx_destroy(ctx_);
  ctx_ = ut20rx_ctx_create(/*framebuff_cnt=*/3);
  ASSERT_NE(ctx_, nullptr);
  ASSERT_EQ(ut20rx_enable_user_owned_post(ctx_), 0);

  static uint8_t user_buf[128];
  ASSERT_EQ(ut20rx_mem_register(ctx_, user_buf, sizeof(user_buf)), 0);
  ASSERT_EQ(ut20rx_post_user_buffer(ctx_, user_buf, sizeof(user_buf), (void*)0x99), 0);

  struct st20_rx_frame_meta meta {};
  meta.width = 1920;
  meta.height = 1080;
  meta.frame_total_size = sizeof(user_buf);

  struct st20_ext_frame ext {};
  ASSERT_EQ(ut20rx_query_ext_frame(ctx_, &ext, &meta), 0)
      << "a posted user buffer must be available to bind";
  EXPECT_EQ(ext.buf_addr, user_buf);
  EXPECT_EQ(ext.buf_len, sizeof(user_buf));
  EXPECT_EQ(ext.opaque, (void*)0x99);
}

/* In USER_OWNED mode with an explicit app query_ext_frame, the transport carries
 * the app's ext opaque on meta->opaque. The unified session must surface that
 * opaque as the buffer completion ctx (buf->user_data), NOT the library's
 * internal user_meta scratch buffer. */
TEST_F(St20NewApiRxTest, ExtFrameCompletionCarriesAppOpaque) {
  ASSERT_EQ(ut20rx_enable_user_owned_query_ext(ctx_), 0);

  /* A distinct non-NULL user_meta proves the completion ctx is the app opaque,
   * not the internal user_meta pointer. */
  ut20rx_set_frame_user_meta(ctx_, 0, (void*)0xDEAD, 8);

  struct st20_rx_frame_meta meta {};
  meta.status = ST_FRAME_STATUS_COMPLETE;
  meta.frame_total_size = 1;
  meta.frame_recv_size = 1;
  meta.pkts_total = 1;
  meta.opaque = (void*)0xABCD; /* the app ext opaque */
  ASSERT_EQ(ut20rx_inject_meta(ctx_, &meta), 0);

  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->user_data, (void*)0xABCD)
      << "completion ctx must be the app ext opaque, not the lib user_meta";
  EXPECT_EQ(put_buffer(b), 0);
}

/* The wrapper pool must be sized from the actual (bumped, >= 2) frame count, not
 * the raw num_buffers. The create path forces st20_frames_cnt >= 2, so a
 * num_buffers of 0 or 1 must still yield buffer_count == frame_count: otherwise
 * the hot path s->buffers[i % buffer_count] divides by zero (0) or aliases two
 * frames onto one wrapper (1). */
TEST_F(St20NewApiRxTest, BufferPoolSizeFollowsFrameCount) {
  const uint32_t fc = ut20rx_frame_count(ctx_);
  ASSERT_GE(fc, 2u);

  ASSERT_EQ(ut20rx_init_buffers(ctx_), 0);
  ASSERT_EQ(ut20rx_buffer_count(ctx_), fc)
      << "wrapper pool must match the frame count (no % 0, no aliasing)";

  std::set<mtl_buffer_t*> seen;
  for (uint32_t i = 0; i < fc; i++) {
    ASSERT_EQ(inject_complete(1000 + i), 0);
    mtl_buffer_t* b = get_buffer();
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(seen.insert(b).second)
        << "frame " << i << " must map to a distinct wrapper";
    EXPECT_EQ(put_buffer(b), 0);
  }
}

/* ── event-system mechanism (producer-nonblocking / consumer-blocking) ─── */

/* Producer overflow on the shared event queue: surplus posts are dropped and
 * counted, never allocated/blocked. */
TEST_F(St20NewApiRxTest, EventPostDropsOnFullBumpsCounter) {
  mtl_event_t ev = {};
  ev.type = MTL_EVENT_BUFFER_READY;

  const int kPosts = 256;
  int accepted = 0, dropped = 0;
  for (int i = 0; i < kPosts; i++) {
    ev.ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
    int ret = ut20rx_post_event(ctx_, &ev);
    if (ret == 0)
      accepted++;
    else
      dropped++;
  }

  EXPECT_GT(dropped, 0) << "posting past ring capacity must drop the surplus";
  EXPECT_EQ(accepted + dropped, kPosts);
  EXPECT_EQ(ut20rx_events_dropped(ctx_), static_cast<uint64_t>(dropped))
      << "every dropped post must bump the producer drop counter";
}

/* Value round-trip across the shared post/poll path on an RX session. */
TEST_F(St20NewApiRxTest, EventPostPollValueRoundTrip) {
  mtl_event_t in = {};
  in.type = MTL_EVENT_FORMAT_DETECTED;
  in.status = 7;
  in.timestamp = 123456789ULL;
  in.ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(0x5A5A));

  ASSERT_EQ(ut20rx_post_event(ctx_, &in), 0);

  mtl_event_t out = {};
  ASSERT_EQ(ut20rx_poll_event(ctx_, &out), 0);
  EXPECT_EQ(out.type, in.type);
  EXPECT_EQ(out.status, in.status);
  EXPECT_EQ(out.timestamp, in.timestamp);
  EXPECT_EQ(out.ctx, in.ctx);
}

/* Pre-check path: stopped before poll() is entered, so the top-of-function
 * check returns -EAGAIN without ever blocking. */
TEST_F(St20NewApiRxTest, EventPollPreStopReturnsEagain) {
  ut20rx_set_stopped(ctx_);
  mtl_event_t ev;
  EXPECT_EQ(ut20rx_poll_event_timeout(ctx_, &ev, 1000), -EAGAIN);
}

/* Missed-wakeup path: a consumer already blocked in poll() must wake promptly
 * when another thread calls stop(), returning -EAGAIN well under the timeout
 * rather than waiting out the full timeout_ms. */
TEST_F(St20NewApiRxTest, EventPollStopWakesBlockedPoll) {
  std::atomic<bool> entered{false};
  std::atomic<int> ret{1};
  std::thread consumer([&] {
    mtl_event_t ev;
    entered.store(true, std::memory_order_release);
    ret.store(ut20rx_poll_event_timeout(ctx_, &ev, 5000), std::memory_order_release);
  });

  while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

  auto t0 = std::chrono::steady_clock::now();
  ut20rx_stop(ctx_);
  consumer.join();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

  EXPECT_EQ(ret.load(std::memory_order_acquire), -EAGAIN);
  EXPECT_LT(elapsed_ms, 1000) << "stop() must wake the blocked poll promptly";
}

/* get_event_fd is wired and returns a valid (>= 0) wakeup fd. */
TEST_F(St20NewApiRxTest, EventFdValid) {
  EXPECT_GE(ut20rx_get_event_fd(ctx_), 0);
}
