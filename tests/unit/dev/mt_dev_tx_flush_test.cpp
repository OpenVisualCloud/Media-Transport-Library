/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_dpdk_flush_tx_queue() frees every mbuf queued before it, as teardown and recovery
 * need before they free a pool. A PMD freeing an mbuf only when it reuses the slot needs
 * 2 rings of pads pushed after them, each pad with 1 ms to go. Where the PMD frees a done
 * batch every burst (mt_if_tx_reclaim_rs_thresh()), a copy of the pad sent behind them
 * and 2 batches behind it is freed after the last of them, so the flush waits for that,
 * no longer than the ring takes to drain.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevTxFlush*'
 */

#include <gtest/gtest.h>

#include <vector>

#include "common/ut_common.h"
#include "common/ut_known_bug.h"
#include "common/ut_tx_ring.h"
#include "dev/mt_dev_harness.h"

namespace {

constexpr uint64_t kStartNs = 1000000000;
constexpr uint64_t kTickNs = 100;
constexpr uint64_t kPollNs = 10000; /* MT_DEV_TX_FLUSH_POLL_NS */
constexpr uint16_t kRs = 32;
constexpr unsigned int kSessionPool = 4095;

class MtDevTxFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0);
    pool_ = rte_pktmbuf_pool_create("ut_flush_session", kSessionPool, 0, 0, 128,
                                    (int)rte_socket_id());
    ASSERT_NE(pool_, nullptr);
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_clock_stop();
    ut_eth_reset();
    ut_txr_destroy(ring_);
    ut_dev_destroy_ctx(ctx_);
    rte_mempool_free(pool_);
  }
  /* an igc port, which the lazy loop flushes */
  void LazyPort(uint16_t nb_desc, uint64_t drain_ns) {
    ASSERT_EQ(ut_dev_set_tx_ring(ctx_, 1, nb_desc, kRs), 0);
    ASSERT_EQ(ut_dev_start_port(ctx_), 0);
    ASSERT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), 0);
    Attach(UT_TXR_FREE_ON_REUSE, nb_desc, kRs, drain_ns);
  }
  /* a deep DPDK 26.07 iavf port, which frees a done batch each burst */
  void EagerPort(uint16_t nb_desc, uint64_t drain_ns, uint16_t rs = kRs) {
    ut_dev_set_driver(ctx_, "net_iavf");
    ut_dev_set_tx_cleanup_frees(ctx_, true);
    ut_dev_set_tx_pacing(ctx_, ST21_TX_PACING_WAY_RL);
    ASSERT_TRUE(ut_dev_init_tx_deep(ctx_));
    ut_dev_set_link_speed(ctx_, 25000);
    ASSERT_EQ(ut_dev_set_tx_ring(ctx_, 1, nb_desc, rs), 0);
    ASSERT_EQ(ut_dev_start_port(ctx_), 0);
    ASSERT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), rs);
    Attach(UT_TXR_FREE_ON_CLEANUP, nb_desc, ut_dev_tx_free_thresh(ctx_, 0), drain_ns, rs);
  }
  void Attach(enum ut_txr_mode mode, uint16_t nb_desc, uint16_t free_thresh,
              uint64_t drain_ns, uint16_t rs = kRs) {
    ring_ = ut_txr_create(mode, nb_desc, rs, free_thresh, drain_ns);
    ASSERT_NE(ring_, nullptr);
    ASSERT_EQ(ut_dev_attach_tx_ring(ctx_, 0, ring_), 0);
    pad_refs_ = rte_mbuf_refcnt_read(ut_dev_pad(ctx_));
  }
  /* session pkts in the ring when the flush starts, at kStartNs */
  void Queue(unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
      struct rte_mbuf* m = rte_pktmbuf_alloc(pool_);
      ASSERT_NE(m, nullptr);
      ASSERT_EQ(ut_txr_xmit(ring_, &m, 1, kStartNs), 1);
    }
    descs_ = ut_txr_descs(ring_);
    xmits_ = ut_txr_xmits(ring_);
    ut_clock_start(kStartNs, kTickNs);
  }
  int Flush() {
    return ut_dev_flush_tx_queue(ctx_, 0);
  }
  unsigned int SessionPktsHeld() {
    return rte_mempool_in_use_count(pool_);
  }
  uint64_t FlushDescs() {
    return ut_txr_descs(ring_) - descs_;
  }
  uint64_t FlushXmits() {
    return ut_txr_xmits(ring_) - xmits_;
  }
  void ExpectPadRefsHeldByTheRingOnly() {
    struct rte_mbuf* pad = ut_dev_pad(ctx_);
    EXPECT_EQ(rte_mbuf_refcnt_read(pad), pad_refs_ + ut_txr_holds(ring_, pad));
  }
  struct rte_mempool* pool_ = nullptr;
  ut_dev_ctx* ctx_ = nullptr;
  struct ut_txr* ring_ = nullptr;
  uint16_t pad_refs_ = 0;
  uint64_t descs_ = 0;
  uint64_t xmits_ = 0;
};

