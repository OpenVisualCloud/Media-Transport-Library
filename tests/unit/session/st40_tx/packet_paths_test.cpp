/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the ST40 TX packet paths of st_tx_ancillary_session.c. A case that pins a known
 * defect names it; the fix of the defect flips the case.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <rte_cksum.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "session/st40_tx_harness.h"
#include "session/st40_tx_test_harness.h"
#include "st2110/st_err.h"
#include "st40_api.h"

namespace {
constexpr size_t kUdpHdr = 42;
constexpr uint64_t kFrameNs = 1000 * 1000; /* ut_txa_create() */
constexpr unsigned int kRtpLevel = 0;      /* prepare() packets: the RTP tasklet */

uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

/* An app RFC 8331 RTP header, first chunk in host order as the lib expects it. */
std::vector<uint8_t> app_rtp(uint8_t f) {
  struct st40_rfc8331_rtp_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.base.version = 2;
  hdr.base.tmstamp = htonl(1234);
  hdr.first_hdr_chunk.anc_count = 1;
  hdr.first_hdr_chunk.f = f;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&hdr);
  return std::vector<uint8_t>(b, b + sizeof(hdr));
}

/* One ANC as ut_txa_set_anc_meta() describes it, its UDW read from `data`. */
std::vector<uint8_t> encoded_anc(uint16_t udw, const uint8_t* data) {
  struct st40_meta meta;
  memset(&meta, 0, sizeof(meta));
  meta.did = 0x41;
  meta.sdid = 0x05;
  meta.udw_size = udw;
  std::vector<uint8_t> anc(st40_rfc8331_payload_bytes(udw));
  uint32_t written = 0;
  EXPECT_EQ(st40_rfc8331_encode_packet(anc.data(), anc.size(), &meta, data, &written), 0);
  return anc;
}
}  // namespace

class St40TxPathTest : public ::testing::Test {
 protected:
  struct Pkt {
    ut_txa_pkt_meta meta;
    std::vector<uint8_t> b;
    const uint8_t* rtp() const {
      return b.data() + kUdpHdr;
    }
  };

