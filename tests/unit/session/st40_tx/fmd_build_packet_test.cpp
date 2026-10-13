/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40 TX in ST40_TX_FLAG_FAST_METADATA mode against SMPTE ST 2110-41: packet
 * layout, RTP header, frame timestamps, the §7 network compatibility model, mbuf
 * capacity checks, oversized data item abort and ops validation. The RFC 8331 path
 * is pinned alongside as a regression guard.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxFmd*'
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "session/st40_tx_harness.h"
#include "st40_api.h"

namespace {
constexpr uint32_t kDit = 0x2a5a5a;
constexpr uint8_t kKBit = 1;
constexpr size_t kRtpBaseLen = 12;
constexpr size_t kFmdRtpLen = 16;
constexpr size_t kRfc8331RtpLen = 20;
constexpr uint8_t kFmdDefaultPt = 115;
constexpr uint8_t kAncDefaultPt = 113;
constexpr uint32_t kMaxDataItem = 1436;  /* ST_PKT_MAX_ETHER_BYTES - 58-byte FMD header */
constexpr uint32_t kUdpSizeLimit = 1460; /* ST 2110-10 Standard UDP Size Limit */
constexpr size_t kIpv4LenOffset = 14 + 2;
constexpr size_t kUdpLenOffset = 14 + 20 + 4;

uint32_t padded(uint32_t n) {
  return (n + 3) / 4 * 4;
}

uint32_t fmd_word(uint32_t dit, uint8_t k, uint32_t words) {
  return (dit << 10) | (static_cast<uint32_t>(k) << 9) | words;
}

uint32_t be32_at(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return ntohl(v);
}

uint16_t be16_at(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return ntohs(v);
}

std::vector<uint8_t> pattern(uint32_t n) {
  std::vector<uint8_t> v(n);
  for (uint32_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(0xa0 + i);
  return v;
}

/* §7 C_MAX = MAX(4, INT(R_NOMINAL / 43200)) */
int ncm_c_max(double r_nominal) {
  return std::max(4, static_cast<int>(r_nominal / 43200));
}

/* §7: largest C_INST with one packet draining every 1 / MAX(800, 1.1 R_NOMINAL) s */
int ncm_max_c_inst(const std::vector<uint64_t>& arrival_ns, double r_nominal) {
  const double t_drain_ns = 1e9 / std::max(800.0, r_nominal * 1.1);
  std::deque<double> drain_ns;
  int c_inst = 0;
  for (uint64_t a : arrival_ns) {
    while (!drain_ns.empty() && drain_ns.front() <= static_cast<double>(a))
      drain_ns.pop_front();
    drain_ns.push_back((drain_ns.empty() ? static_cast<double>(a) : drain_ns.back()) +
                       t_drain_ns);
    c_inst = std::max(c_inst, static_cast<int>(drain_ns.size()));
  }
  return c_inst;
}
}  // namespace

class St40TxFmdTest : public ::testing::Test {
 protected:
  ut_txa_ctx* ctx_ = nullptr;
  std::vector<struct rte_mbuf*> mbufs_;

  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    for (auto* m : mbufs_) ut_txa_free_mbuf(m);
    ut_txa_destroy(ctx_);
  }

  /* junk-filled: a byte the builder skips must not read back as pre-zeroed memory */
  struct rte_mbuf* mbuf(size_t room) {
    struct rte_mbuf* m = ut_txa_alloc_mbuf(room);
    if (!m) return nullptr;
    mbufs_.push_back(m);
    memset(ut_txa_pkt_data(m), 0xa5, room);
    return m;
  }