TEST_F(MtDevTxFlushTest, LazyPortSendsTwoRingsOfPads) {
  LazyPort(128, 100);
  Queue(100);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 2u * 128);
  EXPECT_EQ(FlushXmits(), 2u * 128);
  EXPECT_EQ(SessionPktsHeld(), 0u);
}

/* a 32 ms rs batch, an audio RL queue: most pads time out, the ring still empties */
TEST_F(MtDevTxFlushTest, LazyPortOfASlowQueueStillFreesTheRing) {
  LazyPort(512, 1000000);
  Queue(511);

  EXPECT_EQ(Flush(), 0);

  EXPECT_LT(FlushDescs(), 2u * 512);
  EXPECT_EQ(SessionPktsHeld(), 0u);
}

/* a 26.07 iavf port that is not deep frees a batch each burst too, yet flushes lazily */
TEST_F(MtDevTxFlushTest, NotDeepIavfPortSendsTwoRingsOfPads) {
  ut_dev_set_driver(ctx_, "net_iavf");
  ut_dev_set_tx_cleanup_frees(ctx_, true);
  ut_dev_set_tx_pacing(ctx_, ST21_TX_PACING_WAY_TSC);
  ASSERT_FALSE(ut_dev_init_tx_deep(ctx_));
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, 1, 512, kRs), 0);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  Attach(UT_TXR_FREE_ON_CLEANUP, 512, kRs, 100);
  Queue(100);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 2u * 512);
  EXPECT_EQ(SessionPktsHeld(), 0u);
}

/* aligned or not, the copy's batch is the oldest only with 2 rs batches in use */
class MtDevTxFlushEagerTest : public MtDevTxFlushTest,
                              public ::testing::WithParamInterface<unsigned int> {};

TEST_P(MtDevTxFlushEagerTest, EagerFlushSendsTheMarkAndTwoBatches) {
  constexpr uint64_t kDrainNs = 1000;
  const unsigned int queued = GetParam();
  EagerPort(2048, kDrainNs);
  Queue(queued);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 1u + 2 * kRs);
  EXPECT_EQ(SessionPktsHeld(), 0u);
  EXPECT_EQ(ut_dev_sys_tx_pool_in_use(ctx_), 1u); /* the pad, no copy left */
  ExpectPadRefsHeldByTheRingOnly();
  /* the copy's batch is done with its last descriptor, freed by the next poll */
  const uint64_t batch_end = (uint64_t)(queued / kRs + 1) * kRs;
  const uint64_t done_ns = kStartNs + batch_end * kDrainNs;
  EXPECT_GE(ut_clock_ns(), done_ns);
  EXPECT_LE(ut_clock_ns(), done_ns + kPollNs + 10 * kTickNs);
  /* the mark, 2 bursts of 32 pads, then a burst of none a poll, the last one at return */
  EXPECT_EQ(FlushXmits(), 3 + (ut_clock_ns() - kStartNs) / kPollNs + 1);
}

INSTANTIATE_TEST_SUITE_P(Queued, MtDevTxFlushEagerTest, ::testing::Values(1024u, 1000u));

/* a hang with room in the ring: the queue's bytes at its rate, or at the line rate */
class MtDevTxFlushHungTest : public MtDevTxFlushTest,
                             public ::testing::WithParamInterface<uint64_t> {};