  ut_txa_ctx* ctx_ = nullptr;
  std::vector<uint8_t> data_ = std::vector<uint8_t>(256);
  uint64_t now_ = 0;

  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
    for (size_t i = 0; i < data_.size(); i++) data_[i] = static_cast<uint8_t>(i);
    ut_txa_set_frame_payload(ctx_, data_.data(), static_cast<uint32_t>(data_.size()));
  }

  void TearDown() override {
    ut_txa_destroy(ctx_);
  }

  /* A new session, an epoch ahead as St40TxFmdTest: the first tick syncs pacing and the
   * next one builds. `packets` 2 is split mode with two ANCs. */
  void prepare(bool chain, bool redundant, unsigned int packets = 1, bool fmd = false) {
    ut_txa_destroy(ctx_);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txa_set_cur_epochs(ctx_, 10);
    ut_txa_set_mock_ptp_time(ctx_, 10 * kFrameNs);
    now_ = 0;
    ut_txa_set_mock_tsc_time(ctx_, now_);
    ASSERT_EQ(packets
                  ? ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, packets)
                  : ut_txa_prepare_rtp_tasklet(ctx_),
              0);
    if (fmd) ut_txa_set_fmd(ctx_, 0x2a5a5a, 1);
    ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);
    ASSERT_EQ(redundant ? ut_txa_enable_redundant(ctx_) : 0, 0);
    ASSERT_EQ(chain ? ut_txa_enable_chain(ctx_) : 0, 0);
    ut_txa_set_frame_payload(ctx_, data_.data(), static_cast<uint32_t>(data_.size()));
    ut_txa_set_anc_meta(ctx_, 8, fmd ? 0 : std::max(packets, 1u));
  }

  void put_app(uint8_t f = 0b00) {
    auto app = app_rtp(f);
    ASSERT_EQ(ut_txa_put_app_rtp(ctx_, app.data(), static_cast<uint16_t>(app.size())), 0);
  }

  /* One tasklet run; the mock TSC then moves on to the next target. */
  int tick() {
    int ret = ut_txa_step_frame_tasklet(ctx_);
    now_ = std::max(now_, ut_txa_tsc_time_cursor(ctx_));
    ut_txa_set_mock_tsc_time(ctx_, now_);
    return ret;
  }

  /* Ticks until the frame in flight is given back; the result of each tick. */
  std::vector<int> send_frame() {
    std::vector<int> rets;
    int done = ut_txa_notify_frame_done_calls(ctx_);
    while (ut_txa_notify_frame_done_calls(ctx_) == done && rets.size() < 8)
      rets.push_back(tick());
    return rets;
  }

  std::vector<Pkt> pop(enum mtl_session_port port) {
    std::vector<Pkt> pkts;
    Pkt p;
    p.b.resize(2048);
    for (int len;
         (len = ut_txa_pop_port_packet(ctx_, port, &p.meta, p.b.data(), 2048)) >= 0;) {
      pkts.push_back(p);
      pkts.back().b.resize(len);
    }
    return pkts;
  }

  /* One builder call on a fresh mbuf: its result, and the RTP bytes when it succeeds. */
  int build(bool chain, std::vector<uint8_t>* rtp) {
    struct rte_mbuf* m = ut_txa_alloc_mbuf(2048);
    if (!m) return -ENOMEM;
    int ret = chain ? ut_txa_build_rtp_packet(ctx_, m) : ut_txa_build_packet(ctx_, m);
    const uint8_t* d = ut_txa_pkt_data(m);
    if (ret >= 0) rtp->assign(d + (chain ? 0 : kUdpHdr), d + ut_txa_pkt_data_len(m));
    ut_txa_free_mbuf(m);
    return ret;
  }

  /* The template of `port` sized to the packet; with offload the checksum stays 0. */
  void expect_udp_hdr(const Pkt& p, enum mtl_session_port port, bool offload) {
    std::vector<uint8_t> got(p.b.begin(), p.b.begin() + kUdpHdr);
    std::vector<uint8_t> want(ut_txa_hdr(ctx_, port), ut_txa_hdr(ctx_, port) + kUdpHdr);
    EXPECT_EQ(be16(&got[16]), p.b.size() - 14) << "ipv4 total_length";
    EXPECT_EQ(be16(&got[38]), p.b.size() - 34) << "udp dgram_len";
    EXPECT_TRUE(offload ? !be16(&got[24]) : rte_raw_cksum(&got[14], 20) == 0xffff);
    for (size_t off : {16, 24, 38})
      got[off] = got[off + 1] = want[off] = want[off + 1] = 0;
    EXPECT_EQ(got, want);
    EXPECT_TRUE(p.meta.ipv4_init);
  }

  /* One packet on each port: R is the P packet behind the R header. */
  void expect_twin(bool offload_p, Pkt* p_out) {
    auto p = pop(MTL_SESSION_PORT_P), r = pop(MTL_SESSION_PORT_R);
    ASSERT_EQ(p.size(), 1u);
    ASSERT_EQ(r.size(), 1u);
    expect_udp_hdr(p[0], MTL_SESSION_PORT_P, offload_p);
    expect_udp_hdr(r[0], MTL_SESSION_PORT_R, false);
    EXPECT_TRUE(std::equal(p[0].b.begin() + kUdpHdr, p[0].b.end(),
                           r[0].b.begin() + kUdpHdr, r[0].b.end()));
    EXPECT_EQ(r[0].meta.tsc, p[0].meta.tsc);
    EXPECT_EQ(r[0].meta.idx, p[0].meta.idx);
    EXPECT_EQ(r[0].meta.nb_segs, p[0].meta.nb_segs);
    *p_out = p[0];
  }
};

/* No split: ANCs go in while they fit in the attach max_pkt_len, and the rest of the
 * frame is not sent (planner defect c). Both builders write the same RTP in each field
 * state. */
TEST_F(St40TxPathTest, Rfc8331PacksAncsWhileTheyFit) {
  const struct {
    uint16_t udw, ancs, sent;
  } cases[] = {{8, 3, 3}, {50, 20, 19}};

  for (const auto& c : cases) {
    size_t anc = st40_rfc8331_payload_bytes(c.udw);
    ut_txa_set_anc_meta(ctx_, c.udw, c.ancs);
    for (int f : {0b00, 0b10, 0b11}) {
      SCOPED_TRACE(::testing::Message() << c.ancs << " ANCs, F " << f);
      std::vector<uint8_t> no_chain, chain;
      ut_txa_set_interlaced(ctx_, f != 0b00, f == 0b11);
      ut_txa_set_seq(ctx_, 7);
      ASSERT_EQ(build(false, &no_chain), c.sent);
      ut_txa_set_seq(ctx_, 7);
      ASSERT_EQ(build(true, &chain), c.sent);
      EXPECT_EQ(chain, no_chain);
      ASSERT_EQ(no_chain.size(), 20 + c.sent * anc);
      EXPECT_TRUE(no_chain[1] & 0x80) << "marker";
      EXPECT_EQ(be16(&no_chain[2]), 7);
      EXPECT_EQ(be16(&no_chain[14]), c.sent * anc) << "length";
      EXPECT_EQ(no_chain[16], c.sent) << "anc_count";
      EXPECT_EQ(no_chain[17] >> 6, f);
      for (uint32_t i = 0; i < c.sent; i++) {
        auto at = no_chain.begin() + 20 + i * anc;
        EXPECT_EQ(std::vector<uint8_t>(at, at + anc), encoded_anc(c.udw, &data_[i])) << i;
      }
    }
  }
}