  /* One FMD frame through the frame tasklet, scheduled an epoch ahead so the first step
   * only syncs pacing and the second builds. */
  void prepare_fmd_frame(uint8_t* data, uint32_t size, unsigned int packets = 1) {
    ut_txa_set_cur_epochs(ctx_, 10);
    ut_txa_set_mock_ptp_time(ctx_, 10ull * 1000 * 1000);
    ut_txa_set_mock_tsc_time(ctx_, 0);
    ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, packets), 0);
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ut_txa_set_frame_payload(ctx_, data, size);
  }

  void step_frame() {
    ut_txa_step_frame_tasklet(ctx_);
    ut_txa_set_mock_tsc_time(ctx_, ut_txa_tsc_time_cursor(ctx_));
    ut_txa_step_frame_tasklet(ctx_);
  }

  struct Packet {
    const uint8_t* l2 = nullptr;
    uint32_t pkt_len = 0;
    const uint8_t* rtp = nullptr;
    uint32_t rtp_len = 0;
  };

  /* A packet from either builder; the chain builder's RTP mbuf goes behind a header. */
  bool build_packet(bool no_chain, Packet* p) {
    struct rte_mbuf* hdr = mbuf(2048);
    if (!hdr) return false;
    if (no_chain) {
      if (ut_txa_build_packet(ctx_, hdr) < 0) return false;
      p->rtp = ut_txa_pkt_data(hdr) + ut_txa_udp_hdr_len();
      p->rtp_len = ut_txa_pkt_data_len(hdr) - ut_txa_udp_hdr_len();
    } else {
      struct rte_mbuf* rtp = ut_txa_alloc_mbuf(2048);
      if (!rtp) return false;
      memset(ut_txa_pkt_data(rtp), 0xa5, 2048);
      if (ut_txa_build_rtp_packet(ctx_, rtp) < 0) {
        ut_txa_free_mbuf(rtp);
        return false;
      }
      ut_txa_build_packet_chain(ctx_, hdr, rtp);
      p->rtp = ut_txa_pkt_data(rtp);
      p->rtp_len = ut_txa_pkt_data_len(rtp);
    }
    p->l2 = ut_txa_pkt_data(hdr);
    p->pkt_len = ut_txa_pkt_pkt_len(hdr);
    return true;
  }

  const uint8_t* build(bool no_chain, uint32_t* rtp_len) {
    Packet p;
    if (!build_packet(no_chain, &p)) return nullptr;
    *rtp_len = p.rtp_len;
    return p.rtp;
  }
};

class St40TxFmdBuildTest : public St40TxFmdTest,
                           public ::testing::WithParamInterface<bool> {};

/* §5.4: base RTP + one FMD word, then the item zero-padded to a word; DIL >= 1. */
TEST_P(St40TxFmdBuildTest, DataItemFollowsFmdWordPaddedToWord) {
  for (uint32_t n : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 13u, 48u, 241u, 1435u}) {
    auto data = pattern(n);
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ut_txa_set_frame_payload(ctx_, data.data(), n);

    uint32_t rtp_len = 0;
    const uint8_t* rtp = build(GetParam(), &rtp_len);
    ASSERT_NE(rtp, nullptr) << "n=" << n;

    EXPECT_EQ(rtp_len, kFmdRtpLen + padded(n)) << "n=" << n;
    uint32_t word = be32_at(rtp + kRtpBaseLen);
    EXPECT_EQ(word >> 10, kDit) << "n=" << n;
    EXPECT_EQ((word >> 9) & 0x1, kKBit) << "n=" << n;
    EXPECT_EQ(word & 0x1ff, padded(n) / 4) << "n=" << n;
    EXPECT_EQ(memcmp(rtp + kFmdRtpLen, data.data(), n), 0) << "n=" << n;
    for (uint32_t i = n; i < padded(n); i++)
      EXPECT_EQ(rtp[kFmdRtpLen + i], 0) << "pad byte " << i << " n=" << n;
  }
}

/* The no-chain packet is the 58-byte FMD header plus the padded data item. */
TEST_F(St40TxFmdTest, FullPacketIsFmdHeaderPlusPaddedItem) {
  auto data = pattern(13);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), 13);
  struct rte_mbuf* m = mbuf(2048);
  ASSERT_NE(m, nullptr);

  ASSERT_GE(ut_txa_build_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_fmd_hdr_len(), 58u);
  EXPECT_EQ(ut_txa_pkt_pkt_len(m), 58u + padded(13));
}

/* §5.1/§5.4: an empty frame is a zero-item packet, never an item with DIL 0. */
TEST_P(St40TxFmdBuildTest, EmptyFrameSendsZeroItemPacket) {
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, nullptr, 0);

  Packet p;
  ASSERT_TRUE(build_packet(GetParam(), &p));

  EXPECT_EQ(p.rtp_len, kRtpBaseLen);
  EXPECT_EQ(p.pkt_len, 54u);
  EXPECT_EQ(be16_at(p.l2 + kUdpLenOffset), 8u + kRtpBaseLen);
  EXPECT_EQ(be16_at(p.l2 + kIpv4LenOffset), 28u + kRtpBaseLen);
}

/* §5.4: the largest item fills the UDP size limit and its 359 words fit the 9-bit DIL. */
TEST_P(St40TxFmdBuildTest, MaxDataItemFillsUdpSizeLimit) {
  std::vector<uint8_t> data(kMaxDataItem, 0x5a);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), kMaxDataItem);

  Packet p;
  ASSERT_TRUE(build_packet(GetParam(), &p));

  EXPECT_EQ(be16_at(p.l2 + kUdpLenOffset), kUdpSizeLimit);
  EXPECT_EQ(be16_at(p.l2 + kIpv4LenOffset), kUdpSizeLimit + 20);
  EXPECT_EQ(be32_at(p.rtp + kRtpBaseLen) & 0x1ff, 359u);
}

