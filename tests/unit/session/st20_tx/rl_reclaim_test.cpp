/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

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
constexpr uint32_t kBulk = 4; /* ST_SESSION_MAX_BULK, what the builder adds a round */

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
    normal_ = ut_txv_normal_size(txv);
    pad_size_ = ut_txv_rl_pad_size(txv, kPortP);
    ut_txv_destroy(txv);
    pkt_ = st_tx_video_rl_pad_ns(p_, pad_size_, normal_);
    window_ = (uint64_t)(warm_pkts_ * trs_);
  }

  /* From drains ring drain times before the warm-up window opens, a visit each round
   * until packet 0 is queued, the builder ring empty until packet 0 comes one window
   * before the open, or at once. With pad_first the first visit bursts a pad of the frame
   * before. */
  ut_trs_ctx* Frame(uint16_t rs_thresh, bool pkt0_at_once = false, long double drains = 1,
                    bool pad_first = false) {
    const uint64_t open = kTargetTsc - window_;
    const uint32_t batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
    const uint64_t drain = (uint64_t)(ut_trs_deep_nb_tx_desc() / kSegsPerPkt * trs_);
    uint64_t now = open - (uint64_t)(drains * drain);
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    EXPECT_EQ(0, ut_trs_set_pad_sizes(ctx, normal_, pad_size_));
    ut_trs_set_shaper_bucket(ctx, kPortP, kBurstBytes * 1e9L / bps_);
    ut_trs_set_mock_tsc_script(ctx, &now, 1);
    const uint64_t d[2] = {kFirstBurstNs, 200}, g[2] = {300, 250};
    ut_trs_set_burst_cost(ctx, d, g, 2);
    ut_trs_set_pmd_done_batches(ctx, rs_thresh, batches, kBatchFreeNs);
    if (pad_first) ut_trs_set_pad_inflight_num(ctx, 1);
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

  /* stream mode, the builder adding a bulk a call */
  ut_trs_ctx* Stream() {
    ut_trs_ctx* ctx = ut_trs_create();
    if (ut_trs_setup_stream(ctx, trs_, ut_trs_deep_nb_tx_desc(), kSegsPerPkt) < 0)
      return ctx;
    ut_trs_set_rl_port(ctx, kPortP);
    ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
    ut_trs_set_stream_build(ctx, kBulk);
    return ctx;
  }

  /* The builder at twice the wire rate fills the NIC TX queue; then the thread stalls
   * stall_ns and the builder stops. Returns at the first visit after the last burst. */
  ut_trs_ctx* FrameEnd(uint64_t stall_ns) {
    ut_trs_ctx* ctx = Stream();
    const uint64_t round = (uint64_t)(kBulk / 2 * trs_);
    while (!ut_trs_inflight_num(ctx)) ut_trs_stream(ctx, round, round);
    stall_pkts_ = ut_trs_get_stat_pkts_burst(ctx);
    ut_trs_stall_stream(ctx, stall_ns);
    for (int i = 0; i < kRoundCap; i++) {
      const uint64_t burst = ut_trs_get_last_burst_succ_tsc(ctx);
      Visit(ctx);
      if (ut_trs_get_last_burst_succ_tsc(ctx) == burst && !ut_trs_inflight_num(ctx))
        break;
    }
    return ctx;
  }

  /* An idle session visited every kCallNs, past the calls that catch up the ring's
   * batches at its start, its reclaims on time */
  ut_trs_ctx* Idle(uint64_t* now) {
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    EXPECT_EQ(0, ut_trs_set_pad_sizes(ctx, normal_, pad_size_));
    ut_trs_set_shaper_bucket(ctx, kPortP, kBurstBytes * 1e9L / bps_);
    ut_trs_set_mock_tsc_script(ctx, now, 1);
    ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
    const long double batch_ns = kRsThresh / 2 * pkt_;
    const uint32_t ring_batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
    while (ut_trs_last_tsc(ctx) < *now + 2 * ring_batches * batch_ns) Visit(ctx);
    return ctx;
  }

  void Visit(ut_trs_ctx* ctx) {
    ut_trs_advance_mock_tsc(ctx, kCallNs);
    ut_trs_call_rl_tasklet(ctx);
  }

  uint64_t bps_ = 0;
  /* the pre-arm pad's drain, and the NORMAL pkt's */
  long double p_ = 0, pkt_ = 0, credit_ = 0, trs_ = 0;
  uint16_t normal_ = 0, pad_size_ = 0;
  uint32_t warm_pkts_ = 0;
  uint64_t window_ = 0;
  int stall_pkts_ = 0; /* the pkts FrameEnd() burst before its stall */
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
  ASSERT_EQ(0, ut_trs_set_pad_sizes(ctx, normal_, pad_size_));
  ut_trs_set_mock_tsc_script(ctx, &now, 1);
  ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, kBatchFreeNs);
  while (ut_trs_last_tsc(ctx) < now + span) {
    ut_trs_call_rl_tasklet(ctx);
    ut_trs_advance_mock_tsc(ctx, kOthersNs);
  }
  const long double batch_ns = kRsThresh / 2 * pkt_;
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