TEST_P(MtDevTxFlushHungTest, EagerFlushOnHungQueueGivesUpAtTheDeadline) {
  const uint64_t bps = GetParam();
  EagerPort(2048, UT_TXR_HUNG);
  ut_dev_set_tx_queue_bps(ctx_, 0, bps);
  Queue(1000);

  EXPECT_LT(Flush(), 0);

  const uint64_t rate = bps ? bps : 25000ull * 1000 * 1000 / 8;
  const uint64_t deadline = kStartNs + 2000000 + 2048ull * 1538 * 1000000000 / rate;
  EXPECT_GE(ut_clock_ns(), deadline);
  EXPECT_LE(ut_clock_ns(), deadline + kPollNs + 10 * kTickNs);
  ExpectPadRefsHeldByTheRingOnly();
  ut_txr_destroy(ring_);
  ring_ = nullptr;
  EXPECT_EQ(ut_dev_sys_tx_pool_in_use(ctx_), 1u); /* the copy left with the ring */
}

INSTANTIATE_TEST_SUITE_P(Rate, MtDevTxFlushHungTest,
                         ::testing::Values(125000000ull, 0ull));

/* an audio RL queue full at teardown, a batch 16 or 32 ms: each one drained lets more in,
 * 2 batches for a burst of 32 pads at 16 */
class MtDevTxFlushSlowTest : public MtDevTxFlushTest,
                             public ::testing::WithParamInterface<uint16_t> {};

TEST_P(MtDevTxFlushSlowTest, EagerFlushOfASlowFullQueueStillFreesTheRing) {
  constexpr uint64_t kDrainNs = 1000000; /* 1538 B at the queue's rate */
  const uint16_t rs = GetParam();
  EagerPort(2048, kDrainNs, rs);
  ut_dev_set_tx_queue_bps(ctx_, 0, 1538000);
  Queue(2047);
  ut_clock_start(kStartNs, 1000);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 1u + 2 * rs);
  EXPECT_EQ(SessionPktsHeld(), 0u);
  EXPECT_EQ(ut_dev_sys_tx_pool_in_use(ctx_), 1u);
  ExpectPadRefsHeldByTheRingOnly();
  /* the mark's batch is the newest, done with the mark */
  EXPECT_GE(ut_clock_ns(), kStartNs + 2048 * kDrainNs);
  EXPECT_LE(ut_clock_ns(), kStartNs + 2048 * kDrainNs + kPollNs + 10000);
}

INSTANTIATE_TEST_SUITE_P(Rs, MtDevTxFlushSlowTest, ::testing::Values(32, 16));

/* the least ring, its batches draining a little slower than its rate: the third batch
 * the pads wait for is done only past the deadline, so the 2 drained ones let in the mark
 * and all but the last pad */
TEST_F(MtDevTxFlushTest, EagerFlushSendsNoLaterThanTheDeadline) {
  EagerPort(3 * kRs, 1050000);
  ut_dev_set_tx_queue_bps(ctx_, 0, 1538000);
  Queue(3 * kRs - 1);
  ut_clock_start(kStartNs, 1000);

  EXPECT_LT(Flush(), 0);

  const uint64_t deadline = kStartNs + 2000000 + 3ull * kRs * 1000000;
  EXPECT_EQ(FlushDescs(), 2u * kRs);
  EXPECT_LE(ut_clock_ns(), deadline + 10000);
  ExpectPadRefsHeldByTheRingOnly();
}

TEST_F(MtDevTxFlushTest, EagerFlushOfAFullHungRingFreesTheRefusedMark) {
  EagerPort(2048, UT_TXR_HUNG);
  Queue(2047);

  EXPECT_LT(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 0u);
  EXPECT_EQ(ut_dev_sys_tx_pool_in_use(ctx_), 1u);
  ExpectPadRefsHeldByTheRingOnly();
  /* nothing went in for 2 ms and a batch's drain at the line rate */
  const uint64_t stall_ns =
      2000000 + kRs * 1538ull * 1000000000 / (25000ull * 1000000 / 8);
  EXPECT_GE(ut_clock_ns(), kStartNs + stall_ns);
  EXPECT_LE(ut_clock_ns(), kStartNs + stall_ns + 10 * kTickNs);
}

