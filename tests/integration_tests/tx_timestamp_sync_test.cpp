/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.h"
#include "tests.hpp"

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
  auto* p = (TxSyncPriv*)priv;
  uint64_t tai = tx_sync_publish_beat(p->beat);
  *next_frame_idx = p->sess->claim_frame_idx(tai);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai;
  return 0;
}

int tx_sync_video_notify_done(void* priv, uint16_t frame_idx,
                              struct st20_tx_frame_meta* meta) {
  auto* p = (TxSyncPriv*)priv;
  p->sess->complete_frame(frame_idx, meta->timestamp, meta->rtp_timestamp);
  return 0;
}

int tx_sync_audio_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st30_tx_frame_meta* meta) {
  auto* p = (TxSyncPriv*)priv;
  uint64_t tai = tx_sync_read_beat(p->beat);
  *next_frame_idx = p->sess->claim_frame_idx(tai);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai;
  return 0;
}

int tx_sync_audio_notify_done(void* priv, uint16_t frame_idx,
                              struct st30_tx_frame_meta* meta) {
  auto* p = (TxSyncPriv*)priv;
  p->sess->complete_frame(frame_idx, meta->timestamp, meta->rtp_timestamp);
  return 0;
}

int tx_sync_anc_next_frame(void* priv, uint16_t* next_frame_idx,
                           struct st40_tx_frame_meta* meta) {
  auto* p = (TxSyncPriv*)priv;
  uint64_t tai = tx_sync_read_beat(p->beat);
  *next_frame_idx = p->sess->claim_frame_idx(tai);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = tai;
  return 0;
}

int tx_sync_anc_notify_done(void* priv, uint16_t frame_idx,
                            struct st40_tx_frame_meta* meta) {
  auto* p = (TxSyncPriv*)priv;
  p->sess->complete_frame(frame_idx, meta->timestamp, meta->rtp_timestamp);
  return 0;
}

/* Every USER_TIMESTAMP session must report back the exact TAI it was asked
 * for, and an rtp_timestamp matching the same public conversion apps use. */