/* No extended seq: bytes 12..15 stay the FMD word and seq wraps in the base header. */
TEST_P(St40TxFmdBuildTest, SeqWrapsInBaseHeaderOnly) {
  auto data = pattern(8);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), 8);
  ut_txa_set_seq(ctx_, 0xffff);

  for (uint16_t expected_seq : {0xffff, 0x0000}) {
    uint32_t rtp_len = 0;
    const uint8_t* rtp = build(GetParam(), &rtp_len);
    ASSERT_NE(rtp, nullptr);
    EXPECT_EQ(be16_at(rtp + 2), expected_seq);
    EXPECT_EQ(be32_at(rtp + kRtpBaseLen), fmd_word(kDit, kKBit, 2))
        << "seq " << expected_seq;
  }
}

/* §5.2: the marker bit is 0 for all packets, with or without a data item. */
TEST_P(St40TxFmdBuildTest, MarkerBitIsZero) {
  auto data = pattern(8);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

  for (uint32_t n : {8u, 0u}) {
    ut_txa_set_frame_payload(ctx_, data.data(), n);
    uint32_t rtp_len = 0;
    const uint8_t* rtp = build(GetParam(), &rtp_len);
    ASSERT_NE(rtp, nullptr) << "n=" << n;
    EXPECT_FALSE(rtp[1] & 0x80) << "n=" << n;
  }
}

/* §5.2: V=2, P=0, X=0, CC=0. */
TEST_P(St40TxFmdBuildTest, FirstByteIsVersion2Only) {
  auto data = pattern(8);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), 8);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

  uint32_t rtp_len = 0;
  const uint8_t* rtp = build(GetParam(), &rtp_len);
  ASSERT_NE(rtp, nullptr);

  EXPECT_EQ(rtp[0], 0x80);
}

TEST_P(St40TxFmdBuildTest, OpsSsrcIsHonoured) {
  auto data = pattern(8);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), 8);
  ut_txa_set_ssrc(ctx_, 0x12345678);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

  uint32_t rtp_len = 0;
  const uint8_t* rtp = build(GetParam(), &rtp_len);
  ASSERT_NE(rtp, nullptr);

  EXPECT_EQ(be32_at(rtp + 8), 0x12345678u);
}

INSTANTIATE_TEST_SUITE_P(Builders, St40TxFmdBuildTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "NoChain" : "Chain";
                         });

/* The public header struct maps onto the network-order FMD word. */
TEST_F(St40TxFmdTest, RtpHdrStructMatchesWireLayout) {
  struct st40_fmd_rtp_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.fmd_hdr_chunk.data_item_type = kDit;
  hdr.fmd_hdr_chunk.data_item_k_bit = kKBit;
  hdr.fmd_hdr_chunk.data_item_length = 5;
  hdr.swapped_fmd_hdr_chunk = htonl(hdr.swapped_fmd_hdr_chunk);

  EXPECT_EQ(sizeof(hdr), kFmdRtpLen);
  EXPECT_EQ(be32_at(reinterpret_cast<const uint8_t*>(&hdr) + kRtpBaseLen),
            fmd_word(kDit, kKBit, 5));
}

class St40TxFmdPayloadTypeTest : public St40TxFmdTest {
 protected:
  uint8_t built_pt() {
    uint32_t rtp_len = 0;
    const uint8_t* rtp = build(true, &rtp_len);
    return rtp ? (rtp[1] & 0x7f) : 0;
  }
};

TEST_F(St40TxFmdPayloadTypeTest, FmdDefaultsTo115) {
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, nullptr, 0);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

  EXPECT_EQ(built_pt(), kFmdDefaultPt);
}

TEST_F(St40TxFmdPayloadTypeTest, AncStaysAt113) {
  ut_txa_set_frame_payload(ctx_, nullptr, 0);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

  EXPECT_EQ(built_pt(), kAncDefaultPt);
}

TEST_F(St40TxFmdPayloadTypeTest, FmdHonoursExplicitPayloadType) {
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, nullptr, 0);
  ASSERT_EQ(ut_txa_init_hdr(ctx_, 100), 0);

  EXPECT_EQ(built_pt(), 100);
}

