/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins what the kernel-socket RX backend writes into the headers of the packet it
 * builds from a datagram: the source the kernel reports through recvfrom(), which
 * the RX sessions match against their configured source.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDpSocketRxTest.*'
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <cerrno>

#include "datapath/mt_dp_socket_harness.h"

TEST(MtDpSocketRxTest, PacketCarriesTheDatagramSource) {
  uint16_t sender_port = 0, src_port = 0;
  uint32_t src_addr = 0;

  int ret = ut_dps_rx_source(&sender_port, &src_port, &src_addr);
  if (ret == -ENODEV) GTEST_SKIP() << "host refuses a loopback UDP socket";
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(src_port, sender_port);
  EXPECT_EQ(src_addr, INADDR_LOOPBACK);
}
