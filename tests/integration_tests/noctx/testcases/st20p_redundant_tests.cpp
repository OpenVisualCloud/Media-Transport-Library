/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

/* Indexes into --port_list. */
constexpr int kPrimaryTxPort = 0;
constexpr int kRedundantTxPort = 1;
constexpr int kRxPortP = 2;
constexpr int kRxPortR = 3;

/* st20p_redundant_latency_drops_even_odd
 * Config:    4 ports, else std::runtime_error. MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS:
 *            kPrimaryTxPort drops even packets, kRedundantTxPort odd ones;
 *            initDefaultContext(). 1080p25 (T = 40 ms), 3 buffers.
 *            RX kRxPortP + kRxPortR on kUdpPortP / kUdpPortR (P and R);
 *            primary TX kPrimaryTxPort -> P mcast, kUdpPortP;
 *            redundant TX kRedundantTxPort -> R mcast, kUdpPortR,
 *            rtp_timestamp_delta_us = -testedLatencyMs * 1000;
 *            both TX kTxFlags = USER_PACING | USER_TIMESTAMP.
 * Plan:      St20pRedundantStreamPlan from PTP zero (StartFakePtpClock() before
 *            mtl_start()): primary t_user(n) = 50 ms + n*T, redundant 50 ms +
 *            testedLatencyMs + n*T (kSt20pRedundantStartMs), each snapped to its
 *            nearest epoch. Runs testDurationS = 120 s.
 * Expect:    RX stats read before the sessions stop, frame counts after:
 *            1. primary idx_tx == RX idx_rx +- 1 %
 *            2. per-port lost_packets > 0 on P and on R
 *            3. stat_lost_packets == lost P + lost R
 *            4. each port loses 50 +- 5 % of its packets
 *            5. stat_frames_incomplete == 0, stat_pkts_unrecovered == 0
 * Skip/Fail: SKIP unless built with MTL_SIMULATE_PACKET_DROPS (debug, debugonly).
 */
TEST_F(NoCtxTest, st20p_redundant_latency_drops_even_odd) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st20p_redundant_latency test ctx needs at least 4 ports");
  }

/**
 * This test requires MTL_SIMULATE_PACKET_DROPS to be enabled in the build
 * (debug/debugonly), so the TX side actually drops the configured packets.
 * The packet skip feature affects the critical path performance and is
 * compiled out of release builds. Without it no packets are dropped, so the
 * test would pass vacuously and mask regressions — skip instead of giving a
 * misleading green result.
 */
#ifndef MTL_SIMULATE_PACKET_DROPS
  GTEST_SKIP() << "requires MTL_SIMULATE_PACKET_DROPS (build with debugonly/debug); "
                  "without it no packets are dropped and the test cannot validate "
                  "per-port loss";
#else
  ctx->para.flags |= MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS;
  ctx->para.port_packet_loss[kPrimaryTxPort].tx_stream_loss_id =
      0; /* drop even packets */
  ctx->para.port_packet_loss[kPrimaryTxPort].tx_stream_loss_divider =
      2; /* out of every 2 packets */
  ctx->para.port_packet_loss[kRedundantTxPort].tx_stream_loss_id =
      1; /* drop odd packets */
  ctx->para.port_packet_loss[kRedundantTxPort].tx_stream_loss_divider =
      2; /* out of every 2 packets */
#endif

  initDefaultContext();

  /* Class a */
  uint testedLatencyMs = 10;
  uint testDurationS = 120; /* 2 minutes */
  constexpr uint16_t kUdpPortP = 20000;
  constexpr uint16_t kUdpPortR = 20001;
  constexpr uint32_t kTxFlags = ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_USER_TIMESTAMP;

  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pRedundantStreamPlan(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = kUdpPortP;
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = kUdpPortR;
        handler->setSessionPorts(SESSION_SKIP_PORT, kRxPortP, SESSION_SKIP_PORT,
                                 kRxPortR);
      });
  auto* rxStrategy = static_cast<St20pRedundantStreamPlan*>(rxBundle.strategy);

  auto primaryBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new St20pRedundantStreamPlan(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = kUdpPortP;
        handler->setSessionPorts(kPrimaryTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto* primaryStrategy = static_cast<St20pRedundantStreamPlan*>(primaryBundle.strategy);

  auto latencyBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [testedLatencyMs](St20pHandler* handler) {
        return new St20pRedundantStreamPlan(testedLatencyMs, handler);
      },
      [this, testedLatencyMs](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = kUdpPortR;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(kRedundantTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
      });

  /* we are creating 3 handlers
    - rxBundle receives both primary and redundant streams
    - primaryBundle sends the primary stream (no modifications)
    - latencyBundle sends the redundant stream delayed by testedLatencyMs

    [primaryBundle]:          Tx ---> Rx [latencyBundle]
    [latencyBundle]:          Tx ---> Rx [latencyBundle]
  */

  rxBundle.handler->startSessionRx();
  ASSERT_TRUE(waitForSession(rxBundle.handler->session));
  primaryBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(primaryBundle.handler->session));
  latencyBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));
  StartFakePtpClock(); /* reset ptp time to 0 */
  mtl_start(ctx->handle);

  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));
  sleepUntilFailure(testDurationS);

  /* Grab stats before stopping — captures steady-state counters before
   * session teardown introduces incomplete-frame artifacts. */
  st20_rx_user_stats stats;
  st20p_rx_get_session_stats(rxBundle.handler->sessionsHandleRx, &stats);

  latencyBundle.handler->session.stop();
  primaryBundle.handler->session.stop();
  rxBundle.handler->session.stop();

  uint64_t framesSend = primaryStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";

  /* The test is skipped above unless MTL_SIMULATE_PACKET_DROPS is compiled in,
   * so by here the even/odd drops are active and per-port loss must be
   * reported. With 50% simulated drops (divider=2) each port misses half the
   * packets, and the other port recovers them. */
  uint64_t lost_p = stats.common.port[MTL_SESSION_PORT_P].lost_packets;
  uint64_t lost_r = stats.common.port[MTL_SESSION_PORT_R].lost_packets;
  uint64_t pkts_p = stats.common.port[MTL_SESSION_PORT_P].packets;
  uint64_t pkts_r = stats.common.port[MTL_SESSION_PORT_R].packets;

  EXPECT_GT(lost_p, 0u) << "P must report per-port loss with 50% drops";
  EXPECT_GT(lost_r, 0u) << "R must report per-port loss with 50% drops";
  EXPECT_EQ(stats.common.stat_lost_packets, lost_p + lost_r)
      << "session lost == sum of per-port lost";

  /* Each port should lose ~50% of the merged stream's packets */
  if (pkts_p + lost_p > 0) {
    double pct_p = 100.0 * lost_p / (pkts_p + lost_p);
    EXPECT_NEAR(pct_p, 50.0, 5.0) << "P loss percentage should be ~50%";
  }
  if (pkts_r + lost_r > 0) {
    double pct_r = 100.0 * lost_r / (pkts_r + lost_r);
    EXPECT_NEAR(pct_r, 50.0, 5.0) << "R loss percentage should be ~50%";
  }

  /* With the TX-first stop and drain delay, there should be no
   * incomplete frames from teardown. All per-port losses should have
   * been recovered by the other port. */
  EXPECT_EQ(stats.stat_frames_incomplete, 0u)
      << "no incomplete frames expected after TX-first stop with drain";
  EXPECT_EQ(stats.common.stat_pkts_unrecovered, 0u)
      << "no post-redundancy loss expected with complementary 50/50 drops";
}