/* FMD sends data/data_size as is; ANC meta set on the frame does not leak in. */
TEST_F(St40TxFmdTest, AncMetaIsIgnored) {
  auto data = pattern(13);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), 13);
  ut_txa_set_anc_meta(ctx_, 8, 1);

  uint32_t rtp_len = 0;
  const uint8_t* rtp = build(true, &rtp_len);
  ASSERT_NE(rtp, nullptr);

  EXPECT_EQ(rtp_len, kFmdRtpLen + padded(13));
  EXPECT_EQ(memcmp(rtp + kFmdRtpLen, data.data(), 13), 0);
}

/* The RFC 8331 path still writes ext seq and the payload length field. */
TEST_F(St40TxFmdTest, Rfc8331KeepsExtSeqAndLength) {
  auto udw = pattern(8);
  ut_txa_set_frame_payload(ctx_, udw.data(), 8);
  ut_txa_set_anc_meta(ctx_, 8, 1);
  ut_txa_set_seq(ctx_, 0xffff);

  uint32_t first_len = 0, second_len = 0;
  const uint8_t* first = build(true, &first_len);
  const uint8_t* second = build(true, &second_len);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  EXPECT_EQ(be16_at(first + 2), 0xffff);
  EXPECT_EQ(be16_at(first + 12), 0x0000) << "ext seq before wrap";
  EXPECT_EQ(be16_at(second + 2), 0x0000);
  EXPECT_EQ(be16_at(second + 12), 0x0001) << "ext seq after wrap";
  EXPECT_EQ(second_len, kRfc8331RtpLen + st40_rfc8331_payload_bytes(8));
  EXPECT_EQ(be16_at(second + 14), st40_rfc8331_payload_bytes(8)) << "length";
}

/* kPayloadLen is a word multiple, so the padded write equals the granted room. */
class St40TxFmdCapacityTest : public St40TxFmdTest {
 protected:
  static constexpr uint32_t kPayloadLen = 48;
  uint8_t payload_[kPayloadLen] = {};

  void SetUp() override {
    St40TxFmdTest::SetUp();
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ut_txa_set_frame_payload(ctx_, payload_, kPayloadLen);
  }
};

TEST_F(St40TxFmdCapacityTest, RejectsBufferTooSmallForHeader) {
  struct rte_mbuf* m = mbuf(ut_txa_fmd_hdr_len() - 1);
  ASSERT_NE(m, nullptr);

  EXPECT_LT(ut_txa_build_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_pkt_data_len(m), 0u);
  EXPECT_EQ(ut_txa_pkt_pkt_len(m), 0u);
}

TEST_F(St40TxFmdCapacityTest, BuildsPacketThatExactlyFits) {
  size_t room = ut_txa_fmd_hdr_len() + kPayloadLen;
  struct rte_mbuf* m = mbuf(room);
  ASSERT_NE(m, nullptr);

  EXPECT_GE(ut_txa_build_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_pkt_data_len(m), room);
  EXPECT_EQ(ut_txa_pkt_pkt_len(m), room);
}

TEST_F(St40TxFmdCapacityTest, RejectsBufferOneByteShortOfPayload) {
  struct rte_mbuf* m = mbuf(ut_txa_fmd_hdr_len() + kPayloadLen - 1);
  ASSERT_NE(m, nullptr);

  EXPECT_LT(ut_txa_build_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_pkt_pkt_len(m), 0u);
}

TEST_F(St40TxFmdCapacityTest, ChainRejectsBufferOneByteShortOfPayload) {
  struct rte_mbuf* m = mbuf(kFmdRtpLen + kPayloadLen - 1);
  ASSERT_NE(m, nullptr);

  EXPECT_LT(ut_txa_build_rtp_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_pkt_pkt_len(m), 0u);
}

/* The largest data item exactly fills the chain payload mbuf. */
TEST_F(St40TxFmdTest, MaxDataItemFitsChainRoom) {
  std::vector<uint8_t> data(kMaxDataItem, 0x5a);
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_frame_payload(ctx_, data.data(), kMaxDataItem);
  struct rte_mbuf* m = mbuf(ut_txa_chain_room());
  ASSERT_NE(m, nullptr);

  EXPECT_EQ(ut_txa_build_rtp_packet(ctx_, m), 0);

  EXPECT_EQ(ut_txa_pkt_data_len(m), ut_txa_chain_room());
}

/* One byte past the largest data item aborts the frame and hands the buffer back. */
TEST_F(St40TxFmdTest, OversizedDataItemAbortsFrame) {
  std::vector<uint8_t> data(kMaxDataItem + 1, 0x5a);
  prepare_fmd_frame(data.data(), static_cast<uint32_t>(data.size()));

  step_frame();

  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_stat_build_ret_code(ctx_), ut_txa_err_anc_too_large());
  EXPECT_TRUE(ut_txa_session_waiting_frame(ctx_));
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
}

