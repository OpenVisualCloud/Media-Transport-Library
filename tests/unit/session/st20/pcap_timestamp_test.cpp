/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * An RX video pcapng dump on a port without NIC RX timestamps stamps each packet
 * with the time of its RX burst, which no ST 2110-21 analysis can use: it says so
 * when the dump starts, and stays quiet on a port that has them.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxPcapTimestampTest.*'
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <string>

#include "session/st20/st20_rx_test_base.h"
#include "session/stderr_capture.h"

namespace {
const std::string kNoNicTimestamps = "no NIC RX timestamps";
}  // namespace

class St20RxPcapTimestampTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  std::string StartAndStop() {
    char name[MTL_PCAP_FILE_MAX_LEN] = "";
    int ret = -1;
    std::string log = ut_session::capture_stderr(
        [&] { ret = ut20_start_pcap(ctx_, 100, name, sizeof(name)); });
    EXPECT_EQ(0, ret);
    if (!ret) {
      ut20_stop_pcap(ctx_);
      unlink(name);
    }
    return log;
  }
};

TEST_F(St20RxPcapTimestampTest, WarnsWithoutNicTimestamps) {
  EXPECT_NE(StartAndStop().find(kNoNicTimestamps), std::string::npos);
}

TEST_F(St20RxPcapTimestampTest, QuietWithNicTimestamps) {
  ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
  EXPECT_EQ(StartAndStop().find(kNoNicTimestamps), std::string::npos);
}
