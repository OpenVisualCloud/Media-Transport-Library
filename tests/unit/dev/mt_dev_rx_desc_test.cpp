/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

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

struct RxDescL3Case {
  const char* name;
  uint64_t l3_size;
  uint32_t nb_rx_q;
  uint32_t port_rx_q;
  uint16_t expected;
};

/* stride of the ice/iavf session RX pool: data room, headroom, priv, rte_mbuf, header */
constexpr uint32_t kRxElemSize = 1984;

class MtDevRxDescL3Test : public testing::TestWithParam<RxDescL3Case> {};

TEST_P(MtDevRxDescL3Test, RingIsDeepestPow2WhoseMbufsFitL3ShareAboveThePortFloor) {
  const RxDescL3Case& c = GetParam();
  EXPECT_EQ(ut_dev_rx_desc_by_l3(c.l3_size, c.nb_rx_q, c.port_rx_q, kRxElemSize),
            c.expected);
}

/* measured best: 41 queues a 1080p port (36 sessions), 14 a 2160p one (9). The plugin
 * default of 16 queues a port, plus CNI, may carry one stream: floor 1024. */
INSTANTIATE_TEST_SUITE_P(
    Cases, MtDevRxDescL3Test,
    testing::Values(
        RxDescL3Case{"Xeon6767P1080pOn4Ports", 336ull << 20, 164, 41, 512},
        RxDescL3Case{"Xeon6767P2160pOn4Ports", 336ull << 20, 56, 14, 2048},
        RxDescL3Case{"Xeon8480Plus1080pOn2Ports", 105ull << 20, 82, 41, 512},
        RxDescL3Case{"PluginDefaultOn2PortsOf105MiB", 105ull << 20, 34, 17, 1024},
        RxDescL3Case{"PluginDefaultOn2PortsOf60MiB", 60ull << 20, 34, 17, 1024},
        RxDescL3Case{"DensePortOnSmallL3", 60ull << 20, 32, 32, 512},
        RxDescL3Case{"SparsePortOnSmallL3", 60ull << 20, 31, 31, 1024},
        RxDescL3Case{"UnknownL3", 0, 164, 41, 2048},
        RxDescL3Case{"TinyL3ClampedTo512", 1ull << 20, 164, 41, 512},
        RxDescL3Case{"LargeL3ClampedTo2048", 4ull << 30, 1, 1, 2048}),
    [](const testing::TestParamInfo<RxDescL3Case>& info) { return info.param.name; });

static std::string ReadSysfsLine(const std::string& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

/* the L3 counts only if shared by exactly the node's cpus */
TEST(MtL3SizeTest, Node0IsItsFirstCpuIndex3SizeIfItSharesThatL3ElseZero) {
  std::string node_cpus = ReadSysfsLine("/sys/devices/system/node/node0/cpulist");
  std::string index3 = "/sys/devices/system/cpu/cpu" +
                       std::to_string(std::atoi(node_cpus.c_str())) + "/cache/index3/";
  uint64_t expected = 0;
  if (!node_cpus.empty() && node_cpus == ReadSysfsLine(index3 + "shared_cpu_list"))
    expected = std::strtoull(ReadSysfsLine(index3 + "size").c_str(), nullptr, 10) << 10;
  EXPECT_EQ(ut_dev_host_l3_size(0), expected) << node_cpus;
}

struct RxElemCase {
  const char* driver;
  uint16_t rx_pool_data_size;
  bool mono_pool;
};

class MtDevRxElemSizeTest : public testing::TestWithParam<RxElemCase> {};

TEST_P(MtDevRxElemSizeTest, MatchesObjectSizeOfSessionRxPool) {
  ut_dev_ctx* ctx = ut_dev_create_ctx();
  ASSERT_NE(ctx, nullptr);
  ut_dev_set_driver(ctx, GetParam().driver);
  ut_dev_set_rx_pool_data_size(ctx, GetParam().rx_pool_data_size);
  if (GetParam().mono_pool) ut_dev_set_rx_mono_pool(ctx);
  EXPECT_EQ(ut_dev_rx_elem_size(ctx), ut_dev_rx_pool_obj_size(ctx));
  ut_dev_destroy_ctx(ctx);
}

INSTANTIATE_TEST_SUITE_P(Pools, MtDevRxElemSizeTest,
                         testing::Values(RxElemCase{"net_ice", 0, false},
                                         RxElemCase{"net_iavf", 0, false},
                                         RxElemCase{"net_ice", 3000, false},
                                         RxElemCase{"net_ice", 0, true}),
                         [](const testing::TestParamInfo<RxElemCase>& info) {
                           return std::string(info.param.driver + 4) +
                                  std::to_string(info.param.rx_pool_data_size) +
                                  (info.param.mono_pool ? "Mono" : "");
                         });

/* Ports P and R on socket 0, whose L3 is 192 MiB; port 2 on socket 1. Each DPDK user port
 * adds a CNI and a PTP RX queue. */
class MtDevRxDescDefaultL3Test : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_set_num_ports(ctx_, 3);
    ut_dev_set_rx_port(ctx_, MTL_PORT_P, 0, 32);
    ut_dev_set_rx_port(ctx_, MTL_PORT_R, 0, 32);
    ut_dev_set_rx_port(ctx_, MTL_PORT_2, 1, 256);
    ut_dev_set_l3_size(ctx_, 0, 192ull << 20);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevRxDescDefaultL3Test, SocketRxQueuesAreUserPlusSystemQueuesOfItsPorts) {
  EXPECT_EQ(ut_dev_socket_rx_queues(ctx_, 0), 68u);
}

