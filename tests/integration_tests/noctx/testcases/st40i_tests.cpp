/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "core/test_fixture.hpp"
#include "handlers/st40p_handler.hpp"
#include "test_harness.hpp"

namespace {

/* TX writes one ANC packet per entry of anc_sizes, each with its own byte pattern.
 * Checks on every RX frame:
 * 1. meta_num and pkts_total == anc_sizes.size(), RTP marker set
 * 2. no sequence discontinuity, seq_lost == 0
 * 3. each ANC's udw_size, udw_offset and bytes as sent, udw_buffer_fill == their sum
 */
class SplitAncRoundTripOracle : public FrameTestStrategy {
 public:
  explicit SplitAncRoundTripOracle(std::vector<uint16_t> anc_sizes)
      : FrameTestStrategy(nullptr, true, true), anc_sizes_(std::move(anc_sizes)) {
  }

  void txTestFrameModifier(void* frame, size_t /*frame_size*/) override {
    auto* info = static_cast<st40_frame_info*>(frame);
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->meta, nullptr);
    ASSERT_NE(info->udw_buff_addr, nullptr);

    uint32_t total_bytes = 0;
    uint32_t offset = 0;
    info->meta_num = anc_sizes_.size();
    for (size_t i = 0; i < anc_sizes_.size(); i++) {
      const uint16_t udw_size = anc_sizes_[i];
      struct st40_meta& meta = info->meta[i];
      meta.c = 0;
      meta.line_number = 10 + static_cast<uint16_t>(i);
      meta.hori_offset = 0;
      meta.s = 0;
      meta.stream_num = 0;
      meta.did = 0x45;
      meta.sdid = 0x01;
      meta.udw_size = udw_size;
      meta.udw_offset = offset;

      for (uint16_t j = 0; j < udw_size; j++) {
        info->udw_buff_addr[offset + j] = static_cast<uint8_t>((i + 1) * 7 + j);
      }

      offset += udw_size;
    }
    total_bytes = offset;
    ASSERT_LE(total_bytes, info->udw_buffer_size);
    info->udw_buffer_fill = total_bytes;
  }

  void rxTestFrameModifier(void* frame, size_t /*frame_size*/) override {
    auto* info = static_cast<st40_frame_info*>(frame);
    ASSERT_NE(info, nullptr);

    EXPECT_EQ(info->meta_num, anc_sizes_.size());
    EXPECT_TRUE(info->rtp_marker);
    EXPECT_FALSE(info->seq_discont);
    EXPECT_EQ(info->seq_lost, 0u);
    EXPECT_EQ(info->pkts_total, anc_sizes_.size());

    uint32_t offset = 0;
    for (size_t i = 0; i < anc_sizes_.size(); i++) {
      const uint16_t udw_size = anc_sizes_[i];
      const auto& meta = info->meta[i];
      EXPECT_EQ(meta.udw_size, udw_size);
      EXPECT_EQ(meta.udw_offset, offset);
      for (uint16_t j = 0; j < udw_size; j++) {
        EXPECT_EQ(info->udw_buff_addr[offset + j], static_cast<uint8_t>((i + 1) * 7 + j));
      }
      offset += udw_size;
    }
    EXPECT_EQ(info->udw_buffer_fill, offset);
  }

 private:
  std::vector<uint16_t> anc_sizes_;
};

}  // namespace