/* The R twin is the P packet behind the R header, with the P tsc, idx and stats; chain
 * and no chain put the same bytes on the wire. The checksum follows the offload of each
 * port. */
TEST_F(St40TxPathTest, RedundantTwinMatchesPrimary) {
  for (bool fmd : {false, true}) {
    for (bool offload : {false, true}) {
      Pkt wire[2];
      for (bool chain : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "fmd " << fmd << " offload " << offload << " chain " << chain);
        prepare(chain, true, 1, fmd);
        ut_txa_set_cksum_offload(ctx_, MTL_SESSION_PORT_P, offload);
        send_frame();
        ASSERT_NO_FATAL_FAILURE(expect_twin(offload, &wire[chain]));
        EXPECT_EQ(wire[chain].meta.nb_segs, chain ? 2 : 1);
        auto sp = ut_txa_port_stats(ctx_, MTL_SESSION_PORT_P);
        auto sr = ut_txa_port_stats(ctx_, MTL_SESSION_PORT_R);
        EXPECT_EQ(memcmp(&sp, &sr, sizeof(sp)), 0) << "P and R stats";
        EXPECT_EQ(sp.bytes, wire[chain].b.size()) << "bytes";
        EXPECT_EQ(sr.frames, 1u);
        for (auto pool : {UT_TXA_POOL_P, UT_TXA_POOL_R, UT_TXA_POOL_CHAIN})
          EXPECT_EQ(ut_txa_pool_count(ctx_, pool),
                    pool != UT_TXA_POOL_CHAIN || chain ? 32u : 0u);
      }
      EXPECT_EQ(wire[0].b, wire[1].b);
      EXPECT_EQ(wire[0].meta.tsc, wire[1].meta.tsc);
    }
  }
}

/* Interlaced: the F bits and the field counters alternate per frame. */
TEST_F(St40TxPathTest, InterlacedFramesAlternateFields) {
  prepare(false, false);
  ut_txa_set_interlaced(ctx_, true, false);
  ut_txa_set_frame_count(ctx_, 2);

  for (int f : {0b10, 0b11}) {
    send_frame();
    auto p = pop(MTL_SESSION_PORT_P);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].rtp()[17] >> 6, f);
    EXPECT_EQ(ut_txa_stat_interlace_first(ctx_), 1u);
    EXPECT_EQ(ut_txa_stat_interlace_second(ctx_), f == 0b11 ? 1u : 0u);
  }
}

/* A frame planned above one packet keeps its claim and sends nothing (defect d). */
TEST_F(St40TxPathTest, PlanAboveOnePacketReportsErrorAfterClaim) {
  prepare(false, false);
  ut_txa_set_anc_meta(ctx_, 255, 4);

  EXPECT_EQ(tick(), 0);

  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_FRAME_APP_ERR_TX_FRAME);
  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), 1);
  EXPECT_FALSE(ut_txa_session_waiting_frame(ctx_));
  EXPECT_EQ(ut_txa_frame_refcnt(ctx_), 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
}

/* A frame that is still in flight is not claimed: no ref, no packet, the next tick asks
 * the app again. */
TEST_F(St40TxPathTest, FrameInFlightIsNotClaimed) {
  prepare(false, false);
  ut_txa_set_frame_refcnt(ctx_, 1);

  EXPECT_EQ(tick(), 0);

  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_FRAME_APP_ERR_TX_FRAME);
  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), 1);
  EXPECT_TRUE(ut_txa_session_waiting_frame(ctx_));
  EXPECT_EQ(ut_txa_frame_refcnt(ctx_), 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
}