/* Frames always ready: each visit bursts the bulk the builder added that round and finds
 * the builder ring empty after it. The PMD frees a done batch in each burst, so once
 * the reclaims are on time no visit makes an empty one, and the bursts free the batches
 * as the wire sends them. */
TEST_F(St20TxRlReclaim, VisitsThatBurstMakeNoEmptyBurst) {
  ut_trs_ctx* ctx = Stream();
  const uint64_t round = (uint64_t)(kBulk * trs_);
  const uint32_t ring_pkts = ut_trs_deep_nb_tx_desc() / kSegsPerPkt;
  ut_trs_stream(ctx, (uint64_t)(ring_pkts * trs_), round);
  const uint32_t on_time = ut_trs_reclaim_calls(ctx);
  const int pkts = ut_trs_get_stat_pkts_burst(ctx);
  /* the pkts the wire has sent */
  auto wire = [&] {
    return ut_trs_get_stat_pkts_burst(ctx) - ut_trs_stream_ahead_ns(ctx) / trs_;
  };
  const long double from = wire();
  const uint32_t frees = ut_trs_pmd_frees(ctx);
  uint32_t done = 0;
  for (uint32_t r = 0; r < 10 * ring_pkts / kBulk; r++) {
    const uint32_t sent = (uint32_t)((wire() - from) * kSegsPerPkt / kRsThresh);
    if (sent > done) ut_trs_add_pmd_done_batches(ctx, sent - done, kBatchFreeNs);
    done = std::max(done, sent);
    ut_trs_stream(ctx, round, round);
  }
  ASSERT_GE(ut_trs_get_stat_pkts_burst(ctx) - pkts, (int)(10 * ring_pkts - kBulk));
  EXPECT_EQ(on_time, ut_trs_reclaim_calls(ctx));
  EXPECT_LE(done - (ut_trs_pmd_frees(ctx) - frees), ut_trs_rl_kept_batches());
  ut_trs_destroy(ctx);
}

/* A burst stands for one reclaim: whenever the bursts stop, the first empty burst comes a
 * batch time after the last one, not later */
TEST_F(St20TxRlReclaim, ReclaimsResumeABatchTimeAfterTheLastBurst) {
  const uint64_t round = (uint64_t)(kBulk * trs_);
  const uint64_t ring = (uint64_t)(ut_trs_deep_nb_tx_desc() / kSegsPerPkt * trs_);
  for (uint32_t stop = 0; stop < kRsThresh / 2 / kBulk; stop++) {
    ut_trs_ctx* ctx = Stream();
    ut_trs_stream(ctx, ring + stop * round, round);
    const uint32_t calls = ut_trs_reclaim_calls(ctx);
    const uint64_t burst = ut_trs_get_last_burst_succ_tsc(ctx);
    for (int i = 0; i < kRoundCap && ut_trs_reclaim_calls(ctx) == calls; i++) Visit(ctx);
    ASSERT_EQ(burst, ut_trs_get_last_burst_succ_tsc(ctx)) << stop;
    EXPECT_LE(ut_trs_last_tsc(ctx), burst + kRsThresh / 2 * trs_ + kCallNs) << stop;
    ut_trs_destroy(ctx);
  }
}

/* The frame before's last pad goes half a ring drain before the warm-up window, behind a
 * ring the wire has sent: the waits after that burst still catch up, so by packet 0 the
 * done batches are down to those the PMD keeps, and the warm-up pads freed no more (nor
 * the one that pad burst freed). */