TEST_F(MtDevTxFlushTest, EagerFlushRefundsThePadsTheRingRefused) {
  EagerPort(2048, UT_TXR_HUNG);
  Queue(2000);

  EXPECT_LT(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 47u);
  ExpectPadRefsHeldByTheRingOnly();
}

/* The bounded flush and recovery of a deep port send the port pad from any thread: a ref
 * of its own keeps its count above 1, where rte_mbuf_refcnt_update() adds atomically,
 * and the uninit drops it. */
TEST_F(MtDevTxFlushTest, PortPadKeepsARefOfItsOwn) {
  EagerPort(2048, 1000);
  struct rte_mbuf* pad = ut_dev_pad(ctx_);
  Queue(1000);
  EXPECT_EQ(rte_mbuf_refcnt_read(pad), 2);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(rte_mbuf_refcnt_read(pad), 2 + ut_txr_holds(ring_, pad));
  ut_txr_destroy(ring_);
  ring_ = nullptr;
  ut_dev_free_pad(ctx_);
  EXPECT_EQ(ut_dev_sys_tx_pool_in_use(ctx_), 0u);
}

/* the TX check sends the port pad in place of an invalid pkt, whose ref the PMD frees */
TEST_F(MtDevTxFlushTest, PadInPlaceOfAnInvalidPktKeepsItsOwnRef) {
  EagerPort(2048, 1000);
  struct rte_mbuf* pad = ut_dev_pad(ctx_);
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(pool_);
  ASSERT_NE(pkt, nullptr);

  ASSERT_EQ(ut_dev_tx_pkt_check(ctx_, &pkt, 1), 1);

  EXPECT_EQ(pkt, pad);
  rte_pktmbuf_free(pkt);
  EXPECT_EQ(rte_mbuf_refcnt_read(pad), 2);
  EXPECT_EQ(SessionPktsHeld(), 0u);
}

TEST_F(MtDevTxFlushTest, NotDeepPortPadKeepsARefOfItsOwn) {
  UT_KNOWN_BUG("the port pad has one ref, so two threads sending it can lose one");
  LazyPort(128, 100);

  EXPECT_GE(rte_mbuf_refcnt_read(ut_dev_pad(ctx_)), 2);
}

/* a sender's ref keeps the pad out of the pool whether the check took one or not */
TEST_F(MtDevTxFlushTest, NotDeepPadInPlaceOfAnInvalidPktKeepsItsRef) {
  UT_KNOWN_BUG("dev_tx_pkt_check() hands the PMD the port pad without a ref");
  LazyPort(128, 100);
  struct rte_mbuf* pad = ut_dev_pad(ctx_);
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(pool_);
  ASSERT_NE(pkt, nullptr);
  const uint16_t refs = rte_mbuf_refcnt_read(pad);
  rte_mbuf_refcnt_update(pad, 1);

  ASSERT_EQ(ut_dev_tx_pkt_check(ctx_, &pkt, 1), 1);

  EXPECT_EQ(pkt, pad);
  rte_pktmbuf_free(pkt);
  EXPECT_EQ(rte_mbuf_refcnt_read(pad), refs + 1);
  if (rte_mbuf_refcnt_read(pad) > refs) rte_mbuf_refcnt_update(pad, -1);
}

TEST_F(MtDevTxFlushTest, EagerPortWithoutACopyFlushesWithTheLazyLoop) {
  EagerPort(2048, 100);
  std::vector<struct rte_mbuf*> sys;
  for (struct rte_mbuf* m; (m = rte_pktmbuf_alloc(ut_dev_tx_sys_pool(ctx_)));)
    sys.push_back(m);
  Queue(100);

  EXPECT_EQ(Flush(), 0);

  EXPECT_EQ(FlushDescs(), 2u * 2048);
  EXPECT_EQ(SessionPktsHeld(), 0u);
  for (struct rte_mbuf* m : sys) rte_pktmbuf_free(m);
}

}  // namespace
