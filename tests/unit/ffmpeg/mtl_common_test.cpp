/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <climits>
#include <cstddef>
#include <cstring>
#include <thread>

#include "ffmpeg/mtl_common_harness.h"
#define MTL_FFMPEG_UNIT_TEST
#include "../../../ecosystem/ffmpeg_plugin/mtl_common.h"

struct FfmpegOptionContext {
  StDevArgs devArgs;
};

#define OFFSET(field) offsetof(FfmpegOptionContext, field)
#define ENC 1
static const AVOption kTxOptions[] = {MTL_TX_DEV_ARGS};
#undef ENC
#undef OFFSET

#define OFFSET(field) offsetof(FfmpegOptionContext, field)
#define DEC 1
static const AVOption kRxOptions[] = {MTL_RX_DEV_ARGS};
#undef DEC
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
  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  ASSERT_NE(handle, nullptr);
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
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
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
  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  ASSERT_NE(handle, nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  ASSERT_EQ(params->num_ports, MTL_PORT_MAX);
  EXPECT_STREQ(params->port[MTL_PORT_7], ports[MTL_PORT_7]);
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_7], 100 + MTL_PORT_7);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_7], 200 + MTL_PORT_7);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
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
  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  ASSERT_NE(handle, nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  ASSERT_EQ(params->num_ports, 2);
  EXPECT_STREQ(params->port[MTL_PORT_P], "0000:01:00.0");
  EXPECT_STREQ(params->port[MTL_PORT_R], "0000:02:00.0");
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_P], 11);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_P], 12);
  EXPECT_EQ(params->tx_queues_cnt[MTL_PORT_R], 21);
  EXPECT_EQ(params->rx_queues_cnt[MTL_PORT_R], 22);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, InvalidSparseIpRejectsBeforeMtlInit) {
  StDevArgs args = {};
  args.port[MTL_PORT_6] = const_cast<char*>("0000:06:00.0");
  args.sip[MTL_PORT_6] = const_cast<char*>("999.2.3.4");

  int idx = -1;
  EXPECT_EQ(ut_ffmpeg_get(&args, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 0);
}

TEST_F(FfmpegMtlCommonTest, PtpEnableLeavesDefaultPacing) {
  StDevArgs args = {};
  args.ptp_enable = 1;
  args.ptp_pi = 1;
  args.ptp_unicast = 1;

  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  ASSERT_NE(handle, nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_ENABLE);
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_PI);
  EXPECT_TRUE(params->flags & MTL_FLAG_PTP_UNICAST_ADDR);
  EXPECT_EQ(params->pacing, ST21_TX_PACING_WAY_AUTO);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, ExplicitPtpPacingSelectsPtpPacingWithoutClockClient) {
  StDevArgs args = {};
  args.ptp_pacing = 1;

  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  ASSERT_NE(handle, nullptr);
  const mtl_init_params* params = ut_ffmpeg_last_init_params();
  EXPECT_FALSE(params->flags & MTL_FLAG_PTP_ENABLE);
  EXPECT_EQ(params->pacing, ST21_TX_PACING_WAY_PTP);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, PtpPacingOptionTargetsBothDeviceDirections) {
  for (const AVOption* options : {kTxOptions, kRxOptions}) {
    size_t count = options == kTxOptions ? std::size(kTxOptions) : std::size(kRxOptions);
    const AVOption* pacing_option = nullptr;
    for (size_t i = 0; i < count; i++) {
      if (!strcmp(options[i].name, "ptp_pacing")) pacing_option = &options[i];
    }
    ASSERT_NE(pacing_option, nullptr);
    EXPECT_EQ(pacing_option->offset,
              static_cast<int>(offsetof(FfmpegOptionContext, devArgs.ptp_pacing)));
  }
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

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsDifferentPtpPacing) {
  StDevArgs plain = {};
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&plain, &idx);
  ASSERT_NE(handle, nullptr);

  StDevArgs paced = {};
  paced.ptp_pacing = 1;
  EXPECT_EQ(ut_ffmpeg_get(&paced, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsDifferentPortConfiguration) {
  StDevArgs first = {};
  first.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
  StDevArgs second = first;
  second.port[MTL_PORT_P] = const_cast<char*>("0000:02:00.0");
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    EXPECT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsDifferentSipConfiguration) {
  StDevArgs first = {};
  first.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
  first.sip[MTL_PORT_P] = const_cast<char*>("192.0.2.1");
  StDevArgs second = first;
  second.sip[MTL_PORT_P] = const_cast<char*>("192.0.2.2");
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    EXPECT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsDifferentQueueConfiguration) {
  StDevArgs first = {};
  first.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
  first.tx_queues_cnt[MTL_PORT_P] = 16;
  first.rx_queues_cnt[MTL_PORT_P] = 16;
  StDevArgs second = first;
  second.tx_queues_cnt[MTL_PORT_P] = 17;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    EXPECT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsDifferentDmaConfiguration) {
  StDevArgs first = {};
  first.dma_dev = const_cast<char*>("0000:80:04.0");
  StDevArgs second = first;
  second.dma_dev = const_cast<char*>("0000:80:04.1");
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    EXPECT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleAcceptsActivePtpForNonPtpRequester) {
  StDevArgs first = {};
  first.ptp_enable = 1;
  first.ptp_pi = 1;
  first.ptp_unicast = 1;
  StDevArgs second = {};
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle shared = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(shared, handle);
  if (shared) {
    EXPECT_EQ(ut_ffmpeg_put(shared), 0);
  }
  EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsPtpPiDisabledToEnabled) {
  StDevArgs first = {};
  first.ptp_enable = 1;
  StDevArgs second = first;
  second.ptp_pi = 1;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    ASSERT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsPtpPiEnabledToDisabled) {
  StDevArgs first = {};
  first.ptp_enable = 1;
  first.ptp_pi = 1;
  StDevArgs second = first;
  second.ptp_pi = 0;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    ASSERT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsPtpUnicastDisabledToEnabled) {
  StDevArgs first = {};
  first.ptp_enable = 1;
  StDevArgs second = first;
  second.ptp_unicast = 1;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    ASSERT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, SharedHandleRejectsPtpUnicastEnabledToDisabled) {
  StDevArgs first = {};
  first.ptp_enable = 1;
  first.ptp_unicast = 1;
  StDevArgs second = first;
  second.ptp_unicast = 0;
  int idx = -1;
  mtl_handle handle = ut_ffmpeg_get(&first, &idx);
  ASSERT_NE(handle, nullptr);

  mtl_handle rejected = ut_ffmpeg_get(&second, &idx);
  EXPECT_EQ(rejected, nullptr);
  if (rejected) {
    ASSERT_EQ(ut_ffmpeg_put(rejected), 0);
  }
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}

TEST_F(FfmpegMtlCommonTest, ConcurrentGetsCreateOneSharedHandle) {
  StDevArgs args = {};
  int first_idx = -1;
  int second_idx = -1;
  mtl_handle first = nullptr;
  mtl_handle second = nullptr;
  ut_ffmpeg_block_first_init();

  std::thread first_thread([&] { first = ut_ffmpeg_get(&args, &first_idx); });
  bool first_started = ut_ffmpeg_wait_for_init_calls(1);
  std::thread second_thread([&] { second = ut_ffmpeg_get(&args, &second_idx); });
  bool second_entered = ut_ffmpeg_wait_for_lifecycle_lock_calls(2);
  ut_ffmpeg_release_init();
  first_thread.join();
  second_thread.join();

  ASSERT_TRUE(first_started);
  ASSERT_TRUE(second_entered);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(second, first);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 1);
  EXPECT_EQ(ut_ffmpeg_put(first), 0);
  EXPECT_EQ(ut_ffmpeg_put(second), 0);
  EXPECT_EQ(ut_ffmpeg_uninit_calls(), 1);
}