class St40TxFmdTaskletTest : public St40TxFmdTest {
 protected:
  static constexpr long double kFrameNs = 1e9L * 1001 / 60000; /* 59.94 fps */
  static constexpr uint64_t kFirstEpoch = 100;
  std::vector<uint8_t> data_ = pattern(13);
  std::vector<uint64_t> tsc_;
  std::vector<std::vector<uint8_t>> rtp_;
  uint64_t now_ = 0;

  void set_time(uint64_t ns) {
    now_ = ns;
    ut_txa_set_mock_ptp_time(ctx_, ns);
    ut_txa_set_mock_tsc_time(ctx_, ns);
  }

  /* `frames` frames of `per_ts` zero-item app packets each through the RTP tasklet. */
  void run_rtp_frames(int frames, int per_ts) {
    ut_txa_set_frame_time(ctx_, kFrameNs);
    ut_txa_set_cur_epochs(ctx_, kFirstEpoch - 1);
    set_time(static_cast<uint64_t>(kFirstEpoch * kFrameNs) - 1000ull * 1000);
    ASSERT_EQ(ut_txa_prepare_rtp_tasklet(ctx_), 0);
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);

    for (int f = 0; f < frames; f++) {
      uint8_t pkt[kRtpBaseLen] = {0x80, 96};
      uint32_t tmstamp = htonl(f + 1);
      memcpy(&pkt[4], &tmstamp, sizeof(tmstamp));
      for (int p = 0; p < per_ts; p++)
        ASSERT_EQ(ut_txa_put_app_rtp(ctx_, pkt, sizeof(pkt)), 0);
      int sent = 0;
      for (int step = 0; sent < per_ts && step < 4 * per_ts; step++) {
        ut_txa_step_frame_tasklet(ctx_);
        uint64_t tsc = 0;
        uint8_t out[64];
        while (ut_txa_pop_packet(ctx_, &tsc, out, sizeof(out)) >= 0) {
          tsc_.push_back(tsc);
          sent++;
        }
        set_time(std::max(now_, ut_txa_tsc_time_cursor(ctx_)));
      }
      ASSERT_EQ(sent, per_ts) << "frame " << f;
    }
  }

  /* `frames` frames through the frame tasklet, PTP and TSC advancing together. */
  void run_frames(int frames) {
    ut_txa_set_frame_time(ctx_, kFrameNs);
    ut_txa_set_cur_epochs(ctx_, kFirstEpoch - 1);
    set_time(static_cast<uint64_t>(kFirstEpoch * kFrameNs) - 1000ull * 1000);
    ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, 1), 0);
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ASSERT_EQ(ut_txa_init_hdr(ctx_, 0), 0);
    ut_txa_set_frame_payload(ctx_, data_.data(), static_cast<uint32_t>(data_.size()));
    ut_txa_set_frame_count(ctx_, frames);

    for (int i = 0; i < frames; i++) {
      ut_txa_step_frame_tasklet(ctx_);
      set_time(ut_txa_tsc_time_cursor(ctx_));
      ut_txa_step_frame_tasklet(ctx_);
      uint64_t tsc = 0;
      std::vector<uint8_t> pkt(2048);
      int len =
          ut_txa_pop_packet(ctx_, &tsc, pkt.data(), static_cast<uint32_t>(pkt.size()));
      ASSERT_GT(len, static_cast<int>(ut_txa_udp_hdr_len())) << "frame " << i;
      tsc_.push_back(tsc);
      rtp_.emplace_back(pkt.begin() + static_cast<ptrdiff_t>(ut_txa_udp_hdr_len()),
                        pkt.begin() + len);
    }
  }
};

/* §5.2: the marker bit stays 0 frame after frame. */
TEST_F(St40TxFmdTaskletTest, MarkerStaysZeroAcrossFrames) {
  ASSERT_NO_FATAL_FAILURE(run_frames(8));

  for (size_t i = 0; i < rtp_.size(); i++) EXPECT_FALSE(rtp_[i][1] & 0x80) << i;
}

/* MTL ST40 90 kHz pacing: one frame period per frame, 1501.5 ticks at 59.94 fps */
TEST_F(St40TxFmdTaskletTest, RtpTimestampStepsByFramePeriod) {
  ASSERT_NO_FATAL_FAILURE(run_frames(8));

  for (size_t i = 1; i < rtp_.size(); i++) {
    uint32_t step = be32_at(rtp_[i].data() + 4) - be32_at(rtp_[i - 1].data() + 4);
    EXPECT_TRUE(step == 1501 || step == 1502) << "frame " << i << " step " << step;
    if (i >= 2) {
      EXPECT_EQ(be32_at(rtp_[i].data() + 4) - be32_at(rtp_[i - 2].data() + 4), 3003u);
    }
  }
}

