/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

/* A reclaiming (deep) port at 720p59.94: the frame before left its whole TX ring done
 * and not freed, and the PMD frees one rs batch in each burst, before the doorbell. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr uint64_t kOthersNs = 12000; /* the other tasklets of a scheduler round */
constexpr int kRoundCap = 100000;
constexpr uint16_t kRsThresh = kUtRlRsThresh;
constexpr uint16_t kSegsPerPkt = 2; /* a zero-copy chained video pkt */
/* hardware, 720p59.94 on an E810 VF: a first pad burst that frees no batch (p99) */
constexpr uint64_t kFirstBurstNs = 880;
constexpr uint64_t kBatchFreeNs = kUtRlColdBatchFreeNs;
constexpr uint64_t kCallNs = 2000; /* a visit each round of a busy scheduler */

class St20TxRlReclaim : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, 1280, 720, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    bps_ = ut_txv_rl_queue_bps(txv, kPortP);
    p_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    ut_txv_destroy(txv);
    window_ = (uint64_t)(warm_pkts_ * trs_);
  }

  /* From one ring drain before the warm-up window opens, the builder ring empty until
   * packet 0 comes one window before the open, or at once, a visit each round until
   * packet 0 is queued. */
  ut_trs_ctx* Frame(uint16_t rs_thresh, bool pkt0_at_once = false) {
    const uint64_t open = kTargetTsc - window_;
    const uint32_t batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
    const uint64_t drain = (uint64_t)(ut_trs_deep_nb_tx_desc() / kSegsPerPkt * trs_);
    uint64_t now = open - drain;
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    ut_trs_set_shaper_bucket(ctx, kPortP, kBurstBytes * 1e9L / bps_);
    ut_trs_set_mock_tsc_script(ctx, &now, 1);
    const uint64_t d[2] = {kFirstBurstNs, 200}, g[2] = {300, 250};
    ut_trs_set_burst_cost(ctx, d, g, 2);
    ut_trs_set_pmd_done_batches(ctx, rs_thresh, batches, kBatchFreeNs);
    bool queued = false;
    uint64_t at = now; /* the mock clock */
    for (int round = 0; round < kRoundCap && !ut_trs_real_send_count(ctx); round++) {
      if (!queued && (pkt0_at_once || now >= open - window_)) {
        ut_trs_enqueue_first_pkt(ctx, kTargetTsc);
        queued = true;
      }
      ut_trs_advance_mock_tsc(ctx, now - at);
      ut_trs_call_rl_tasklet(ctx);
      at = std::max(now, ut_trs_last_tsc(ctx));
      now = at + kOthersNs;
    }
    return ctx;
  }

  /* A frame of segs-descriptor pkts fills the NIC TX queue; its last pkts leave it full
   * of pkts the shaper has yet to send. Returns at the first visit with none to send. */
  ut_trs_ctx* FrameOnTheWire(uint16_t segs) {
    ut_trs_ctx* ctx = ut_trs_create();
    if (ut_trs_setup_stream(ctx, trs_, ut_trs_deep_nb_tx_desc(), segs) < 0) return ctx;
    ut_trs_set_rl_port(ctx, kPortP);
    ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
    ut_trs_stream(ctx, (uint64_t)(ut_trs_deep_nb_tx_desc() / segs * trs_), kCallNs);
    for (int i = 0; i < kRoundCap && !ut_trs_reclaim_calls(ctx); i++) Visit(ctx);
    return ctx;
  }

  void Visit(ut_trs_ctx* ctx) {
    ut_trs_advance_mock_tsc(ctx, kCallNs);
    ut_trs_call_rl_tasklet(ctx);
  }

  uint64_t bps_ = 0;
  long double p_ = 0, credit_ = 0, trs_ = 0;
  uint32_t warm_pkts_ = 0;
  uint64_t window_ = 0;
};