/* Inside the scheduler window the TSC gate reports pending work, outside it all done. */
TEST_F(St40TxPathTest, GateReturnsPendingInsideScheduleWindow) {
  prepare(false, false);
  ut_txa_set_sch_schedule_ns(ctx_, 1000);
  ut_txa_step_frame_tasklet(ctx_);

  for (uint64_t ahead : {500, 1000, 1500}) {
    ut_txa_set_mock_tsc_time(ctx_, ut_txa_tsc_time_cursor(ctx_) - ahead);
    EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), ahead < 1000 ? 1 : 0) << ahead;
    EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_TSCTRS_TARGET_TSC_NOT_REACH);
  }
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
}

/* A target over 1 s ahead is an invalid tsc: the gate does not wait, the packet goes. */
TEST_F(St40TxPathTest, GateSendsWhenTargetIsOverOneSecondAhead) {
  prepare(false, false);
  ut_txa_set_mock_tsc_time(ctx_, 5000 * kFrameNs);
  ut_txa_step_frame_tasklet(ctx_);

  ut_txa_set_mock_tsc_time(ctx_, ut_txa_tsc_time_cursor(ctx_) - 1000 * kFrameNs - 1);
  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u);
}

/* No chain RTP level: only the first packet of an interlaced timestamp has its first
 * chunk swapped; the next one and a progressive one go as the app wrote them. */
TEST_F(St40TxPathTest, Rfc8331NoChainSwapsOnlyFirstPacketOfTimestamp) {
  auto app = app_rtp(0b10);
  for (bool interlaced : {true, false}) {
    prepare(false, false, kRtpLevel);
    ut_txa_set_rtp_level(ctx_, interlaced);
    for (int i = 0; i < 2; i++) {
      put_app(0b10);
      tick();
    }

    auto p = pop(MTL_SESSION_PORT_P);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(memcmp(p[0].rtp() + 16, &app[16], 4) != 0, interlaced);
    EXPECT_EQ(memcmp(p[1].rtp() + 16, &app[16], 4), 0);
  }
}

class St40TxChainTest : public St40TxPathTest,
                        public ::testing::WithParamInterface<bool> {};

/* Split: one packet per ANC, half a frame apart, one RTP timestamp, one frame done. */
TEST_P(St40TxChainTest, SplitFrameSendsOnePacketPerAnc) {
  prepare(GetParam(), false, 2);
  ut_txa_set_seq(ctx_, 7);

  EXPECT_EQ(send_frame(), (std::vector<int>{0, 1, 1}));

  auto p = pop(MTL_SESSION_PORT_P);
  ASSERT_EQ(p.size(), 2u);
  for (uint32_t i = 0; i < 2; i++) {
    EXPECT_EQ(be16(p[i].rtp() + 2), 7 + i);
    EXPECT_EQ((p[i].rtp()[1] & 0x80) != 0, i == 1);
    EXPECT_EQ(p[i].rtp()[16], 1);
    EXPECT_EQ(p[i].meta.idx, i);
    EXPECT_EQ(std::vector<uint8_t>(p[i].b.begin() + kUdpHdr + 20, p[i].b.end()),
              encoded_anc(8, &data_[i]));
  }
  EXPECT_EQ(memcmp(p[1].rtp() + 4, p[0].rtp() + 4, 4), 0) << "tmstamp";
  EXPECT_EQ(p[1].meta.tsc - p[0].meta.tsc, kFrameNs / 2);
  EXPECT_EQ(ut_txa_port_stats(ctx_, MTL_SESSION_PORT_P).frames, 1u);
  EXPECT_EQ(ut_txa_stat_interlace_first(ctx_), 0u);
}

/* A build error gives the frame back: no packet, one seq gone, every mbuf free. In split
 * mode no room maps to ANC_TOO_LARGE. */
TEST_P(St40TxChainTest, BuildErrorAbortsFrame) {
  for (bool split : {false, true}) {
    prepare(GetParam(), false, split ? 2 : 1);
    ut_txa_set_anc_meta(ctx_, split ? 8 : 256, 1);
    if (split) ut_txa_set_max_pkt_len(ctx_, 16);

    EXPECT_EQ(send_frame().size(), 2u);

    EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_),
              split ? -STI_FRAME_ANC_TOO_LARGE : -EINVAL);
    EXPECT_TRUE(ut_txa_session_waiting_frame(ctx_));
    EXPECT_EQ(ut_txa_port_stats(ctx_, MTL_SESSION_PORT_P).frames, 0u);
    EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
    EXPECT_EQ(ut_txa_seq(ctx_), 1);
    EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_P), 32u);
    EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_CHAIN), GetParam() ? 32u : 0u);
  }
}