TEST_F(St20TxRlReclaim, WaitAfterABurstStillFreesTheFrameBefore) {
  ut_trs_ctx* ctx = Frame(kRsThresh, false, 0.5L, true);
  const uint32_t batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
  EXPECT_LE(batches - ut_trs_pmd_frees(ctx), ut_trs_rl_kept_batches());
  EXPECT_LE(ut_trs_pmd_pad_burst_frees(ctx), 1 + ut_trs_rl_kept_batches());
  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx));
  EXPECT_LT(ut_trs_last_real_send_tsc(ctx), kTargetTsc);
  ut_trs_destroy(ctx);
}

/* After the frame's last burst, the NIC TX queue full or, after a stall, already sent:
 * two batch times after the wire end the visits have freed the done batches down to
 * those the PMD keeps. */
TEST_F(St20TxRlReclaim, VisitsAfterTheLastBurstFreeTheBatchesTheWireSent) {
  const uint64_t drain = (uint64_t)(ut_trs_deep_nb_tx_desc() / kSegsPerPkt * trs_);
  for (uint64_t stall : {(uint64_t)0, drain}) {
    ut_trs_ctx* ctx = FrameEnd(stall);
    const uint32_t batches = ut_trs_deep_nb_tx_desc() / kRsThresh;
    const long double batch_ns = kRsThresh / kSegsPerPkt * trs_;
    ASSERT_GT(ut_trs_get_stat_pkts_burst(ctx), stall_pkts_) << stall;
    if (stall)
      ASSERT_LT(ut_trs_stream_ahead_ns(ctx), batch_ns);
    else
      ASSERT_GE(ut_trs_stream_nic_desc_used(ctx), ut_trs_deep_nb_tx_desc() - kSegsPerPkt);
    const long double wire = ut_trs_last_tsc(ctx) + ut_trs_stream_ahead_ns(ctx);
    uint32_t done = 0;
    while (ut_trs_last_tsc(ctx) < wire + 2 * batch_ns) {
      const long double left = std::max(wire - ut_trs_last_tsc(ctx), 0.0L);
      const uint32_t sent = batches - std::min(batches, (uint32_t)ceill(left / batch_ns));
      if (sent > done) ut_trs_add_pmd_done_batches(ctx, sent - done, kBatchFreeNs);
      done = std::max(done, sent);
      Visit(ctx);
    }
    ASSERT_EQ(batches, done) << stall;
    EXPECT_LE(done - ut_trs_pmd_frees(ctx), ut_trs_rl_kept_batches()) << stall;
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
  ASSERT_EQ(0, ut_trs_set_pad_sizes(ctx, normal_, pad_size_));
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

/* The session start, attach, recovery and a hang move last_burst_succ_time_tsc with no
 * burst, which frees nothing: half a batch time after a reclaim, the next one is still
 * due a batch time after it */
TEST_F(St20TxRlReclaim, OnlyABurstDelaysAReclaim) {
  uint64_t now = kTargetTsc;
  ut_trs_ctx* ctx = Idle(&now);
  const long double batch_ns = kRsThresh / 2 * pkt_;
  uint32_t calls = ut_trs_reclaim_calls(ctx);
  while (ut_trs_reclaim_calls(ctx) == calls) Visit(ctx);
  const uint64_t last = ut_trs_last_tsc(ctx);
  calls = ut_trs_reclaim_calls(ctx);
  while (ut_trs_last_tsc(ctx) < last + batch_ns / 2) Visit(ctx);
  ASSERT_EQ(calls, ut_trs_reclaim_calls(ctx));
  ut_trs_set_last_burst_succ_tsc(ctx, ut_trs_last_tsc(ctx));
  for (int i = 0; i < kRoundCap && ut_trs_reclaim_calls(ctx) == calls; i++) Visit(ctx);
  EXPECT_LE(ut_trs_last_tsc(ctx), last + batch_ns + kCallNs);
  ut_trs_destroy(ctx);
}

/* An idle session, its reclaims on time, makes one RL burst of each kind when the next
 * call is due: the burst is that call, so neither the visit's second call nor the visits
 * a batch time after it make an empty one */
TEST_F(St20TxRlReclaim, EveryRlBurstKindCounts) {
  const char* kinds[] = {"pad inflight", "in-frame pad", "inflight2", "tail", "pre-arm"};
  const long double batch_ns = kRsThresh / 2 * pkt_;
  for (int kind = 0; kind < 5; kind++) {
    uint64_t now = kTargetTsc;
    ut_trs_ctx* ctx = Idle(&now);
    const uint64_t due = ut_trs_reclaim_tsc(ctx);
    ASSERT_GT(due, ut_trs_last_tsc(ctx)) << kinds[kind];
    ut_trs_advance_mock_tsc(ctx, due - ut_trs_last_tsc(ctx));
    const uint32_t calls = ut_trs_reclaim_calls(ctx);
    const uint32_t bursts = ut_trs_burst_call_count(ctx);
    const uint32_t pads = ut_trs_pad_send_count(ctx);
    switch (kind) {
      case 0:
        ut_trs_set_pad_inflight_num(ctx, 1);
        break;
      case 1: /* the pkt refused, its pad goes */
        ut_trs_set_pad_interval(ctx, 4);
        ut_trs_enqueue_mid_pkt(ctx);
        ut_trs_set_burst_refuse(ctx, bursts + 1, 1);
        break;
      case 2:
        ut_trs_set_inflight_num2(ctx, 1);
        break;
      case 3:
        ut_trs_set_inflight_num(ctx, 1);
        ut_trs_set_rl_tail(ctx, due);
        break;
      default:
        ut_trs_set_target_tsc(ctx, due + window_);
        ASSERT_TRUE(ut_trs_pre_arm(ctx));
        break;
    }
    ut_trs_call_rl_tasklet(ctx);
    ASSERT_GT(ut_trs_burst_call_count(ctx), bursts) << kinds[kind];
    if (kind != 2 && kind != 3) {
      ASSERT_GT(ut_trs_pad_send_count(ctx), pads) << kinds[kind];
    }
    while (ut_trs_last_tsc(ctx) + kCallNs < due + batch_ns) Visit(ctx);
    EXPECT_EQ(calls, ut_trs_reclaim_calls(ctx)) << kinds[kind];
    ut_trs_destroy(ctx);
  }
}

/* The NIC TX queue full, the wire stops a ring time, and the bursts are refused: no batch
 * is done, so none is owed a call. The wire resumes and the frame ends a batch time
 * later: from its last burst, an empty burst at most each batch the wire completes, and
 * one more. */
TEST_F(St20TxRlReclaim, AStuckWireOwesNoEmptyBurst) {
  ut_trs_ctx* ctx = Stream();
  const uint64_t round = (uint64_t)(kBulk / 2 * trs_);
  const uint64_t ring = (uint64_t)(ut_trs_deep_nb_tx_desc() / kSegsPerPkt * trs_);
  const long double batch_ns = kRsThresh / kSegsPerPkt * trs_;
  while (!ut_trs_inflight_num(ctx)) ut_trs_stream(ctx, round, round);
  ut_trs_stall_stream_wire(ctx, ring);
  const uint64_t resume = ut_trs_last_tsc(ctx) + ring;
  while (ut_trs_last_tsc(ctx) < resume) Visit(ctx);
  ut_trs_stream(ctx, (uint64_t)batch_ns, round);
  for (int i = 0; i < kRoundCap; i++) {
    const uint64_t burst = ut_trs_get_last_burst_succ_tsc(ctx);
    Visit(ctx);
    if (ut_trs_get_last_burst_succ_tsc(ctx) == burst && !ut_trs_inflight_num(ctx)) break;
  }
  ASSERT_GE(ut_trs_stream_nic_desc_used(ctx), ut_trs_deep_nb_tx_desc() - 2 * kRsThresh);
  const uint32_t calls = ut_trs_reclaim_calls(ctx);
  const long double ahead = ut_trs_stream_ahead_ns(ctx);
  const uint64_t end = ut_trs_last_tsc(ctx) + (uint64_t)ahead;
  while (ut_trs_last_tsc(ctx) < end) Visit(ctx);
  EXPECT_LE(ut_trs_reclaim_calls(ctx) - calls, ahead / batch_ns + 1);
  ut_trs_destroy(ctx);
}

}  // namespace