void tx_sync_check_self_consistent(const std::vector<TxSyncRecord>& records,
                                   uint32_t sampling_rate) {
  ASSERT_GT(records.size(), 0u);
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

void tx_timestamp_sync_test(enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;

  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled, one for tx and one as a reachable "
        "destination\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  TxSyncBeat beat;
  /* Safe to read before mtl_start(): none of the 3 sessions below set
   * USER_PACING, so start_tai's accuracy doesn't affect real transmit
   * scheduling -- revisit this read's placement if that ever changes. */
  beat.start_tai = mtl_ptp_read_time(m_handle);
  TxSyncSession video_sess, audio_sess, anc_sess;
  TxSyncPriv video_priv{&video_sess, &beat};
  TxSyncPriv audio_priv{&audio_sess, &beat};
  TxSyncPriv anc_priv{&anc_sess, &beat};

  struct st20_tx_ops ops20;
  memset(&ops20, 0, sizeof(ops20));
  ops20.name = "tx_timestamp_sync_st20";
  ops20.priv = &video_priv;
  ops20.num_port = 1;
  if (ctx->mcast_only)
    memcpy(ops20.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops20.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  snprintf(ops20.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops20.udp_port[MTL_SESSION_PORT_P] = TX_SYNC_ST20_UDP_PORT;
  ops20.packing = ST20_PACKING_BPM;
  ops20.type = ST20_TYPE_FRAME_LEVEL;
  ops20.width = 1280;
  ops20.height = 720;
  ops20.fps = ST_FPS_P50;
  ops20.fmt = ST20_FMT_YUV_422_10BIT;
  ops20.payload_type = TX_SYNC_ST20_PAYLOAD_TYPE;
  ops20.framebuff_cnt = TX_SYNC_FB_CNT;
  ops20.get_next_frame = tx_sync_video_next_frame;
  ops20.notify_frame_done = tx_sync_video_notify_done;
  ops20.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

  st20_tx_handle h20 = st20_tx_create(m_handle, &ops20);
  ASSERT_TRUE(h20 != NULL);

  struct st30_tx_ops ops30;
  memset(&ops30, 0, sizeof(ops30));
  ops30.name = "tx_timestamp_sync_st30";
  ops30.priv = &audio_priv;
  ops30.num_port = 1;
  if (ctx->mcast_only)
    memcpy(ops30.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops30.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  snprintf(ops30.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops30.udp_port[MTL_SESSION_PORT_P] = TX_SYNC_ST30_UDP_PORT;
  ops30.type = ST30_TYPE_FRAME_LEVEL;
  ops30.fmt = ST30_FMT_PCM16;
  ops30.channel = 2;
  ops30.sampling = ST30_SAMPLING_48K;
  ops30.ptime = ST30_PTIME_1MS;
  ops30.payload_type = TX_SYNC_ST30_PAYLOAD_TYPE;
  /* one packet per frame so notify_frame_done reports the instant this test
   * requested, instead of a later packet's continuation timestamp */
  int st30_pkt_size =
      st30_get_packet_size(ops30.fmt, ops30.ptime, ops30.sampling, ops30.channel);
  ASSERT_GT(st30_pkt_size, 0);
  ops30.framebuff_size = (uint32_t)st30_pkt_size;
  ops30.framebuff_cnt = TX_SYNC_FB_CNT;
  ops30.get_next_frame = tx_sync_audio_next_frame;
  ops30.notify_frame_done = tx_sync_audio_notify_done;
  ops30.flags |= ST30_TX_FLAG_USER_TIMESTAMP;

  st30_tx_handle h30 = st30_tx_create(m_handle, &ops30);
  ASSERT_TRUE(h30 != NULL);

  int audio_sampling_rate = st30_get_sample_rate(ops30.sampling);
  ASSERT_GT(audio_sampling_rate, 0);

  struct st40_tx_ops ops40;
  memset(&ops40, 0, sizeof(ops40));
  ops40.name = "tx_timestamp_sync_st40";
  ops40.priv = &anc_priv;
  ops40.num_port = 1;
  if (ctx->mcast_only)
    memcpy(ops40.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops40.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  snprintf(ops40.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops40.udp_port[MTL_SESSION_PORT_P] = TX_SYNC_ST40_UDP_PORT;
  ops40.type = ST40_TYPE_FRAME_LEVEL;
  ops40.fps = ST_FPS_P50;
  ops40.payload_type = TX_SYNC_ST40_PAYLOAD_TYPE;
  ops40.framebuff_cnt = TX_SYNC_FB_CNT;
  ops40.get_next_frame = tx_sync_anc_next_frame;
  ops40.notify_frame_done = tx_sync_anc_notify_done;
  ops40.flags |= ST40_TX_FLAG_USER_TIMESTAMP;

  st40_tx_handle h40 = st40_tx_create(m_handle, &ops40);
  ASSERT_TRUE(h40 != NULL);

  /* empty ANC frames: content is irrelevant to the timestamp sync under test */
  uint8_t* anc_frame_bufs[TX_SYNC_FB_CNT] = {};
  for (int i = 0; i < TX_SYNC_FB_CNT; i++) {
    anc_frame_bufs[i] = (uint8_t*)st_test_zmalloc(1);
    ASSERT_TRUE(anc_frame_bufs[i] != NULL);
    auto* dst = (struct st40_frame*)st40_tx_get_framebuffer(h40, i);
    ASSERT_TRUE(dst != NULL);
    dst->meta_num = 0;
    dst->data_size = 0;
    dst->data = anc_frame_bufs[i];
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);

  sleep(5);

  ret = mtl_stop(m_handle);
  EXPECT_GE(ret, 0);

  ret = st20_tx_free(h20);
  EXPECT_GE(ret, 0);
  ret = st30_tx_free(h30);
  EXPECT_GE(ret, 0);
  ret = st40_tx_free(h40);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < TX_SYNC_FB_CNT; i++) st_test_free(anc_frame_bufs[i]);

  std::vector<TxSyncRecord> video_records = video_sess.records();
  std::vector<TxSyncRecord> audio_records = audio_sess.records();
  std::vector<TxSyncRecord> anc_records = anc_sess.records();

  tx_sync_check_self_consistent(video_records, TX_SYNC_VIDEO_ANC_SAMPLING_RATE);
  tx_sync_check_self_consistent(audio_records, (uint32_t)audio_sampling_rate);
  tx_sync_check_self_consistent(anc_records, TX_SYNC_VIDEO_ANC_SAMPLING_RATE);

  /* record[0] is excluded from the match by tx_sync_count_matching_beats, so
   * the comparable population is one less than the smaller record set. */
  size_t comparable_beats = std::min(video_records.size(), anc_records.size());
  ASSERT_GT(comparable_beats, 1u);
  comparable_beats -= 1;
  int matched_beats = tx_sync_count_matching_beats(video_records, anc_records);
  /* Video and ancillary each publish/read the beat independently roughly once
   * per 20ms; over the 5s run that is ~250 beats. Requiring 90% to match
   * rules out a broken sync mechanism that only agrees by coincidence, while
   * tolerating the rare beat a session's own pacing skips or delays past. */
  EXPECT_GE(matched_beats, (int)(comparable_beats * 9 / 10));
}

} /* namespace */

TEST(TxUserTimestampSync, st20_st30_st40_stay_in_sync) {
  tx_timestamp_sync_test(ST_TEST_LEVEL_ALL);
}