/* The waiting visits free the done batches down to those the PMD keeps, so the first pad
 * burst, with nothing queued yet, frees none: a first burst that frees a batch counts a
 * stall, one that frees none does not. The kept ones go in the bursts behind it, which
 * the queued pads cover. */
TEST_F(St20TxRlReclaim, WaitFreesTheFrameBeforeAheadOfTheWarmUp) {
  ASSERT_GT(kFirstBurstNs + kBatchFreeNs, ut_trs_rl_stall_ns());
  ASSERT_LT(kFirstBurstNs, ut_trs_rl_stall_ns());
  ut_trs_ctx* ctx = Frame(kRsThresh);
  EXPECT_LE(ut_trs_pmd_pad_burst_frees(ctx), ut_trs_rl_kept_batches());
  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx));
  EXPECT_LT(ut_trs_last_real_send_tsc(ctx), kTargetTsc);
  ut_trs_destroy(ctx);
}

/* Packet 0 dequeued while the frame before still drains: the visits waiting for its
 * window free the done batches, or the first pad burst frees one and stalls past what
 * absorb takes at 720p59.94, so packet 0 is held. */
TEST_F(St20TxRlReclaim, WaitForTheWindowFreesTheFrameBefore) {
  ut_trs_ctx* ctx = Frame(kRsThresh, true);
  EXPECT_LE(ut_trs_pmd_pad_burst_frees(ctx), ut_trs_rl_kept_batches());
  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx));
  EXPECT_LT(ut_trs_last_real_send_tsc(ctx), kTargetTsc);
  ut_trs_destroy(ctx);
}

/* a port that does not reclaim gets no empty bursts */
TEST_F(St20TxRlReclaim, NoEmptyBurstWithoutReclaim) {
  ut_trs_ctx* ctx = Frame(0);
  EXPECT_EQ(0u, ut_trs_reclaim_calls(ctx));
  ut_trs_destroy(ctx);
}

/* An idle session, its builder ring empty, visited every round for a second: a reclaim
 * at most once a batch time (rs_thresh / 2 NORMAL pkts of shaper time), plus the calls
 * that catch up the ring's batches at the start; and no fewer, so a frame's batches are
 * freed as they complete. */
TEST_F(St20TxRlReclaim, IdleSessionReclaimsOnceABatchTime) {
  const uint64_t span = 1000000000;
  uint64_t now = kTargetTsc;
  ut_trs_ctx* ctx = ut_trs_create();
  ut_trs_set_trs(ctx, trs_);
  ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
  ut_trs_set_drain(ctx, kPortP, p_);
  ut_trs_set_credit(ctx, kPortP, credit_);
  ut_trs_set_mock_tsc_script(ctx, &now, 1);
  ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
  while (ut_trs_last_tsc(ctx) < now + span) {
    ut_trs_call_rl_tasklet(ctx);
    ut_trs_advance_mock_tsc(ctx, kOthersNs);
  }
  const long double batch_ns = kRsThresh / 2 * p_;
  const uint32_t ring_batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
  const uint32_t calls = ut_trs_reclaim_calls(ctx);
  EXPECT_LE(calls, span / batch_ns + ring_batches + 1);
  EXPECT_GE(calls, span / batch_ns - 1);
  ut_trs_destroy(ctx);
}

/* an st22 session keeps main's RL path: idle on a reclaiming port, it reclaims nothing */
TEST_F(St20TxRlReclaim, IdleSt22SessionReclaimsNothing) {
  uint64_t now = kTargetTsc;
  ut_trs_ctx* ctx = ut_trs_create();
  ut_trs_set_trs(ctx, trs_);
  ut_trs_set_st22(ctx);
  ut_trs_set_mock_tsc_script(ctx, &now, 1);
  ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
  for (int call = 0; call < 1000; call++) {
    ut_trs_call_rl_tasklet(ctx);
    ut_trs_advance_mock_tsc(ctx, kOthersNs);
  }
  EXPECT_EQ(0u, ut_trs_reclaim_calls(ctx));
  ut_trs_destroy(ctx);
}