/* No R mbuf: P and its chained payload go back to their pools once, nothing is sent, and
 * the seq of the lost packet stays used (defect b). */
TEST_P(St40TxChainTest, RAllocFailureFreesPayloadOnce) {
  prepare(GetParam(), true);
  ut_txa_hold_pool(ctx_, UT_TXA_POOL_R);
  ASSERT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_R), 0u);

  EXPECT_EQ(tick(), 0);
  EXPECT_EQ(tick(), 0);

  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_FRAME_PKT_ALLOC_FAIL);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
  EXPECT_EQ(ut_txa_seq(ctx_), 1);
  EXPECT_FALSE(ut_txa_session_waiting_frame(ctx_));
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 0);
  EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_P), 32u);
  EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_CHAIN), GetParam() ? 32u : 0u);
}

/* RTP level redundant: R carries the app RTP; chain counts no R frame (defect a). */
TEST_P(St40TxChainTest, RtpRedundantFrameCounts) {
  for (bool offload : {false, true}) {
    Pkt p;
    prepare(GetParam(), true, kRtpLevel);
    ut_txa_set_cksum_offload(ctx_, MTL_SESSION_PORT_P, offload);
    put_app();

    EXPECT_EQ(tick(), 0);

    ASSERT_NO_FATAL_FAILURE(expect_twin(offload, &p));
    for (auto port : {MTL_SESSION_PORT_P, MTL_SESSION_PORT_R})
      EXPECT_EQ(ut_txa_port_stats(ctx_, port).bytes, p.b.size()) << port;
    EXPECT_EQ(ut_txa_port_stats(ctx_, MTL_SESSION_PORT_P).frames, 1u);
    EXPECT_EQ(ut_txa_port_stats(ctx_, MTL_SESSION_PORT_R).frames, GetParam() ? 0u : 1u);
  }
}

/* RTP level, no R mbuf: nothing is sent and every mbuf is free once; without chain the P
 * stats already count the packet. */
TEST_P(St40TxChainTest, RtpRAllocFailureSendsNothing) {
  prepare(GetParam(), true, kRtpLevel);
  ut_txa_hold_pool(ctx_, UT_TXA_POOL_R);
  put_app();

  EXPECT_EQ(tick(), 0);

  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_RTP_PKT_ALLOC_FAIL);
  EXPECT_EQ(pop(MTL_SESSION_PORT_P).size(), 0u);
  EXPECT_EQ(pop(MTL_SESSION_PORT_R).size(), 0u);
  EXPECT_EQ(ut_txa_port_stats(ctx_, MTL_SESSION_PORT_P).packets, GetParam() ? 0u : 1u);
  EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_P), 32u);
  EXPECT_EQ(ut_txa_pool_count(ctx_, UT_TXA_POOL_CHAIN), GetParam() ? 32u : 0u);
}

INSTANTIATE_TEST_SUITE_P(Paths, St40TxChainTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "Chain" : "NoChain";
                         });

/* The frame tasklet, or the RTP tasklet with one app packet queued. */
class St40TxSendPathTest : public St40TxPathTest,
                           public ::testing::WithParamInterface<bool> {};

/* A full P ring ends the tick before a frame or an app packet is taken. */
TEST_P(St40TxSendPathTest, RingFullSkipsTick) {
  prepare(false, false, GetParam() ? kRtpLevel : 1);
  if (GetParam()) put_app();
  ASSERT_EQ(ut_txa_fill_ring(ctx_, MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(tick(), 0);

  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_),
            GetParam() ? -STI_RTP_RING_FULL : -STI_FRAME_RING_FULL);
  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), 0);
  if (!GetParam()) return;
  ASSERT_EQ(pop(MTL_SESSION_PORT_P).size(), 31u);
  tick();
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u) << "the app packet waited in its ring";
}

/* A full R ring parks R, and the next tick retries it before any new work. The RTP
 * tasklet reports the retry with the P code (defect e). */