/* The checker must catch a violation: at 60 pps C_MAX is 4. */
TEST(St40TxFmdNcmTest, CheckerPassesFourSimultaneousPacketsAndFailsFive) {
  const std::vector<uint64_t> four(4, 0), five(5, 0);

  EXPECT_EQ(ncm_c_max(60), 4);
  EXPECT_LE(ncm_max_c_inst(four, 60), ncm_c_max(60));
  EXPECT_GT(ncm_max_c_inst(five, 60), ncm_c_max(60));
}

/* §7 over 2 s at 59.94 fps, on scheduled TX time rather than wire egress */
TEST_F(St40TxFmdTaskletTest, PacketsMeetNetworkCompatibilityModel) {
  ASSERT_NO_FATAL_FAILURE(run_frames(120));
  const double r_nominal = 60000.0 / 1001;

  EXPECT_LE(ncm_max_c_inst(tsc_, r_nominal), ncm_c_max(r_nominal));
}

/* §7 with 4 RTP level packets per timestamp, on scheduled TX time, not wire egress */
TEST_F(St40TxFmdTaskletTest,
       RtpLevelFourPacketsPerTimestampMeetNetworkCompatibilityModel) {
  ASSERT_NO_FATAL_FAILURE(run_rtp_frames(120, 4));
  const double r_nominal = 4 * 60000.0 / 1001;

  ASSERT_EQ(tsc_.size(), 480u);
  EXPECT_LE(ncm_max_c_inst(tsc_, r_nominal), ncm_c_max(r_nominal));
}

/* Stale ANC meta[] under split mode, as the debug test patterns force, would be one
 * packet per meta entry; the FMD frame is still one packet. */
TEST_F(St40TxFmdTest, TaskletSendsOnePacketDespiteStaleAncMeta) {
  auto data = pattern(13);
  prepare_fmd_frame(data.data(), 13, 2);
  ut_txa_set_anc_meta(ctx_, 255, ST40_MAX_META);

  step_frame();

  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
  EXPECT_TRUE(ut_txa_session_waiting_frame(ctx_));
}

/* An app-built FMD packet whose first data word reads as RFC 8331 F=0b11. */
std::vector<uint8_t> app_fmd_packet() {
  struct st40_fmd_rtp_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.base.version = 2;
  hdr.base.payload_type = 96;
  hdr.base.tmstamp = htonl(1234);
  hdr.fmd_hdr_chunk.data_item_type = kDit;
  hdr.fmd_hdr_chunk.data_item_k_bit = kKBit;
  hdr.fmd_hdr_chunk.data_item_length = 2;
  hdr.swapped_fmd_hdr_chunk = htonl(hdr.swapped_fmd_hdr_chunk);

  std::vector<uint8_t> pkt(sizeof(hdr) + 8, 0x3c);
  memcpy(pkt.data(), &hdr, sizeof(hdr));
  pkt[kFmdRtpLen] = 0x01;
  pkt[kFmdRtpLen + 1] = 0xc0;
  return pkt;
}

/* An app-built RFC 8331 header, first chunk in host order as the lib expects it. */
std::vector<uint8_t> app_rfc8331_packet(uint8_t f_bits) {
  struct st40_rfc8331_rtp_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.base.version = 2;
  hdr.base.marker = 1;
  hdr.base.tmstamp = htonl(1234);
  hdr.first_hdr_chunk.anc_count = 1;
  hdr.first_hdr_chunk.f = f_bits;

  std::vector<uint8_t> pkt(sizeof(hdr));
  memcpy(pkt.data(), &hdr, sizeof(hdr));
  return pkt;
}

