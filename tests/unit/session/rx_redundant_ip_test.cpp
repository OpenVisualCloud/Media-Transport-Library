/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The IP rules every RX validator applies to a redundant (two-port) session, at
 * create (*_ops_check) and at update_source (st_rx_source_info_check).
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <tuple>

#include "session/st20_harness.h"
#include "session/st30_harness.h"
#include "session/st40_harness.h"

extern "C" int st_rx_source_info_check(struct st_rx_source_info* src, int num_ports);

namespace {

struct Validator {
  const char* name;
  int (*check)(const struct st_rx_source_info* src);
};

struct IpCase {
  const char* name;
  uint8_t ip[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  int expect;
};

/* st_rx_source_info_check() takes a non-const src; copy to keep the Validator type */
int update_source_check(const struct st_rx_source_info* src) {
  struct st_rx_source_info copy = *src;
  return st_rx_source_info_check(&copy, MTL_SESSION_PORT_MAX);
}

class RxRedundantIpTest : public testing::TestWithParam<std::tuple<Validator, IpCase>> {};

TEST_P(RxRedundantIpTest, Check) {
  const Validator& v = std::get<0>(GetParam());
  const IpCase& c = std::get<1>(GetParam());
  struct st_rx_source_info src;

  memset(&src, 0, sizeof(src));
  memcpy(src.ip_addr, c.ip, sizeof(src.ip_addr));
  EXPECT_EQ(v.check(&src), c.expect);
}

INSTANTIATE_TEST_SUITE_P(
    Rules, RxRedundantIpTest,
    testing::Combine(
        testing::Values(Validator{"St20", ut20_ops_check_source},
                        Validator{"St22", ut20_st22_ops_check_source},
                        Validator{"St30", ut30_ops_check_source},
                        Validator{"St40", ut40_ops_check_source},
                        Validator{"UpdateSource", update_source_check}),
        testing::Values(IpCase{"DistinctGroups", {{239, 1, 1, 1}, {239, 1, 1, 2}}, 0},
                        /* each port joins its own (S, G) */
                        IpCase{"SameGroup", {{239, 1, 1, 1}, {239, 1, 1, 1}}, 0},
                        IpCase{"DistinctUnicast", {{10, 0, 0, 1}, {10, 0, 0, 2}}, 0},
                        IpCase{"SameUnicast", {{10, 0, 0, 1}, {10, 0, 0, 1}}, -EINVAL},
                        IpCase{"ZeroR", {{239, 1, 1, 1}, {0, 0, 0, 0}}, -EINVAL})),
    [](const testing::TestParamInfo<std::tuple<Validator, IpCase>>& info) {
      return std::string(std::get<0>(info.param).name) + std::get<1>(info.param).name;
    });

}  // namespace
