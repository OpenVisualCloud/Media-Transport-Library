/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

#define TX_SYNC_FB_CNT (3)
/* ST_FPS_P50 period, exact in ns so no rational rounding is involved */
#define TX_SYNC_BEAT_PERIOD_NS (20 * 1000 * 1000ULL)
/* ST2110-10 media clock shared by video and ancillary RTP timestamps */
#define TX_SYNC_VIDEO_ANC_SAMPLING_RATE (90 * 1000)
#define TX_SYNC_ST20_UDP_PORT (41000)
#define TX_SYNC_ST30_UDP_PORT (41010)
#define TX_SYNC_ST40_UDP_PORT (41020)
#define TX_SYNC_ST20_PAYLOAD_TYPE (112)
#define TX_SYNC_ST30_PAYLOAD_TYPE (111)
#define TX_SYNC_ST40_PAYLOAD_TYPE (113)

namespace {

using ScopedSt20Tx = mtl_test::ScopedHandle<st20_tx_handle, st20_tx_free>;
using ScopedSt30Tx = mtl_test::ScopedHandle<st30_tx_handle, st30_tx_free>;
using ScopedSt40Tx = mtl_test::ScopedHandle<st40_tx_handle, st40_tx_free>;

struct TxSyncRecord {
  uint64_t requested_tai;
  uint64_t reported_timestamp;
  uint32_t reported_rtp_timestamp;
};

/* Correlates the TAI instant a get_next_frame call requested for a frame slot
 * with the rtp_timestamp notify_frame_done later reports for that same slot. */
class TxSyncSession {
 public:
  uint16_t claim_frame_idx(uint64_t requested_tai) {
    std::lock_guard<std::mutex> lck(mtx_);
    uint16_t idx = next_fb_idx_;
    pending_tai_[idx] = requested_tai;
    next_fb_idx_ = (next_fb_idx_ + 1) % TX_SYNC_FB_CNT;
    return idx;
  }

  void complete_frame(uint16_t idx, uint64_t reported_timestamp,
                      uint32_t reported_rtp_timestamp) {
    std::lock_guard<std::mutex> lck(mtx_);
    records_.push_back({pending_tai_[idx], reported_timestamp, reported_rtp_timestamp});
  }

  size_t size() {
    std::lock_guard<std::mutex> lck(mtx_);
    return records_.size();
  }

  std::vector<TxSyncRecord> records() {
    std::lock_guard<std::mutex> lck(mtx_);
    return records_;
  }

 private:
  std::mutex mtx_;
  uint64_t pending_tai_[TX_SYNC_FB_CNT] = {};
  uint16_t next_fb_idx_ = 0;
  std::vector<TxSyncRecord> records_;
};

/* Video publishes the shared beat; audio and ancillary read whatever instant
 * is currently in force so all three request the identical wall-clock intent. */
struct TxSyncBeat {
  std::atomic<uint64_t> current_tai{0};
  uint64_t start_tai = 0;
  uint64_t beat_count = 0;
};

struct TxSyncPriv {
  TxSyncSession* sess;
  TxSyncBeat* beat;
};

uint64_t tx_sync_publish_beat(TxSyncBeat* beat) {
  uint64_t tai = beat->start_tai + beat->beat_count * TX_SYNC_BEAT_PERIOD_NS;
  beat->beat_count++;
  beat->current_tai.store(tai, std::memory_order_relaxed);
  return tai;
}

uint64_t tx_sync_read_beat(TxSyncBeat* beat) {
  uint64_t tai = beat->current_tai.load(std::memory_order_relaxed);
  return tai ? tai : beat->start_tai;
}

int tx_sync_video_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st20_tx_frame_meta* meta) {
  auto* p = static_cast<TxSyncPriv*>(priv);
  uint64_t tai = tx_sync_publish_beat(p->beat);
  *next_frame_idx = p->sess->claim_frame_idx(tai);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai;
  return 0;
}

template <typename Meta>
int tx_sync_follower_next_frame(void* priv, uint16_t* next_frame_idx, Meta* meta) {
  auto* p = static_cast<TxSyncPriv*>(priv);
  uint64_t tai = tx_sync_read_beat(p->beat);
  *next_frame_idx = p->sess->claim_frame_idx(tai);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai;
  return 0;
}

