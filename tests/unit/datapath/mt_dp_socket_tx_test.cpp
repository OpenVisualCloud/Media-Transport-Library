/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins where the kernel-socket TX backend sends GSO-sized packets: to the
 * destination in each packet's own header, which st*_tx_update_destination()
 * rewrites, not the flow the queue was created for.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDpSocketTxTest.*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "datapath/mt_dp_socket_harness.h"

TEST(MtDpSocketTxTest, GsoPacketsGoToTheirHeaderDestination) {
  constexpr int kPackets = 4;
  int flow_port_datagrams = -1, hdr_port_datagrams = -1;

  int sent = ut_dps_gso_burst(kPackets, &flow_port_datagrams, &hdr_port_datagrams);
  if (sent == -ENODEV) GTEST_SKIP() << "host refuses a loopback UDP socket bound to lo";
  ASSERT_EQ(sent, kPackets);
  EXPECT_EQ(hdr_port_datagrams, kPackets);
  EXPECT_EQ(flow_port_datagrams, 0);
}
