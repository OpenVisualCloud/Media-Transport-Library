/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <climits>
#include <cstddef>
#include <cstring>

#include "ffmpeg/ffmpeg_test_compat.h"
#define MTL_FFMPEG_UNIT_TEST
#include "../../../ecosystem/ffmpeg_plugin/mtl_common.h"
#include "ffmpeg/mtl_common_harness.h"

struct FfmpegOptionContext {
  StDevArgs devArgs;
};

#define OFFSET(field) offsetof(FfmpegOptionContext, field)
#define ENC 1
static const AVOption kTxOptions[] = {MTL_TX_DEV_ARGS};
#undef ENC
#undef OFFSET

class FfmpegMtlCommonTest : public testing::Test {
 protected:
  void SetUp() override {
    ut_ffmpeg_reset();
  }
};

TEST_F(FfmpegMtlCommonTest, SparsePortsCompactWithPairedAddressesAndQueues) {
  StDevArgs args = {};
  args.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
  args.sip[MTL_PORT_P] = const_cast<char*>("192.0.2.1");
  args.tx_queues_cnt[MTL_PORT_P] = 11;
  args.rx_queues_cnt[MTL_PORT_P] = 12;
  args.port[MTL_PORT_4] = const_cast<char*>("0000:04:00.0");
  args.sip[MTL_PORT_4] = const_cast<char*>("198.51.100.4");
  args.tx_queues_cnt[MTL_PORT_4] = 41;
  args.rx_queues_cnt[MTL_PORT_4] = 42;
  args.port[MTL_PORT_7] = const_cast<char*>("0000:07:00.0");
  args.sip[MTL_PORT_7] = const_cast<char*>("203.0.113.7");
  args.tx_queues_cnt[MTL_PORT_7] = 71;
  args.rx_queues_cnt[MTL_PORT_7] = 72;

  int idx = -1;
  ASSERT_NE(ut_ffmpeg_get(&args, &idx), nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  ASSERT_EQ(params->num_ports, 3);
  EXPECT_STREQ(params->port[0], "0000:01:00.0");
  EXPECT_STREQ(params->port[1], "0000:04:00.0");
  EXPECT_STREQ(params->port[2], "0000:07:00.0");
  EXPECT_EQ(params->tx_queues_cnt[0], 11);
  EXPECT_EQ(params->rx_queues_cnt[0], 12);
  EXPECT_EQ(params->tx_queues_cnt[1], 41);
  EXPECT_EQ(params->rx_queues_cnt[1], 42);
  EXPECT_EQ(params->tx_queues_cnt[2], 71);
  EXPECT_EQ(params->rx_queues_cnt[2], 72);
  EXPECT_EQ(params->sip_addr[1][0], 198);
  EXPECT_EQ(params->sip_addr[1][3], 4);
}

TEST_F(FfmpegMtlCommonTest, AllEightSourceIndexesReachMtlInit) {
  StDevArgs args = {};
  char ports[MTL_PORT_MAX][16];
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    snprintf(ports[i], sizeof(ports[i]), "0000:%02x:00.0", i);
    args.port[i] = ports[i];
    args.tx_queues_cnt[i] = 100 + i;
    args.rx_queues_cnt[i] = 200 + i;
  }

  int idx = -1;
  ASSERT_NE(ut_ffmpeg_get(&args, &idx), nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  ASSERT_EQ(params->num_ports, MTL_PORT_MAX);
  EXPECT_STREQ(params->port[MTL_PORT_7], ports[MTL_PORT_7]);
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_7], 100 + MTL_PORT_7);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_7], 200 + MTL_PORT_7);
}

TEST_F(FfmpegMtlCommonTest, P2ThroughP7OptionsTargetMatchingSourceIndexes) {
  for (int source = MTL_PORT_2; source <= MTL_PORT_7; source++) {
    char option_name[32];
    const AVOption* port_option = nullptr;
    snprintf(option_name, sizeof(option_name), "p%d_port", source);
    for (const AVOption& option : kTxOptions) {
      if (!strcmp(option.name, option_name)) port_option = &option;
    }
    ASSERT_NE(port_option, nullptr) << option_name;
    EXPECT_EQ(port_option->offset,
              static_cast<int>(offsetof(FfmpegOptionContext, devArgs.port) +
                               source * sizeof(char*)));

    snprintf(option_name, sizeof(option_name), "p%d_sip", source);
    const AVOption* sip_option = nullptr;
    for (const AVOption& option : kTxOptions) {
      if (!strcmp(option.name, option_name)) sip_option = &option;
    }
    ASSERT_NE(sip_option, nullptr) << option_name;
    EXPECT_EQ(sip_option->offset,
              static_cast<int>(offsetof(FfmpegOptionContext, devArgs.sip) +
                               source * sizeof(char*)));

    snprintf(option_name, sizeof(option_name), "p%d_tx_queues", source);
    const AVOption* tx_option = nullptr;
    for (const AVOption& option : kTxOptions) {
      if (!strcmp(option.name, option_name)) tx_option = &option;
    }
    ASSERT_NE(tx_option, nullptr) << option_name;
    EXPECT_EQ(tx_option->offset,
              static_cast<int>(offsetof(FfmpegOptionContext, devArgs.tx_queues_cnt) +
                               source * sizeof(int)));

    snprintf(option_name, sizeof(option_name), "p%d_rx_queues", source);
    const AVOption* rx_option = nullptr;
    for (const AVOption& option : kTxOptions) {
      if (!strcmp(option.name, option_name)) rx_option = &option;
    }
    ASSERT_NE(rx_option, nullptr) << option_name;
    EXPECT_EQ(rx_option->offset,
              static_cast<int>(offsetof(FfmpegOptionContext, devArgs.rx_queues_cnt) +
                               source * sizeof(int)));
  }
}