template <typename Meta>
int tx_sync_notify_done(void* priv, uint16_t frame_idx, Meta* meta) {
  auto* p = static_cast<TxSyncPriv*>(priv);
  p->sess->complete_frame(frame_idx, meta->timestamp, meta->rtp_timestamp);
  return 0;
}

/* Every USER_TIMESTAMP session must report back the exact TAI it was asked
 * for, and an rtp_timestamp matching the same public conversion apps use. */
void tx_sync_check_self_consistent(const std::vector<TxSyncRecord>& records,
                                   uint32_t sampling_rate) {
  ASSERT_GT(records.size(), 1u);
  /* skip the first record: a follower session can still observe the beat's
   * zero/start_tai fallback before video publishes its first beat */
  for (size_t i = 1; i < records.size(); i++) {
    const TxSyncRecord& rec = records[i];
    EXPECT_EQ(rec.reported_timestamp, rec.requested_tai);
    EXPECT_EQ(rec.reported_rtp_timestamp,
              st10_tai_to_media_clk(rec.requested_tai, sampling_rate));
  }
}

/* Video and ancillary share the 90kHz media clock: for every beat both sides
 * requested, they must have written the identical rtp_timestamp on the wire.
 * That is what "staying in sync" means on the actual transmitted packets.
 * Skips index 0 in both inputs like tx_sync_check_self_consistent: record[0]
 * can be a false match, since video's real first publish and a follower's
 * zero/start_tai startup fallback both happen to equal beat.start_tai. */
int tx_sync_count_matching_beats(const std::vector<TxSyncRecord>& a,
                                 const std::vector<TxSyncRecord>& b) {
  int matched = 0;
  for (size_t i = 1; i < a.size(); i++) {
    for (size_t j = 1; j < b.size(); j++) {
      if (a[i].requested_tai == b[j].requested_tai) {
        EXPECT_EQ(a[i].reported_rtp_timestamp, b[j].reported_rtp_timestamp);
        matched++;
        break;
      }
    }
  }
  return matched;
}

} /* namespace */

