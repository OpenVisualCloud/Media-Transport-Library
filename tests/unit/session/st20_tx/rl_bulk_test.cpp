/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "session/st_video_transmitter_harness.h"

namespace {

constexpr int kBulk4 = 4; /* the bulk whose bursts set the pad positions */
constexpr uint64_t kHoldNs = 20000;

/* what one bulk-4 dequeue sends: its pkts up to a frame's start or end */
struct Run {
  int first;    /* the index in its frame of the run's first pkt */
  int k;        /* its pkts */
  bool due;     /* a pad follows its first burst */
  size_t start; /* the pkts of all runs before it */
};

/* The runs of the bulk-4 RL transmitter on the frames of c, fed as
 * ut_trs_run_frames() does. A run is due a pad when fmodf(first + 1 + pi / 2, pi) < k,
 * but a held frame's first run leaves by the inflight retry, which sends none. */
std::vector<Run> Bulk4Runs(const ut_trs_frames_case& c) {
  std::vector<Run> runs;
  int frame = 0, next = -(int)c.lead;
  size_t start = 0;
  while (frame < c.frames) {
    int pos = 0;
    while (pos < kBulk4 && next < c.total_pkts) {
      int first = next < 0 ? next + c.total_pkts : next;
      int k = 0;
      while (pos < kBulk4 && next < c.total_pkts && (!k || next)) {
        k++;
        next++;
        pos++;
      }
      bool held = c.hold_ns && !first;
      bool due = !held && fmodf((float)(first + 1) + c.pad_interval / 2, c.pad_interval) <
                              (float)k;
      runs.push_back({first, k, due, start});
      start += k;
    }
    if (next >= c.total_pkts) {
      frame++;
      next = 0;
    }
  }
  return runs;
}

/* bulk 4 with bursts that take at most c.accept_limit pkts: a run's pad follows its first
 * burst, the retries send none */
std::vector<uint32_t> Bulk4Wire(const ut_trs_frames_case& c) {
  std::vector<uint32_t> wire;
  for (const Run& r : Bulk4Runs(c)) {
    int sent = c.accept_limit ? std::min<int>(c.accept_limit, r.k) : r.k;
    for (int i = 0; i < r.k; i++) {
      if (i == sent && r.due) wire.push_back(UT_TRS_WIRE_PAD);
      wire.push_back((uint32_t)(r.first + i));
    }
    if (sent == r.k && r.due) wire.push_back(UT_TRS_WIRE_PAD);
  }
  return wire;
}

std::vector<uint32_t> Wire(const ut_trs_ctx* ctx) {
  std::vector<uint32_t> wire;
  for (uint32_t i = 0; i < ut_trs_wire_count(ctx); i++)
    wire.push_back(ut_trs_wire(ctx, i));
  return wire;
}

/* Bursts that come up short leave each pad of a due run after 0 to k of its pkts, as
 * bulk 4 does: the pkts all in run order, one pad per due run. */
void ExpectEachPadInsideItsRun(const ut_trs_frames_case& c,
                               const std::vector<uint32_t>& wire) {
  std::vector<Run> runs = Bulk4Runs(c);
  std::vector<uint32_t> pkts, sent;
  std::vector<const Run*> due;
  for (const Run& r : runs) {
    for (int i = 0; i < r.k; i++) pkts.push_back((uint32_t)(r.first + i));
    if (r.due) due.push_back(&r);
  }
  size_t pad = 0;
  for (uint32_t w : wire) {
    if (w != UT_TRS_WIRE_PAD) {
      sent.push_back(w);
      continue;
    }
    ASSERT_LT(pad, due.size()) << "extra pad after " << sent.size() << " pkts";
    EXPECT_GE(sent.size(), due[pad]->start) << "pad " << pad;
    EXPECT_LE(sent.size(), due[pad]->start + due[pad]->k) << "pad " << pad;
    pad++;
  }
  EXPECT_EQ(due.size(), pad);
  EXPECT_EQ(pkts, sent);
}

class St20TxRlBulkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }
  void TearDown() override {
    for (ut_trs_ctx* ctx : ctxs_) ut_trs_destroy(ctx);
  }
  /* the wire of c, empty if the run stopped short */
  std::vector<uint32_t> Run(const ut_trs_frames_case& c) {
    ut_trs_ctx* ctx = ut_trs_create();
    if (!ctx) return {};
    ctxs_.push_back(ctx);
    if (ut_trs_run_frames(ctx, &c) < 0) return {};
    return Wire(ctx);
  }
  ut_trs_ctx* Last() {
    return ctxs_.back();
  }
  std::vector<ut_trs_ctx*> ctxs_;
};