TEST_F(FfmpegMtlCommonTest, PrimaryAndRedundantPortsPreservePairing) {
  StDevArgs args = {};
  args.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
  args.sip[MTL_PORT_P] = const_cast<char*>("192.0.2.1");
  args.tx_queues_cnt[MTL_PORT_P] = 11;
  args.rx_queues_cnt[MTL_PORT_P] = 12;
  args.port[MTL_PORT_R] = const_cast<char*>("0000:02:00.0");
  args.sip[MTL_PORT_R] = const_cast<char*>("198.51.100.2");
  args.tx_queues_cnt[MTL_PORT_R] = 21;
  args.rx_queues_cnt[MTL_PORT_R] = 22;

  int idx = -1;
  ASSERT_NE(ut_ffmpeg_get(&args, &idx), nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  ASSERT_EQ(params->num_ports, 2);
  EXPECT_STREQ(params->port[MTL_PORT_P], "0000:01:00.0");
  EXPECT_STREQ(params->port[MTL_PORT_R], "0000:02:00.0");
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_P], 11);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_P], 12);
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_R], 21);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_R], 22);
}

TEST_F(FfmpegMtlCommonTest, InvalidSparseIpRejectsBeforeMtlInit) {
  StDevArgs args = {};
  args.port[MTL_PORT_6] = const_cast<char*>("0000:06:00.0");
  args.sip[MTL_PORT_6] = const_cast<char*>("999.2.3.4");

  int idx = -1;
  EXPECT_EQ(ut_ffmpeg_get(&args, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 0);
}

TEST_F(FfmpegMtlCommonTest, PtpOptionsMapToInitFlagsAndPacing) {
  StDevArgs args = {};
  args.ptp_enable = 1;
  args.ptp_pi = 1;
  args.ptp_unicast = 1;

  int idx = -1;
  ASSERT_NE(ut_ffmpeg_get(&args, &idx), nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_ENABLE);
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_PI);
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_UNICAST_ADDR);
  EXPECT_EQ(params->pacing, ST21_TX_PACING_WAY_PTP);
  EXPECT_EQ(params->priv, nullptr);
  EXPECT_EQ(params->ptp_sync_notify, nullptr);
}

TEST_F(FfmpegMtlCommonTest, PtpDependenciesRejectBeforeMtlInit) {
  for (int pi = 0; pi <= 1; pi++) {
    for (int unicast = 0; unicast <= 1; unicast++) {
      if (!pi && !unicast) continue;
      StDevArgs args = {};
      args.ptp_pi = pi;
      args.ptp_unicast = unicast;
      int idx = -1;
      EXPECT_EQ(ut_ffmpeg_get(&args, &idx), nullptr);
    }
  }
  EXPECT_EQ(ut_ffmpeg_init_calls(), 0);
}

TEST_F(FfmpegMtlCommonTest,
       SharedHandleRejectsUnsupportedLateRequestWithoutRefcountChange) {
  StDevArgs plain = {};
  int first_idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&plain, &first_idx);
  ASSERT_NE(handle, nullptr);

  StDevArgs late_ptp = {};
  late_ptp.ptp_enable = 1;
  int rejected_idx = -1;
  EXPECT_EQ(ut_ffmpeg_get(&late_ptp, &rejected_idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleAcceptsInitiallyEnabledCapabilities) {
  StDevArgs args = {};
  args.ptp_enable = 1;
  args.ptp_pi = 1;
  args.ptp_unicast = 1;
  int first_idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&args, &first_idx);
  ASSERT_NE(handle, nullptr);

  int second_idx = -1;
  EXPECT_EQ(ut_ffmpeg_get(&args, &second_idx), handle);
  EXPECT_EQ(first_idx, 0);
  EXPECT_EQ(second_idx, 1);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 1);
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, LastPutResetsCapabilitiesForNextHandle) {
  StDevArgs ptp = {};
  ptp.ptp_enable = 1;
  ptp.ptp_pi = 1;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&ptp, &idx);
  ASSERT_NE(handle, nullptr);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);

  StDevArgs plain = {};
  ASSERT_NE(ut_ffmpeg_get(&plain, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_get(&ptp, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 2);
}