TEST_F(MtDevRxDescDefaultL3Test, RxSessionsCntMaxReplacesRxQueuesCnt) {
  ut_dev_set_rx_sessions_cnt_max(ctx_, 10);
  EXPECT_EQ(ut_dev_socket_rx_queues(ctx_, 0), 24u);
}

TEST_F(MtDevRxDescDefaultL3Test, MaxRxQueuesCapsAPortOnceKnown) {
  ut_dev_set_max_rx_queues(ctx_, MTL_PORT_P, 8);
  EXPECT_EQ(ut_dev_socket_rx_queues(ctx_, 0), 42u);
}

TEST_F(MtDevRxDescDefaultL3Test, AfPacketPortHasOneRxQueue) {
  ut_dev_set_pmd(ctx_, MTL_PORT_R, MTL_PMD_DPDK_AF_PACKET);
  EXPECT_EQ(ut_dev_socket_rx_queues(ctx_, 0), 35u);
}

/* 68 queues share 144 MiB: 1119 mbufs of 1984 bytes; P alone gives 2048, socket 1 512 */
TEST_F(MtDevRxDescDefaultL3Test, IceRingFitsL3OverRxQueuesOfItsSocket) {
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 0, 4096), 1024);
}

/* 64 queues (62 + CNI + PTP) → 1008 → 512; 62 → 1040 → 1024 */
TEST_F(MtDevRxDescDefaultL3Test, SystemRxQueuesShareTheL3) {
  ut_dev_set_num_ports(ctx_, 1);
  ut_dev_set_rx_port(ctx_, MTL_PORT_P, 0, 62);
  ut_dev_use_non_igc_driver(ctx_);
  ut_dev_set_l3_size(ctx_, 0, 1024ull * 63 * 100 / 75 * ut_dev_rx_elem_size(ctx_));
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 0, 4096), 512);
}

/* P's 10 queues of 112 on the socket get 679 → 512; its own count keeps the 1024 floor */
TEST_F(MtDevRxDescDefaultL3Test, SparsePortKeeps1024OnABusySocket) {
  ut_dev_set_rx_port(ctx_, MTL_PORT_P, 0, 8);
  ut_dev_set_rx_port(ctx_, MTL_PORT_R, 0, 100);
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 0, 4096), 1024);
}

TEST_F(MtDevRxDescDefaultL3Test, UserNbRxDescWins) {
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 256, 4096), 256);
}

TEST_F(MtDevRxDescDefaultL3Test, UnknownL3Keeps2048) {
  ut_dev_set_l3_size(ctx_, 0, 0);
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 0, 4096), 2048);
}

TEST_F(MtDevRxDescDefaultL3Test, TaskletThreadKeeps2048) {
  ut_dev_set_tasklet_thread(ctx_);
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, false, false, 0, 4096), 2048);
}

TEST_F(MtDevRxDescDefaultL3Test, IavfTimestampWorkaroundAppliesToComputedRing) {
  EXPECT_EQ(ut_dev_config_port_nb_rx_desc(ctx_, true, true, 0, 4096), 1056);
}

struct RxDescDriverCase {
  const char* driver;
  int expected;
};

class MtDevRxDescDriverTest : public MtDevRxDescDefaultL3Test,
                              public testing::WithParamInterface<RxDescDriverCase> {};

/* the L3 fit is calibrated on ice and iavf only */
TEST_P(MtDevRxDescDriverTest, OnlyIceAndIavfFitTheRingToTheL3) {
  EXPECT_EQ(ut_dev_config_port_driver_nb_rx_desc(ctx_, GetParam().driver),
            GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Drivers, MtDevRxDescDriverTest,
    testing::Values(
        RxDescDriverCase{"net_ice", 1024}, RxDescDriverCase{"net_iavf", 1024},
        RxDescDriverCase{"net_igc", 2048}, RxDescDriverCase{"net_e1000_igb", 2048},
        RxDescDriverCase{"net_ixgbe", 2048}, RxDescDriverCase{"net_i40e", 2048},
        RxDescDriverCase{"mlx5_pci", 2048}, RxDescDriverCase{"net_ena", 2048},
        RxDescDriverCase{"net_af_xdp", 2048}, RxDescDriverCase{"net_af_packet", 2048},
        RxDescDriverCase{"kernel_socket", 2048}, RxDescDriverCase{"native_af_xdp", 2048}),
    [](const testing::TestParamInfo<RxDescDriverCase>& info) {
      return std::string(info.param.driver);
    });

}  // namespace