TEST(TxUserTimestampSync, st20_st30_st40_stay_in_sync) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  /* Declare session state and empty ANC buffers before handles and ScopedMtlStart
   * so RAII teardown runs mtl_stop -> tx_free -> buffer/session destruction. */
  TxSyncBeat beat;
  /* Safe to read before mtl_start(): none of the 3 sessions below set
   * USER_PACING, so start_tai's accuracy does not affect real transmit
   * scheduling -- revisit this read's placement if that ever changes. */
  beat.start_tai = mtl_ptp_read_time(m_handle);
  TxSyncSession video_sess, audio_sess, anc_sess;
  TxSyncPriv video_priv{&video_sess, &beat};
  TxSyncPriv audio_priv{&audio_sess, &beat};
  TxSyncPriv anc_priv{&anc_sess, &beat};
  /* empty ANC frames: content is irrelevant to the timestamp sync under test */
  std::array<uint8_t, TX_SYNC_FB_CNT> anc_frame_bufs{};

  struct st20_tx_ops ops20 = {};
  ops20.name = "tx_timestamp_sync_st20";
  ops20.priv = &video_priv;
  ops20.framebuff_cnt = TX_SYNC_FB_CNT;
  mtl_test::fill_raw_tx_port(ops20, ctx, TX_SYNC_ST20_UDP_PORT,
                             TX_SYNC_ST20_PAYLOAD_TYPE);
  ops20.packing = ST20_PACKING_BPM;
  ops20.type = ST20_TYPE_FRAME_LEVEL;
  ops20.width = 1280;
  ops20.height = 720;
  ops20.fps = ST_FPS_P50;
  ops20.fmt = ST20_FMT_YUV_422_10BIT;
  ops20.get_next_frame = tx_sync_video_next_frame;
  ops20.notify_frame_done = tx_sync_notify_done<struct st20_tx_frame_meta>;
  ops20.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

  ScopedSt20Tx h20(st20_tx_create(m_handle, &ops20));
  ASSERT_TRUE(h20);

  struct st30_tx_ops ops30 = {};
  ops30.name = "tx_timestamp_sync_st30";
  ops30.priv = &audio_priv;
  ops30.framebuff_cnt = TX_SYNC_FB_CNT;
  mtl_test::fill_raw_tx_port(ops30, ctx, TX_SYNC_ST30_UDP_PORT,
                             TX_SYNC_ST30_PAYLOAD_TYPE);
  ops30.type = ST30_TYPE_FRAME_LEVEL;
  ops30.fmt = ST30_FMT_PCM16;
  ops30.channel = 2;
  ops30.sampling = ST30_SAMPLING_48K;
  ops30.ptime = ST30_PTIME_1MS;
  /* one packet per frame so notify_frame_done reports the instant this test
   * requested, instead of a later packet's continuation timestamp */
  int st30_pkt_size =
      st30_get_packet_size(ops30.fmt, ops30.ptime, ops30.sampling, ops30.channel);
  ASSERT_GT(st30_pkt_size, 0);
  ops30.framebuff_size = static_cast<uint32_t>(st30_pkt_size);
  ops30.get_next_frame = tx_sync_follower_next_frame<struct st30_tx_frame_meta>;
  ops30.notify_frame_done = tx_sync_notify_done<struct st30_tx_frame_meta>;
  ops30.flags |= ST30_TX_FLAG_USER_TIMESTAMP;

  ScopedSt30Tx h30(st30_tx_create(m_handle, &ops30));
  ASSERT_TRUE(h30);

  int audio_sampling_rate = st30_get_sample_rate(ops30.sampling);
  ASSERT_GT(audio_sampling_rate, 0);

  struct st40_tx_ops ops40 = {};
  ops40.name = "tx_timestamp_sync_st40";
  ops40.priv = &anc_priv;
  ops40.framebuff_cnt = TX_SYNC_FB_CNT;
  mtl_test::fill_raw_tx_port(ops40, ctx, TX_SYNC_ST40_UDP_PORT,
                             TX_SYNC_ST40_PAYLOAD_TYPE);
  ops40.type = ST40_TYPE_FRAME_LEVEL;
  ops40.fps = ST_FPS_P50;
  ops40.get_next_frame = tx_sync_follower_next_frame<struct st40_tx_frame_meta>;
  ops40.notify_frame_done = tx_sync_notify_done<struct st40_tx_frame_meta>;
  ops40.flags |= ST40_TX_FLAG_USER_TIMESTAMP;

  ScopedSt40Tx h40(st40_tx_create(m_handle, &ops40));
  ASSERT_TRUE(h40);

  for (int i = 0; i < TX_SYNC_FB_CNT; i++) {
    auto* dst = static_cast<struct st40_frame*>(st40_tx_get_framebuffer(h40.get(), i));
    ASSERT_NE(dst, nullptr);
    dst->meta_num = 0;
    dst->data_size = 0;
    dst->data = &anc_frame_bufs[i];
  }

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  ASSERT_GE(mtl_run.start(), 0);

  /* Wait for >= 50 completed records on all 3 sessions: 50 beats at 50 fps
   * takes ~1.0 s vs legacy sleep(5), giving 49 comparable beats (up to 5
   * misses of slack) so phase offset cannot flake the 90% match check. */
  constexpr size_t kMinSyncRecords = 50;
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return video_sess.size() >= kMinSyncRecords &&
               audio_sess.size() >= kMinSyncRecords && anc_sess.size() >= kMinSyncRecords;
      },
      6000));

  EXPECT_GE(mtl_run.stop(), 0);

  std::vector<TxSyncRecord> video_records = video_sess.records();
  std::vector<TxSyncRecord> audio_records = audio_sess.records();
  std::vector<TxSyncRecord> anc_records = anc_sess.records();

  tx_sync_check_self_consistent(video_records, TX_SYNC_VIDEO_ANC_SAMPLING_RATE);
  tx_sync_check_self_consistent(audio_records,
                                static_cast<uint32_t>(audio_sampling_rate));
  tx_sync_check_self_consistent(anc_records, TX_SYNC_VIDEO_ANC_SAMPLING_RATE);

  /* record[0] is excluded from the match by tx_sync_count_matching_beats, so
   * the comparable population is one less than the smaller record set. */
  size_t comparable_beats = std::min(video_records.size(), anc_records.size());
  ASSERT_GT(comparable_beats, 1u);
  comparable_beats -= 1;
  int matched_beats = tx_sync_count_matching_beats(video_records, anc_records);
  /* Video and ancillary each publish/read the beat independently roughly once
   * per 20 ms. Requiring 90% to match rules out a broken sync mechanism that
   * only agrees by coincidence, while tolerating a rare skipped beat. */
  EXPECT_GE(matched_beats, static_cast<int>(comparable_beats * 9 / 10));
}
