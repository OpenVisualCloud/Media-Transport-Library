/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

constexpr ut_txv_bulk_case Rl1() {
  ut_txv_bulk_case c = {};
  c.num_port = 1;
  c.way[0] = ST21_TX_PACING_WAY_RL;
  return c;
}

struct BulkCase {
  const char* name;
  ut_txv_bulk_case c;
  unsigned int bulk;
};

ut_txv_bulk_case With(ut_txv_bulk_case c, void (*set)(ut_txv_bulk_case*)) {
  set(&c);
  return c;
}

const BulkCase kBulkCases[] = {
    {"RlFrame", Rl1(), 16},
    {"RlRlFrame",
     With(Rl1(),
          [](ut_txv_bulk_case* c) {
            c->num_port = 2;
            c->way[1] = ST21_TX_PACING_WAY_RL;
          }),
     16},
    {"RlTscFrame",
     With(Rl1(),
          [](ut_txv_bulk_case* c) {
            c->num_port = 2;
            c->way[1] = ST21_TX_PACING_WAY_TSC;
          }),
     4},
    {"TscFrame",
     With(Rl1(), [](ut_txv_bulk_case* c) { c->way[0] = ST21_TX_PACING_WAY_TSC; }), 4},
    /* a slice-level bulk waits for the lines of all its pkts */
    {"RlSlice", With(Rl1(), [](ut_txv_bulk_case* c) { c->slice_level = true; }), 4},
    {"RlRtp", With(Rl1(), [](ut_txv_bulk_case* c) { c->rtp_level = true; }), 4},
    {"RlSt22", With(Rl1(), [](ut_txv_bulk_case* c) { c->st22 = true; }), 4},
    {"RlDisableBulk", With(Rl1(), [](ut_txv_bulk_case* c) { c->disable_bulk = true; }),
     1},
    /* 31 pkts: a 16-entry ring holds 15 */
    {"RlFrame31Pkts", With(Rl1(), [](ut_txv_bulk_case* c) { c->height = 8; }), 4},
    /* 35 pkts: a 32-entry ring holds 31 */
    {"RlFrame35Pkts", With(Rl1(), [](ut_txv_bulk_case* c) { c->height = 9; }), 16},
};

class St20TxBulkAttachTest : public ::testing::TestWithParam<BulkCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxBulkAttachTest, AttachPicksTheBulkOfItsPacingAndType) {
  EXPECT_EQ(GetParam().bulk, ut_txv_attach_bulk(ctx_, &GetParam().c));
}

INSTANTIATE_TEST_SUITE_P(Sessions, St20TxBulkAttachTest, ::testing::ValuesIn(kBulkCases),
                         [](const ::testing::TestParamInfo<BulkCase>& info) {
                           return std::string(info.param.name);
                         });

class St20TxBulkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* the builder ring, two transmitter bulks of 16, the 2048 deep descriptors at 2 a pkt and
 * another lcore's cache */
TEST_F(St20TxBulkTest, DeepRlPoolsHoldTwoRlBulks) {
  constexpr unsigned int kRing = 512, kDeepDesc = 2048, kCacheSlack = 192;
  ut_txv_bulk_case c = Rl1();
  c.deep = true;

  ASSERT_EQ(16u, ut_txv_attach_bulk(ctx_, &c));
  EXPECT_EQ(kRing + 2 * 16 + kDeepDesc / 2 + kCacheSlack, ut_txv_first_pool_n(ctx_));
}

TEST_F(St20TxBulkTest, TrainedRlKeepsTheRlBulk) {
  ASSERT_EQ(0, ut_txv_init_rl_pacing_at_bulk(ctx_, 16, false));

  EXPECT_EQ(ST21_TX_PACING_WAY_RL, ut_txv_pacing_way(ctx_, 0));
  EXPECT_EQ(16u, ut_txv_bulk(ctx_));
}

/* TSC spends bulk - 1 of the narrow vrx 8: 3 at the TSC bulk */
TEST_F(St20TxBulkTest, RlFallingBackToTscTakesTheTscBulk) {
  ASSERT_EQ(0, ut_txv_init_rl_pacing_at_bulk(ctx_, 16, true));

  EXPECT_EQ(ST21_TX_PACING_WAY_TSC, ut_txv_pacing_way(ctx_, 0));
  EXPECT_EQ(4u, ut_txv_bulk(ctx_));
  EXPECT_EQ(5u, ut_txv_pacing_vrx(ctx_));
}

}  // namespace