struct WireCase {
  const char* name;
  int total_pkts;
  float pad_interval;
  uint64_t hold_ns;
};

/* 1080p59.94 at a trained pad interval, and 70 pkts, whose last bulk carries dummies, at
 * small user intervals: pi 5 pads after 4 of every 5 slices, pi 6 after 2 of every 3 */
const WireCase kWireCases[] = {
    {"Hd164Held", 4320, 164.37f, kHoldNs}, {"Hd164Late", 4320, 164.37f, 0},
    {"Short5Held", 70, 5.0f, kHoldNs},     {"Short5Late", 70, 5.0f, 0},
    {"Short6Held", 70, 6.0f, kHoldNs},     {"Short6Late", 70, 6.0f, 0},
};

class St20TxRlBulkWireTest : public St20TxRlBulkTest,
                             public ::testing::WithParamInterface<WireCase> {
 protected:
  ut_trs_frames_case Case(unsigned int bulk) {
    const WireCase& w = GetParam();
    ut_trs_frames_case c = {};
    c.bulk = bulk;
    c.total_pkts = w.total_pkts;
    c.frames = 2;
    c.pad_interval = w.pad_interval;
    c.hold_ns = w.hold_ns;
    return c;
  }
};

/* pins the bulk-4 wire, the one longer bulks must keep */
TEST_P(St20TxRlBulkWireTest, Bulk4WireIsOnePadAfterEachDueSlice) {
  ut_trs_frames_case c = Case(4);
  EXPECT_EQ(Bulk4Wire(c), Run(c));
}

TEST_P(St20TxRlBulkWireTest, Bulk16WireMatchesBulk4) {
  std::vector<uint32_t> bulk4 = Run(Case(4));
  ASSERT_FALSE(bulk4.empty());
  EXPECT_EQ(bulk4, Run(Case(16)));
}

INSTANTIATE_TEST_SUITE_P(Frames, St20TxRlBulkWireTest, ::testing::ValuesIn(kWireCases),
                         [](const ::testing::TestParamInfo<WireCase>& info) {
                           return std::string(info.param.name);
                         });

TEST_F(St20TxRlBulkTest, SmallPadIntervalSendsTwoPadsInOneBulk) {
  ut_trs_frames_case c = {};
  c.bulk = 16;
  c.total_pkts = 70;
  c.frames = 1;
  c.pad_interval = 6.0f;
  std::vector<uint32_t> wire = Run(c);
  ASSERT_GE(wire.size(), 18u);

  /* pkts 0-15, one bulk, and its pads after pkts 7 and 11 */
  std::vector<uint32_t> first(wire.begin(), wire.begin() + 18);
  std::vector<uint32_t> expect = {
      0,  1,  2,  3, 4, 5, 6, 7, UT_TRS_WIRE_PAD, 8, 9, 10, 11, UT_TRS_WIRE_PAD,
      12, 13, 14, 15};
  EXPECT_EQ(expect, first);
}

/* 70 pkts: the last bulk of 16 holds pkts 64-69 and 10 dummies */
class St20TxRlBulkBoundaryTest : public St20TxRlBulkTest {
 protected:
  void SetUp() override {
    St20TxRlBulkTest::SetUp();
    c_.bulk = 16;
    c_.total_pkts = 70;
    c_.frames = 2;
    c_.pad_interval = 5.0f;
    c_.hold_ns = kHoldNs;
    wire_ = Run(c_);
    ASSERT_FALSE(wire_.empty());
  }
  ut_trs_frames_case c_ = {};
  std::vector<uint32_t> wire_;
};

TEST_F(St20TxRlBulkBoundaryTest, DummiesAreFreedNotSent) {
  for (uint32_t w : wire_)
    EXPECT_TRUE(w == UT_TRS_WIRE_PAD || w < (uint32_t)c_.total_pkts);
  EXPECT_EQ(2 * 10, ut_trs_stat_pkts_burst_dummy(Last()));
  EXPECT_EQ(0u, ut_trs_frame_pkts_in_use(Last()));
}