namespace {

static void build_split_rtp_packet(std::vector<uint8_t>& out, uint16_t seq, uint32_t ts,
                                   bool marker, const std::vector<uint8_t>& payload) {
  const uint32_t payload_bytes =
      st40_rfc8331_payload_bytes(static_cast<uint16_t>(payload.size()));
  out.assign(sizeof(st40_rfc8331_rtp_hdr) + payload_bytes, 0);

  auto* rtp = reinterpret_cast<st40_rfc8331_rtp_hdr*>(out.data());
  rtp->base.version = 2;
  rtp->base.payload_type = 113;
  rtp->base.seq_number = htons(seq);
  rtp->seq_number_ext = 0;
  rtp->base.tmstamp = htonl(ts);
  rtp->base.marker = marker ? 1 : 0;
  rtp->first_hdr_chunk.anc_count = 1;
  rtp->length = htons(payload_bytes);
  st40_rfc8331_rtp_hdr_bswap(rtp);

  auto* ph = reinterpret_cast<st40_rfc8331_payload_hdr*>(rtp + 1);
  ph->first_hdr_chunk.c = 0;
  ph->first_hdr_chunk.line_number = 1;
  ph->first_hdr_chunk.horizontal_offset = 0;
  ph->first_hdr_chunk.s = 0;
  ph->first_hdr_chunk.stream_num = 0;
  ph->second_hdr_chunk.did = st40_add_parity_bits(0x45);
  ph->second_hdr_chunk.sdid = st40_add_parity_bits(0x01);
  ph->second_hdr_chunk.data_count = st40_add_parity_bits(payload.size());
  /* Swap the payload header to wire order before packing UDW bits and checksum so the
   * 32-bit swap does not scramble the upper bits of UDW[0] or invalidate the checksum. */
  st40_rfc8331_payload_hdr_bswap(ph);

  uint8_t* udw_dst = reinterpret_cast<uint8_t*>(&ph->second_hdr_chunk);
  for (size_t i = 0; i < payload.size(); i++) {
    st40_set_udw(static_cast<int>(i + 3), st40_add_parity_bits(payload[i]), udw_dst);
  }
  uint16_t checksum = st40_calc_checksum(3 + payload.size(), udw_dst);
  st40_set_udw(static_cast<int>(payload.size() + 3), checksum, udw_dst);
}

static int send_rtp_burst_dpdk(const st_tests_context* ctx, uint16_t port,
                               uint8_t payload_type,
                               const std::vector<std::vector<uint8_t>>& pkts) {
  struct st40_tx_ops tx_ops = {};
  tx_ops.name = "st40i_seq_gap_tx";
  tx_ops.num_port = 1;
  memcpy(tx_ops.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(tx_ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  tx_ops.udp_port[MTL_SESSION_PORT_P] = port;
  tx_ops.type = ST40_TYPE_RTP_LEVEL;
  tx_ops.fps = ST_FPS_P60;
  tx_ops.payload_type = payload_type;
  tx_ops.rtp_ring_size = 16;
  tx_ops.notify_rtp_done = [](void*) { return 0; };

  mtl_test::ScopedHandle<st40_tx_handle, st40_tx_free> tx(
      st40_tx_create(ctx->handle, &tx_ops));
  if (!tx) return -EIO;

  for (const auto& pkt : pkts) {
    void* usrptr = nullptr;
    void* mbuf = nullptr;
    const bool got = mtl_test::wait_until(
        [&]() {
          mbuf = st40_tx_get_mbuf(tx.get(), &usrptr);
          return mbuf != nullptr;
        },
        1000, 1);
    if (!got || !mbuf || !usrptr) return -ETIMEDOUT;
    memcpy(usrptr, pkt.data(), pkt.size());
    int ret = st40_tx_put_mbuf(tx.get(), mbuf, static_cast<uint16_t>(pkt.size()));
    if (ret < 0) return ret;
  }

  /* Give the TX tasklet time to pace out both RTP packets before freeing TX. */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return 0;
}

}  // namespace

/* Common to every test here: initDefaultContext(); one session TX TEST_PORT_1 -> RX
 * TEST_PORT_2, 60p, 4 buffers, TX and RX BLOCK_GET (fillSt40pOps()), default pacing;
 * StartFakePtpClock() before the sessions start. Every TX and RX frame also passes the
 * St40pHandler thread checks. No SKIP rule; st40i_split_seq_gap_reports_loss asserts
 * that the sequence-gap frame arrives.
 */

/* st40i_smoke
 * Config: kInterlaced TX and RX, TX kFps = 50 fields/s; no strategy.
 * Expect: after 2 s (~100 fields) and stop: txFrames() > 0, rxFrames() > 0, equal.
 */
TEST_F(NoCtxTest, st40i_smoke) {
  initDefaultContext();

  constexpr bool kInterlaced = true;
  constexpr enum st_fps kFps = ST_FPS_P50;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      /*strategyFactory=*/nullptr, [](St40pHandler* handler) {
        handler->sessionsOpsTx.interlaced = kInterlaced;
        handler->sessionsOpsRx.interlaced = kInterlaced;
        handler->sessionsOpsTx.fps = kFps;
      });

  auto* handler = bundle.handler;
  ASSERT_NE(handler, nullptr);

  StartFakePtpClock();
  handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  sleepUntilFailure(2);

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "st40i_smoke transmitted no frames";
  ASSERT_GT(handler->rxFrames(), 0u) << "st40i_smoke received no frames";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40i_smoke TX/RX frame count mismatch";
}

/* st40i_split_flag_accepts_and_propagates
 * Config: TX kTxFlags = SPLIT_ANC_BY_PKT, RX progressive; one 1-byte ANC per frame.
 * Expect: on every frame, SplitAncRoundTripOracle checks 1-3; after 1 s and stop:
 *         txFrames() > 0, rxFrames() > 0.
 */
TEST_F(NoCtxTest, st40i_split_flag_accepts_and_propagates) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      /*strategyFactory=*/[](St40pHandler*) { return new SplitAncRoundTripOracle({1}); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsRx.interlaced = false;
      });

  ASSERT_NE(bundle.handler, nullptr);

  StartFakePtpClock();
  bundle.handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  sleepUntilFailure(1);

  bundle.handler->stopSession();

  EXPECT_GT(bundle.handler->txFrames(), 0u);
  EXPECT_GT(bundle.handler->rxFrames(), 0u);
}

/* st40i_split_multi_packet_roundtrip
 * Config: TX kTxFlags = SPLIT_ANC_BY_PKT, RX progressive, kFramebuffCnt = 4 TX and RX;
 *         ANC packets of 8, 6 and 4 bytes per frame.
 * Expect: on every frame, SplitAncRoundTripOracle checks 1-3; after 1 s and stop:
 *         rxFrames() >= 1.
 */