uint32_t host_word_at(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

/* Everything but the RTP timestamp (bytes 4..7), which the lib re-paces. */
void expect_same_but_tmstamp(const uint8_t* got, const std::vector<uint8_t>& sent) {
  EXPECT_EQ(memcmp(got, sent.data(), 4), 0);
  EXPECT_EQ(memcmp(got + 8, sent.data() + 8, sent.size() - 8), 0);
}

class St40TxRtpLevelTest : public St40TxFmdTest {
 protected:
  /* RTP bytes of `sent` after the no-chain update, behind eth/ipv4/udp. */
  const uint8_t* update_no_chain(const std::vector<uint8_t>& sent) {
    size_t off = ut_txa_udp_hdr_len();
    struct rte_mbuf* m = mbuf(2048);
    if (!m) return nullptr;
    memcpy(ut_txa_pkt_data(m) + off, sent.data(), sent.size());
    ut_txa_set_pkt_len(m, off + sent.size());
    if (ut_txa_rtp_update_packet(ctx_, m) < 0) return nullptr;
    return ut_txa_pkt_data(m) + off;
  }

  /* RTP bytes of `sent` after being chained behind a header mbuf. */
  const uint8_t* update_chain(const std::vector<uint8_t>& sent) {
    struct rte_mbuf* hdr = mbuf(2048);
    struct rte_mbuf* rtp = ut_txa_alloc_mbuf(2048);
    if (!hdr || !rtp) {
      if (rtp) ut_txa_free_mbuf(rtp);
      return nullptr;
    }
    memcpy(ut_txa_pkt_data(rtp), sent.data(), sent.size());
    ut_txa_set_pkt_len(rtp, sent.size());
    ut_txa_build_packet_chain(ctx_, hdr, rtp);
    return ut_txa_pkt_data(rtp);
  }
};

TEST_F(St40TxRtpLevelTest, FmdNoChainLeavesPacketUntouched) {
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_rtp_level(ctx_, true);
  auto sent = app_fmd_packet();

  const uint8_t* got = update_no_chain(sent);
  ASSERT_NE(got, nullptr);

  expect_same_but_tmstamp(got, sent);
  EXPECT_EQ(ut_txa_stat_interlace_first(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_interlace_second(ctx_), 0u);
}

TEST_F(St40TxRtpLevelTest, FmdChainLeavesPacketUntouched) {
  ut_txa_set_fmd(ctx_, kDit, kKBit);
  ut_txa_set_rtp_level(ctx_, true);
  auto sent = app_fmd_packet();

  const uint8_t* got = update_chain(sent);
  ASSERT_NE(got, nullptr);

  expect_same_but_tmstamp(got, sent);
  EXPECT_EQ(ut_txa_stat_interlace_first(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_interlace_second(ctx_), 0u);
}

void put_be32(std::vector<uint8_t>& v, uint32_t word) {
  for (int shift = 24; shift >= 0; shift -= 8)
    v.push_back(static_cast<uint8_t>(word >> shift));
}

/* §5.2/§5.4: X=1 with an RFC 8285 extension, then two items of two DITs, marker 0. */
std::vector<uint8_t> app_fmd_packet_two_items() {
  std::vector<uint8_t> pkt = {0x90, 96, 0x00, 0x07};
  put_be32(pkt, 1234);
  put_be32(pkt, 0x11223344);
  put_be32(pkt, 0xbede0001); /* one-byte header extension, one word */
  put_be32(pkt, 0x10aa0000);
  put_be32(pkt, fmd_word(0x000001, 0, 1));
  put_be32(pkt, 0xc0c1c2c3);
  put_be32(pkt, fmd_word(0x200002, 1, 2));
  put_be32(pkt, 0xd0d1d2d3);
  put_be32(pkt, 0xd4d5d6d7);
  return pkt;
}

/* §5.1: a zero-item packet is the bare RTP header. */
std::vector<uint8_t> app_zero_item_packet() {
  std::vector<uint8_t> pkt = {0x80, 96, 0x00, 0x08};
  put_be32(pkt, 1234);
  put_be32(pkt, 0x11223344);
  return pkt;
}

class St40TxRtpLevelFmdTest : public St40TxRtpLevelTest,
                              public ::testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    St40TxRtpLevelTest::SetUp();
    ut_txa_set_fmd(ctx_, kDit, kKBit);
    ut_txa_set_rtp_level(ctx_, false);
  }

  const uint8_t* update(const std::vector<uint8_t>& sent) {
    return GetParam() ? update_no_chain(sent) : update_chain(sent);
  }
};

TEST_P(St40TxRtpLevelFmdTest, ExtensionAndTwoItemsPassUntouched) {
  auto sent = app_fmd_packet_two_items();

  const uint8_t* got = update(sent);
  ASSERT_NE(got, nullptr);

  expect_same_but_tmstamp(got, sent);
}

TEST_P(St40TxRtpLevelFmdTest, ZeroItemPacketPassesUntouched) {
  auto sent = app_zero_item_packet();

  const uint8_t* got = update(sent);
  ASSERT_NE(got, nullptr);

  expect_same_but_tmstamp(got, sent);
}

INSTANTIATE_TEST_SUITE_P(Builders, St40TxRtpLevelFmdTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "NoChain" : "Chain";
                         });

