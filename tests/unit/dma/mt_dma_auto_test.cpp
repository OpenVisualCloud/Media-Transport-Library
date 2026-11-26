/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "dma/mt_dma_auto_harness.h"
#include "mtl_api.h"

class MtDmaAutoProbeTest : public ::testing::Test {
 protected:
  static constexpr uint16_t kIntel = 0x8086;
  static constexpr uint16_t kSprDsa = 0x0b25;
  static constexpr const char* kVfio = "vfio-pci";

  void SetUp() override {
    ctx_ = ut_dma_auto_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dma_auto_destroy(ctx_);
  }

  static std::string Bdf(int node, int dev) {
    char bdf[32];
    (void)snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.0", 0x10 * (node + 1), dev);
    return bdf;
  }

  void AddDev(const std::string& bdf, uint16_t vendor, uint16_t device,
              const char* driver, int numa_node) {
    ASSERT_EQ(ut_dma_auto_add_dev(ctx_, bdf.c_str(), vendor, device, driver, numa_node),
              0);
  }

  void AddDsas(int node, int nb) {
    for (int i = 0; i < nb; i++) AddDev(Bdf(node, i), kIntel, kSprDsa, kVfio, node);
  }

  void Probe() {
    ASSERT_EQ(ut_dma_auto_init(ctx_, true), 0);
  }

  int ProbesOnNode(int node) {
    std::string bus = Bdf(node, 0).substr(0, 8);
    int nb = 0;
    for (int i = 0; i < ut_dma_auto_nb_probes(ctx_); i++) {
      if (std::string(ut_dma_auto_probe_bdf(ctx_, i)).compare(0, 8, bus) == 0) nb++;
    }
    return nb;
  }

  ut_dma_auto_ctx* ctx_ = nullptr;
};

TEST_F(MtDmaAutoProbeTest, ProbesAnIntelDmaDeviceOnVfioPciOnThePortNode) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, kSprDsa, kVfio, 0);

  Probe();

  ASSERT_EQ(ut_dma_auto_nb_probes(ctx_), 1);
  EXPECT_EQ(std::string(ut_dma_auto_probe_bdf(ctx_, 0)), Bdf(0, 0));
}

TEST_F(MtDmaAutoProbeTest, NoProbeWithoutTheFlag) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, kSprDsa, kVfio, 0);

  ASSERT_EQ(ut_dma_auto_init(ctx_, false), 0);

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, SkipsAListedDeviceIdOfAnotherVendor) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), 0x8087, kSprDsa, kVfio, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, SkipsAnIntelDeviceIdThatIsNotListed) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, 0x1592, kVfio, 0); /* E810-C NIC */

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, SkipsADeviceOnAnotherDriver) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, kSprDsa, "idxd", 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, SkipsADeviceWithNoDriver) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, kSprDsa, nullptr, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, SkipsADeviceOnANodeWithNoPort) {
  ut_dma_auto_add_port(ctx_, 1);
  AddDev(Bdf(0, 0), kIntel, kSprDsa, kVfio, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 0);
}

TEST_F(MtDmaAutoProbeTest, ProbesTheDevicesOfEachPortNode) {
  ut_dma_auto_add_port(ctx_, 0);
  ut_dma_auto_add_port(ctx_, 1);
  AddDsas(0, 1);
  AddDsas(1, 1);
  AddDsas(2, 1);

  Probe();

  EXPECT_EQ(ProbesOnNode(0), 1);
  EXPECT_EQ(ProbesOnNode(1), 1);
  EXPECT_EQ(ProbesOnNode(2), 0);
}

TEST_F(MtDmaAutoProbeTest, OneNodeGetsAtMostTheMaxDmaDevices) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, MTL_DMA_DEV_MAX + 1);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), MTL_DMA_DEV_MAX);
}

TEST_F(MtDmaAutoProbeTest, TwoNodesGetHalfTheMaxDmaDevicesEach) {
  ut_dma_auto_add_port(ctx_, 0);
  ut_dma_auto_add_port(ctx_, 1);
  AddDsas(0, MTL_DMA_DEV_MAX / 2 + 1);
  AddDsas(1, MTL_DMA_DEV_MAX / 2 + 1);

  Probe();

  EXPECT_EQ(ProbesOnNode(0), MTL_DMA_DEV_MAX / 2);
  EXPECT_EQ(ProbesOnNode(1), MTL_DMA_DEV_MAX / 2);
}

TEST_F(MtDmaAutoProbeTest, TwoPortsOnOneNodeShareOneMax) {
  ut_dma_auto_add_port(ctx_, 0);
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, MTL_DMA_DEV_MAX + 1);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), MTL_DMA_DEV_MAX);
}

TEST_F(MtDmaAutoProbeTest, EachDmaDeviceOfAProbeCountsAgainstTheShare) {
  const int dsa_wqs = 16;
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, MTL_DMA_DEV_MAX / dsa_wqs + 1);
  for (int i = 0; i <= MTL_DMA_DEV_MAX / dsa_wqs; i++)
    ut_dma_auto_set_probe(ctx_, i, 0, dsa_wqs);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), MTL_DMA_DEV_MAX / dsa_wqs);
}

TEST_F(MtDmaAutoProbeTest, ABusyDeviceDoesNotEndTheScan) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, 2);
  ut_dma_auto_set_probe(ctx_, 0, -EBUSY, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 2);
}

TEST_F(MtDmaAutoProbeTest, ABusyDeviceUsesNoShare) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, MTL_DMA_DEV_MAX + 1);
  ut_dma_auto_set_probe(ctx_, 0, -EBUSY, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), MTL_DMA_DEV_MAX + 1);
}

TEST_F(MtDmaAutoProbeTest, AnAlreadyProbedDeviceDoesNotEndTheScan) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, 2);
  ut_dma_auto_set_probe(ctx_, 0, -EEXIST, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 2);
}

TEST_F(MtDmaAutoProbeTest, AnAlreadyProbedDeviceUsesNoShare) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDsas(0, MTL_DMA_DEV_MAX + 1);
  ut_dma_auto_set_probe(ctx_, 0, -EEXIST, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), MTL_DMA_DEV_MAX + 1);
}

class MtDmaAutoIdTest : public MtDmaAutoProbeTest,
                        public ::testing::WithParamInterface<uint16_t> {};

TEST_P(MtDmaAutoIdTest, ProbesAListedDeviceId) {
  ut_dma_auto_add_port(ctx_, 0);
  AddDev(Bdf(0, 0), kIntel, GetParam(), kVfio, 0);

  Probe();

  EXPECT_EQ(ut_dma_auto_nb_probes(ctx_), 1);
}

INSTANTIATE_TEST_SUITE_P(IoatAndIdxd, MtDmaAutoIdTest,
                         ::testing::Values(0x6f20, 0x6f21, 0x6f22, 0x6f23, 0x6f24, 0x6f25,
                                           0x6f26, 0x6f27, 0x6f2e, 0x6f2f, 0x2021, 0x0b00,
                                           0x0b25, 0x11fb, 0x1212));
