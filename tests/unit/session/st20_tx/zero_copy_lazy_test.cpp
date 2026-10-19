/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * tv_pkts_capable_chain() against the production builder and TSC transmitter on a port
 * whose PMD frees sent mbufs lazily: DPDK 26.07 iavf/ice at the default tx_free_thresh,
 * or a PMD that frees a slot's mbuf when it reuses it. Where the bound keeps zero copy,
 * the app never hands the builder a frame still referenced; where it copies, zero copy
 * would. The 512-descriptor ports are not deep and keep main's bound, total_pkts *
 * (frames - 1) >= nb_tx_desc, which zero-copies 480p BPM (686 pkts) on 2 buffers, and
 * its frames come back referenced.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxZeroCopyLazyTest*'
 */

#include <gtest/gtest.h>

#include "common/ut_known_bug.h"
#include "common/ut_tx_ring.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr uint64_t kRunNs = 100000000; /* 6 frames */
constexpr uint64_t kStepNs = 1000;

struct LazyCase {
  const char* name;
  uint32_t width;
  uint32_t height;
  uint16_t frames;
  uint16_t nb_tx_desc;
  bool known_bug;
};

class St20TxZeroCopyLazyTest
    : public ::testing::TestWithParam<std::tuple<LazyCase, ut_txr_mode>> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxZeroCopyLazyTest, ZeroCopyWhereTheBoundAllowsNeverMeetsAReferencedFrame) {
  const LazyCase& lc = std::get<0>(GetParam());
  if (lc.known_bug)
    UT_KNOWN_BUG("main's bound zero-copies frames the queue and builder ring still hold");
  ut_txv_pipe_case c = {};
  c.width = lc.width;
  c.height = lc.height;
  c.frames_cnt = lc.frames;
  c.num_port = 1;
  c.nb_tx_desc[MTL_SESSION_PORT_P] = lc.nb_tx_desc;
  c.ring_mode = std::get<1>(GetParam());
  c.free_thresh = 32;
  ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
  const int total_pkts = ut_txv_pipe_total_pkts(ctx_);

  ut_txv_pipe_run(ctx_, kRunNs, kStepNs);

  ut_txv_ctx* bound = ut_txv_create();
  ASSERT_NE(bound, nullptr);
  bool zero_copy =
      ut_txv_pkts_capable_chain(bound, lc.nb_tx_desc, total_pkts, lc.frames, false);
  ut_txv_destroy(bound);
  EXPECT_GE(ut_txv_pipe_frames_asked(ctx_), 2u);
  EXPECT_EQ(ut_txv_pipe_first_alloc_fail(ctx_), 0u);
  EXPECT_EQ(ut_txv_pipe_first_burst_fail(ctx_), 0u);
  EXPECT_EQ(ut_txv_pipe_first_frame_busy(ctx_) == 0, zero_copy)
      << total_pkts << " pkts, " << lc.frames << " buffers";
}

INSTANTIATE_TEST_SUITE_P(
    Formats, St20TxZeroCopyLazyTest,
    testing::Combine(testing::Values(LazyCase{"P480Fb2Desc512", 720, 480, 2, 512, true},
                                     LazyCase{"P480Fb3Desc512", 720, 480, 3, 512},
                                     LazyCase{"P576Fb2Desc512", 720, 576, 2, 512},
                                     LazyCase{"P720Fb2Desc512", 1280, 720, 2, 512},
                                     LazyCase{"P480Fb3Desc2048", 720, 480, 3, 2048},
                                     LazyCase{"P576Fb2Desc2048", 720, 576, 2, 2048},
                                     LazyCase{"P576Fb3Desc2048", 720, 576, 3, 2048},
                                     LazyCase{"P720Fb2Desc2048", 1280, 720, 2, 2048}),
                     testing::Values(UT_TXR_FREE_ON_CLEANUP, UT_TXR_FREE_ON_REUSE)),
    [](const testing::TestParamInfo<std::tuple<LazyCase, ut_txr_mode>>& info) {
      return std::string(std::get<0>(info.param).name) +
             (std::get<1>(info.param) == UT_TXR_FREE_ON_CLEANUP ? "Cleanup" : "Reuse");
    });

}  // namespace