TEST_F(St20TxRlBulkBoundaryTest, EachFrameStartsAtItsTargetAfterThePreviousFrame) {
  std::vector<uint32_t> starts;
  for (uint32_t i = 0; i < wire_.size(); i++)
    if (!wire_[i]) starts.push_back(i);
  ASSERT_EQ(2u, starts.size());
  EXPECT_LT((uint32_t)(std::find(wire_.begin(), wire_.end(), 69u) - wire_.begin()),
            starts[1]);
  for (int f = 0; f < 2; f++) {
    SCOPED_TRACE(f);
    EXPECT_GE(ut_trs_wire_tsc(Last(), starts[f]), ut_trs_frame_target(Last(), f));
  }
}

TEST_F(St20TxRlBulkBoundaryTest, HeldFrameKeepsTheBulk4Wire) {
  EXPECT_EQ(Bulk4Wire(c_), wire_);
}

struct ShortCase {
  unsigned int bulk;
  uint16_t accept_limit;
  uint32_t fail_every;
  unsigned int lead;
  int total_pkts;
  float pad_interval;
};

std::string ShortName(const ::testing::TestParamInfo<ShortCase>& info) {
  const ShortCase& p = info.param;
  return "Bulk" + std::to_string(p.bulk) + "Accept" + std::to_string(p.accept_limit) +
         "FailEvery" + std::to_string(p.fail_every) + "Lead" + std::to_string(p.lead) +
         "Pkts" + std::to_string(p.total_pkts);
}

class St20TxRlBulkShortBurstTest : public St20TxRlBulkTest,
                                   public ::testing::WithParamInterface<ShortCase> {
 protected:
  ut_trs_frames_case Case() {
    const ShortCase& p = GetParam();
    ut_trs_frames_case c = {};
    c.bulk = p.bulk;
    c.total_pkts = p.total_pkts;
    c.frames = 2;
    c.pad_interval = p.pad_interval;
    c.hold_ns = kHoldNs;
    c.accept_limit = p.accept_limit;
    c.fail_every = p.fail_every;
    c.lead = p.lead;
    return c;
  }
};

TEST_P(St20TxRlBulkShortBurstTest, EveryPadLeavesInsideItsRun) {
  ut_trs_frames_case c = Case();
  std::vector<uint32_t> wire = Run(c);
  ASSERT_FALSE(wire.empty());
  ExpectEachPadInsideItsRun(c, wire);
}

/* lead > 0: RTP level, frame 0 starts lead pkts into a bulk, its bulks not 4-aligned */
INSTANTIATE_TEST_SUITE_P(
    Limits, St20TxRlBulkShortBurstTest,
    ::testing::Values(ShortCase{4, 1, 0, 0, 70, 5.0f}, ShortCase{4, 3, 0, 0, 70, 5.0f},
                      ShortCase{4, 3, 3, 0, 70, 5.0f}, ShortCase{16, 1, 0, 0, 70, 5.0f},
                      ShortCase{16, 3, 0, 0, 70, 5.0f}, ShortCase{16, 5, 0, 0, 70, 5.0f},
                      ShortCase{16, 13, 0, 0, 70, 5.0f}, ShortCase{16, 0, 3, 0, 70, 5.0f},
                      ShortCase{16, 5, 3, 0, 70, 5.0f}, ShortCase{16, 13, 4, 0, 70, 5.0f},
                      ShortCase{4, 0, 3, 1, 4320, 164.37f},
                      ShortCase{4, 0, 2, 1, 70, 5.0f}, ShortCase{4, 1, 3, 2, 70, 5.0f},
                      ShortCase{4, 2, 3, 3, 70, 5.0f}),
    ShortName);

class St20TxRlBulkOffsetTest : public St20TxRlBulkShortBurstTest {};

/* RTP level, frame 0 lead pkts into a bulk: a short burst's retry must send no pad */
TEST_P(St20TxRlBulkOffsetTest, Bulk4RetriesSendNoPad) {
  ut_trs_frames_case c = Case();
  EXPECT_EQ(Bulk4Wire(c), Run(c));
}

INSTANTIATE_TEST_SUITE_P(Leads, St20TxRlBulkOffsetTest,
                         ::testing::Values(ShortCase{4, 1, 0, 1, 70, 5.0f},
                                           ShortCase{4, 1, 0, 2, 70, 5.0f},
                                           ShortCase{4, 1, 0, 3, 70, 5.0f},
                                           ShortCase{4, 2, 0, 3, 70, 5.0f},
                                           ShortCase{4, 1, 0, 2, 4320, 164.37f},
                                           ShortCase{4, 1, 0, 3, 4320, 164.37f},
                                           ShortCase{4, 2, 0, 3, 4320, 164.37f}),
                         ShortName);

}  // namespace