/* The frame before still on the wire, its oldest rs batch done only once the shaper sent
 * it: from the visit after its last pkts, an empty burst at most once a batch time, and
 * none ahead for the batches the queue has yet to send. */
TEST_F(St20TxRlReclaim, NoEmptyBurstAheadOfTheWire) {
  ut_trs_ctx* ctx = FrameOnTheWire(kSegsPerPkt);
  ASSERT_GE(ut_trs_stream_nic_desc_used(ctx), ut_trs_deep_nb_tx_desc() - kSegsPerPkt);
  const long double ahead = ut_trs_stream_ahead_ns(ctx);
  const uint64_t end = ut_trs_last_tsc(ctx) + (uint64_t)ahead;
  while (ut_trs_last_tsc(ctx) < end) Visit(ctx);
  const long double batch_ns = kRsThresh / 2 * trs_;
  EXPECT_LE(ut_trs_reclaim_calls(ctx), 1 + ahead / batch_ns + 1);
  ut_trs_destroy(ctx);
}

/* The same with the PMD, of chained or copied pkts: each rs batch is done once the shaper
 * sent its last pkt. Two batch times after the wire the visits have freed them down to
 * those the PMD keeps, so the next warm-up's first pad burst frees none. */
TEST_F(St20TxRlReclaim, VisitsFreeTheBatchesAsTheWireSendsThem) {
  for (uint16_t segs : {kSegsPerPkt, (uint16_t)1}) {
    ut_trs_ctx* ctx = FrameOnTheWire(segs);
    const uint32_t batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
    const long double batch_ns = kRsThresh / segs * trs_;
    const long double wire = ut_trs_last_tsc(ctx) + ut_trs_stream_ahead_ns(ctx);
    uint32_t done = 0;
    while (ut_trs_last_tsc(ctx) < wire + 2 * batch_ns) {
      /* the batches the shaper sent by now, oldest first */
      const long double left = std::max(wire - ut_trs_last_tsc(ctx), 0.0L);
      const uint32_t sent = batches - std::min(batches, (uint32_t)ceill(left / batch_ns));
      if (sent > done) ut_trs_add_pmd_done_batches(ctx, sent - done, kBatchFreeNs);
      done = std::max(done, sent);
      Visit(ctx);
    }
    ASSERT_EQ(batches, done) << segs;
    EXPECT_LE(done - ut_trs_pmd_frees(ctx), ut_trs_rl_kept_batches()) << segs;
    ut_trs_destroy(ctx);
  }
}

/* a thread stall at an instant inside a batch free delays the burst's doorbell by its
 * length */
TEST_F(St20TxRlReclaim, ThreadStallInsideABatchFreeDelaysTheBurst) {
  uint64_t now = kTargetTsc;
  ut_trs_ctx* ctx = ut_trs_create();
  ut_trs_set_trs(ctx, trs_);
  ut_trs_set_drain(ctx, kPortP, p_);
  ut_trs_set_credit(ctx, kPortP, credit_);
  ut_trs_set_mock_tsc_script(ctx, &now, 1);
  ut_trs_set_pmd_done_batches(ctx, kRsThresh, ut_trs_rl_kept_batches() + 1, kBatchFreeNs);
  const uint64_t start = ut_trs_get_mock_tsc(ctx);
  const uint64_t at = start + kBatchFreeNs / 2, ns = 50000;
  ut_trs_set_cpu_stalls(ctx, &at, &ns, 1);
  ASSERT_EQ(1u, ut_trs_call_burst_pad(ctx));
  ASSERT_EQ(1u, ut_trs_pmd_frees(ctx));
  EXPECT_GE(ut_trs_get_last_burst_succ_tsc(ctx), start + kBatchFreeNs + ns);
  ut_trs_destroy(ctx);
}

}  // namespace