TEST_F(St40TxRtpLevelTest, Rfc8331NoChainSwapsChunkAndCountsField) {
  ut_txa_set_rtp_level(ctx_, true);
  auto sent = app_rfc8331_packet(0x3);

  const uint8_t* got = update_no_chain(sent);
  ASSERT_NE(got, nullptr);

  EXPECT_EQ(be32_at(got + 16), host_word_at(sent.data() + 16));
  EXPECT_EQ(ut_txa_stat_interlace_second(ctx_), 1u);
}

TEST_F(St40TxRtpLevelTest, Rfc8331ChainSwapsChunk) {
  /* progressive: only the unconditional chunk swap is under test */
  ut_txa_set_rtp_level(ctx_, false);
  auto sent = app_rfc8331_packet(0x0);

  const uint8_t* got = update_chain(sent);
  ASSERT_NE(got, nullptr);

  EXPECT_EQ(be32_at(got + 16), host_word_at(sent.data() + 16));
}

TEST_F(St40TxFmdTest, OpsCheckRejectsFmdWithSplitAnc) {
  EXPECT_EQ(ut_txa_ops_check(ST40_TX_FLAG_FAST_METADATA | ST40_TX_FLAG_SPLIT_ANC_BY_PKT,
                             0, 0, 0),
            -EINVAL);
}

TEST_F(St40TxFmdTest, OpsCheckRejectsKBitAboveOne) {
  EXPECT_EQ(ut_txa_ops_check(ST40_TX_FLAG_FAST_METADATA, 0, 2, 0), -EINVAL);
}

struct OpsCheckCase {
  uint32_t value;
  bool accepted;
};

std::string ops_check_case_name(const OpsCheckCase& c, const char* value_fmt) {
  char value[16];
  (void)snprintf(value, sizeof(value), value_fmt, c.value);
  return std::string(c.accepted ? "Accepts" : "Rejects") + value;
}

class St40TxFmdDitCheckTest : public St40TxFmdTest,
                              public ::testing::WithParamInterface<OpsCheckCase> {};

/* §8: 0x300000-0x3FEFFF is reserved, and a DIT is 22 bits. */
TEST_P(St40TxFmdDitCheckTest, OpsCheck) {
  EXPECT_EQ(ut_txa_ops_check(ST40_TX_FLAG_FAST_METADATA, GetParam().value, 1, 0),
            GetParam().accepted ? 0 : -EINVAL);
}

INSTANTIATE_TEST_SUITE_P(
    Ranges, St40TxFmdDitCheckTest,
    ::testing::Values(OpsCheckCase{0x000000, true}, OpsCheckCase{0x0fffff, true},
                      OpsCheckCase{0x100000, true}, OpsCheckCase{0x1fffff, true},
                      OpsCheckCase{0x200000, true}, OpsCheckCase{0x2fffff, true},
                      OpsCheckCase{0x3ff000, true}, OpsCheckCase{0x3fffff, true},
                      OpsCheckCase{0x300000, false}, OpsCheckCase{0x3fefff, false},
                      OpsCheckCase{0x400000, false}, OpsCheckCase{0xffffffff, false}),
    [](const ::testing::TestParamInfo<OpsCheckCase>& info) {
      return ops_check_case_name(info.param, "0x%x");
    });

class St40TxFmdPayloadTypeCheckTest : public St40TxFmdTest,
                                      public ::testing::WithParamInterface<OpsCheckCase> {
};

/* §5.2: a dynamic payload type, 96-127; 0 picks the 115 default. */
TEST_P(St40TxFmdPayloadTypeCheckTest, OpsCheck) {
  uint8_t pt = static_cast<uint8_t>(GetParam().value);
  EXPECT_EQ(ut_txa_ops_check(ST40_TX_FLAG_FAST_METADATA, kDit, 1, pt),
            GetParam().accepted ? 0 : -EINVAL);
}

INSTANTIATE_TEST_SUITE_P(Values, St40TxFmdPayloadTypeCheckTest,
                         ::testing::Values(OpsCheckCase{0, true}, OpsCheckCase{96, true},
                                           OpsCheckCase{115, true},
                                           OpsCheckCase{127, true},
                                           OpsCheckCase{1, false},
                                           OpsCheckCase{95, false}),
                         [](const ::testing::TestParamInfo<OpsCheckCase>& info) {
                           return ops_check_case_name(info.param, "%u");
                         });

TEST_F(St40TxFmdTest, Rfc8331OpsCheckAcceptsStaticPayloadTypes) {
  EXPECT_EQ(ut_txa_ops_check(0, 0, 0, 1), 0);
  EXPECT_EQ(ut_txa_ops_check(0, 0, 0, 95), 0);
}