TEST_F(FfmpegMtlCommonTest, OverlongPortNameRejectsBeforePmdDetection) {
  char port[MTL_PORT_MAX_LEN + 1];
  memset(port, 'a', sizeof(port) - 1);
  port[sizeof(port) - 1] = '\0';
  StDevArgs args = {};
  args.port[MTL_PORT_P] = port;
  int idx = -1;

  mtl_handle handle = ut_ffmpeg_get(&args, &idx);
  EXPECT_EQ(handle, nullptr);
  if (handle) {
    EXPECT_EQ(ut_ffmpeg_put(handle), 0);
  }
  EXPECT_EQ(ut_ffmpeg_init_calls(), 0);
}

TEST_F(FfmpegMtlCommonTest, QueueOptionsUseInitParameterRange) {
  for (const AVOption* options : {kTxOptions, kRxOptions}) {
    size_t count = options == kTxOptions ? std::size(kTxOptions) : std::size(kRxOptions);
    for (size_t i = 0; i < count; i++) {
      if (!strstr(options[i].name, "queues")) continue;
      EXPECT_EQ(options[i].min, 0) << options[i].name;
      EXPECT_EQ(options[i].max, UINT16_MAX) << options[i].name;
    }
  }
}

TEST_F(FfmpegMtlCommonTest, InvalidQueueCountRejectsBeforeMtlInit) {
  for (int count : {-1, static_cast<int>(UINT16_MAX) + 1}) {
    ut_ffmpeg_reset();
    StDevArgs args = {};
    args.port[MTL_PORT_P] = const_cast<char*>("0000:01:00.0");
    args.tx_queues_cnt[MTL_PORT_P] = count;
    int idx = -1;

    mtl_handle handle = ut_ffmpeg_get(&args, &idx);
    EXPECT_EQ(handle, nullptr) << count;
    if (handle) {
      EXPECT_EQ(ut_ffmpeg_put(handle), 0);
    }
    EXPECT_EQ(ut_ffmpeg_init_calls(), 0) << count;
  }
}

TEST_F(FfmpegMtlCommonTest, EmptyDmaDeviceEntryRejectsBeforeMtlInit) {
  for (const char* devices : {"", ",0000:80:04.0", "0000:80:04.0,", "0000:80:04.0,,x"}) {
    ut_ffmpeg_reset();
    StDevArgs args = {};
    args.dma_dev = const_cast<char*>(devices);
    int idx = -1;

    EXPECT_EQ(ut_ffmpeg_get(&args, &idx), nullptr) << devices;
    EXPECT_EQ(ut_ffmpeg_init_calls(), 0) << devices;
  }
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
  handle = ut_ffmpeg_get(&plain, &idx);
  ASSERT_NE(handle, nullptr);
  EXPECT_EQ(ut_ffmpeg_get(&ptp, &idx), nullptr);
  EXPECT_EQ(ut_ffmpeg_init_calls(), 2);
  ASSERT_EQ(ut_ffmpeg_put(handle), 0);
}