TEST_P(St40TxSendPathTest, ParkedRGoesFirst) {
  bool rtp = GetParam();
  prepare(false, true, rtp ? kRtpLevel : 1);
  if (rtp) put_app();
  ASSERT_EQ(ut_txa_fill_ring(ctx_, MTL_SESSION_PORT_R), 0);

  EXPECT_EQ(rtp ? tick() : send_frame().back(), rtp ? 1 : 0);
  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_),
            rtp ? -STI_RTP_PKT_R_ENQUEUE_FAIL : -STI_FRAME_PKT_R_ENQUEUE_FAIL);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u);

  if (rtp) put_app();
  EXPECT_EQ(tick(), 0);
  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_),
            rtp ? -STI_RTP_INFLIGHT_ENQUEUE_FAIL : -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL);
  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), rtp ? 0 : 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u) << "no new P";
  if (rtp) return;

  /* room in ring R: the parked R goes, then the tick goes on to claim a frame */
  ASSERT_EQ(pop(MTL_SESSION_PORT_R).size(), 31u);
  tick();
  EXPECT_EQ(pop(MTL_SESSION_PORT_R).size(), 1u);
  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), -STI_FRAME_APP_GET_FRAME_BUSY);
  tick();
  EXPECT_EQ(pop(MTL_SESSION_PORT_R).size(), 0u) << "the sent R stays sent";
}

INSTANTIATE_TEST_SUITE_P(Paths, St40TxSendPathTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "Rtp" : "Frame";
                         });

#ifdef MTL_SIMULATE_PACKET_DROPS
class St40TxTestHookTest : public St40TxPathTest {};

/* Redundant seq-gap drops follow the schedule P5, R4, P4 across sent frames; the abort of
 * the third frame starts the schedule again at P5. */
TEST_F(St40TxTestHookTest, SeqGapPlanKeptAcrossFramesResetByAbort) {
  const size_t sent[][2] = {{0, 5}, {5, 1}, {0, 0}, {0, 5}};
  for (bool chain : {false, true}) {
    SCOPED_TRACE(chain ? "chain" : "no chain");
    prepare(chain, true, 2);
    ut_txa_set_test_pattern(ctx_, ST40_TX_TEST_SEQ_GAP, 8);
    ut_txa_set_frame_count(ctx_, 4);

    for (const auto& s : sent) {
      ut_txa_set_anc_meta(ctx_, s[0] + s[1] ? 8 : 256, 5);
      send_frame();
      EXPECT_EQ(pop(MTL_SESSION_PORT_P).size(), s[0]);
      EXPECT_EQ(pop(MTL_SESSION_PORT_R).size(), s[1]);
    }
  }
}

/* The single port seq-gap steps the seq by 2 once per frame; across 0xffff the ext seq
 * carries. */
TEST_F(St40TxTestHookTest, SeqStepTwoAcrossWrapCarriesExt) {
  prepare(false, false);
  ut_txa_set_test_pattern(ctx_, ST40_TX_TEST_SEQ_GAP, 2);
  ut_txa_set_frame_count(ctx_, 2);
  ut_txa_set_seq(ctx_, 0xffff);

  send_frame();
  send_frame();

  auto p = pop(MTL_SESSION_PORT_P);
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(be16(p[0].rtp() + 2), 0xffff);
  EXPECT_EQ(be16(p[0].rtp() + 12), 0);
  EXPECT_EQ(be16(p[1].rtp() + 2), 0x0001);
  EXPECT_EQ(be16(p[1].rtp() + 12), 1);
}

/* NO_MARKER clears the marker of the last packet; BAD_PARITY corrupts each ANC as
 * tx_ancillary_corrupt_parity() does. */
TEST_F(St40TxTestHookTest, NoMarkerAndBadParityPerAnc) {
  for (int pattern : {ST40_TX_TEST_NO_MARKER, ST40_TX_TEST_BAD_PARITY}) {
    bool parity = pattern == ST40_TX_TEST_BAD_PARITY;
    prepare(false, false, 2);
    ut_txa_set_test_pattern(ctx_, pattern, 1);
    send_frame();

    auto p = pop(MTL_SESSION_PORT_P);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ((p[1].rtp()[1] & 0x80) != 0, parity) << "marker";
    for (uint32_t i = 0; i < 2; i++) {
      auto bad = encoded_anc(8, &data_[i]);
      ut40_tx_corrupt_parity(bad.data(), 8);
      EXPECT_EQ(memcmp(p[i].rtp() + 20, bad.data(), bad.size()) == 0, parity) << pattern;
    }
  }
}
#else
TEST(St40TxTestHookTest, NeedsNonReleaseBuild) {
  GTEST_SKIP() << "TX_ANC_TEST_* hooks build only without buildtype=release";
}
#endif