TEST_F(NoCtxTest, st40i_split_multi_packet_roundtrip) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
  constexpr uint16_t kFramebuffCnt = 4;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      /*strategyFactory=*/
      [](St40pHandler*) { return new SplitAncRoundTripOracle({8, 6, 4}); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsRx.interlaced = false;
        handler->sessionsOpsTx.framebuff_cnt = kFramebuffCnt;
        handler->sessionsOpsRx.framebuff_cnt = kFramebuffCnt;
      });

  ASSERT_NE(bundle.handler, nullptr);

  StartFakePtpClock();
  bundle.handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  sleepUntilFailure(1);

  bundle.handler->stopSession();

  EXPECT_GE(bundle.handler->rxFrames(), 1u);
}

/* st40i_split_loopback
 * Config: TX kTxFlags = SPLIT_ANC_BY_PKT, kInterlaced TX and RX, TX kFps = 50 fields/s;
 *         two 4-byte ANC packets per frame.
 * Expect: on every field, SplitAncRoundTripOracle checks 1-3; after 1 s and stop:
 *         txFrames() > 0, rxFrames() > 0.
 */
TEST_F(NoCtxTest, st40i_split_loopback) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
  constexpr bool kInterlaced = true;
  constexpr enum st_fps kFps = ST_FPS_P50;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      /*strategyFactory=*/
      [](St40pHandler*) { return new SplitAncRoundTripOracle({4, 4}); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsRx.interlaced = kInterlaced;
        handler->sessionsOpsTx.interlaced = kInterlaced;
        handler->sessionsOpsTx.fps = kFps;
      });

  ASSERT_NE(bundle.handler, nullptr);

  StartFakePtpClock();
  bundle.handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  sleepUntilFailure(1);

  bundle.handler->stopSession();

  EXPECT_GT(bundle.handler->txFrames(), 0u);
  EXPECT_GT(bundle.handler->rxFrames(), 0u);
}

/* st40i_split_seq_gap_reports_loss
 * Config: RX only, progressive, UDP udp_port, payload type kPayloadType, BLOCK_GET.
 * Plan:   no background RX thread; send two RTP packets of one frame (seq 100 and 102,
 *         marker on the second) over a DPDK st40_tx RTP-level session on port P, then
 *         read the assembled frame directly on the test thread.
 * Expect: 1. frame received, seq_discont, seq_lost >= 1, rtp_marker, meta_num == 2
 */
TEST_F(NoCtxTest, st40i_split_seq_gap_reports_loss) {
  /* Force the no-chain RTP-level TX path so the library transmits the pre-swapped
   * RFC 8331 RTP header as-is on both chain-capable and no-chain NICs instead of
   * swapping first_hdr_chunk a second time in tx_ancillary_session_build_packet_chain().
   */
  ctx->para.flags |= MTL_FLAG_TX_NO_CHAIN;
  initDefaultContext();

  constexpr uint16_t udp_port = 33000;
  constexpr uint8_t kPayloadType = 113;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      /*strategyFactory=*/nullptr, [](St40pHandler* handler) {
        handler->sessionsOpsRx.interlaced = false;
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = udp_port;
        handler->sessionsOpsRx.port.payload_type = kPayloadType;
        handler->sessionsOpsRx.flags |= ST40P_RX_FLAG_BLOCK_GET;
      });

  ASSERT_NE(bundle.handler, nullptr);
  ASSERT_EQ(st40p_rx_set_block_timeout(bundle.handler->sessionsHandleRx, 500 * NS_PER_MS),
            0);

  StartFakePtpClock();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  std::vector<uint8_t> pkt1;
  std::vector<uint8_t> pkt2;
  build_split_rtp_packet(pkt1, /*seq=*/100, /*ts=*/1234, /*marker=*/false,
                         {0x11, 0x22, 0x33, 0x44});
  build_split_rtp_packet(pkt2, /*seq=*/102, /*ts=*/1234, /*marker=*/true,
                         {0x11, 0x22, 0x33, 0x44});

  ASSERT_EQ(0, send_rtp_burst_dpdk(ctx, udp_port, kPayloadType, {pkt1, pkt2}))
      << "send_rtp_burst_dpdk failed";

  struct st40_frame_info* frame_info = nullptr;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    frame_info = st40p_rx_get_frame(bundle.handler->sessionsHandleRx);
    if (frame_info) break;
  }

  ASSERT_NE(frame_info, nullptr) << "RX frame with sequence gap was not received";
  EXPECT_TRUE(frame_info->seq_discont);
  EXPECT_GE(frame_info->seq_lost, 1u);
  EXPECT_TRUE(frame_info->rtp_marker);
  EXPECT_EQ(frame_info->meta_num, 2u);

  st40p_rx_put_frame(bundle.handler->sessionsHandleRx, frame_info);
}
