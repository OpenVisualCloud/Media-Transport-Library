/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

struct RxDescCase {
  const char* name;
  bool iavf;
  bool hw_timestamp;
  uint16_t nb_rx_desc;
  uint16_t nb_max;
  int expected;
};

class MtDevRxDescTest : public testing::TestWithParam<RxDescCase> {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

/* iavf vector RX, chosen for a power-of-2 ring, ignores the RX timestamp valid bit. */
TEST_P(MtDevRxDescTest, RingSizeKeepsIavfOffVectorRxWithHwTimestamps) {
  const RxDescCase& c = GetParam();
  EXPECT_EQ(
      ut_dev_config_port_nb_rx_desc(ctx_, c.iavf, c.hw_timestamp, c.nb_rx_desc, c.nb_max),
      c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, MtDevRxDescTest,
    testing::Values(
        RxDescCase{"IavfTimestampPowerOf2Raised", true, true, 2048, 4096, 2080},
        RxDescCase{"IavfTimestampAtMaxLowered", true, true, 8192, 4096, 4064},
        RxDescCase{"IavfTimestampNonPowerOf2Kept", true, true, 2080, 4096, 2080},
        RxDescCase{"IavfNoTimestampKept", true, false, 2048, 4096, 2048},
        RxDescCase{"IceTimestampKept", false, true, 2048, 4096, 2048}),
    [](const testing::TestParamInfo<RxDescCase>& info) { return info.param.name; });

}  // namespace
