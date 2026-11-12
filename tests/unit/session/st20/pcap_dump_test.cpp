/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * An RX video pcapng dump creates its file whatever the packet count asked for:
 * the file name used to carry the count, which at six digits overflowed
 * MTL_PCAP_FILE_MAX_LEN and made mkstemps() fail.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20RxPcapDumpTest*'
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxPcapDumpTest : public St20RxBaseTest,
                           public ::testing::WithParamInterface<uint32_t> {
 protected:
  int num_port() const override {
    return 1;
  }
};

TEST_P(St20RxPcapDumpTest, CreatesItsFile) {
  char name[MTL_PCAP_FILE_MAX_LEN] = "";
  struct stat st;

  ASSERT_EQ(0, ut20_start_pcap(ctx_, GetParam(), name, sizeof(name)));
  EXPECT_EQ(0, stat(name, &st)) << name;
  ut20_stop_pcap(ctx_);
  unlink(name);
}

INSTANTIATE_TEST_SUITE_P(Packets, St20RxPcapDumpTest,
                         ::testing::Values(100u, 99999u, 100000u, 4294967295u));
