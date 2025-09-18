/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "noctx.hpp"

void NoCtxTest::SetUp() {
  ctx = new struct st_tests_context;
  ASSERT_TRUE(ctx != nullptr);
  memcpy(ctx, st_test_ctx(), sizeof(*ctx));

  ctx->level = ST_TEST_LEVEL_MANDATORY;
  ctx->para.flags |= MTL_FLAG_RANDOM_SRC_PORT;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.priv = ctx;
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 16;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 16;

  /* NOCTX test: always operate on a copy of the global ctx.
     Do not use the global ctx directly for anything except copying its values.
   */
  ASSERT_TRUE(ctx != NULL);
  memcpy(ctx, st_test_ctx(), sizeof(*ctx));
}

void NoCtxTest::TearDown() {
  for (auto& t : tx_thread) {
    if (t.joinable()) t.join();
  }
  tx_thread.clear();

  for (auto& t : rx_thread) {
    if (t.joinable()) t.join();
  }
  rx_thread.clear();

  if (ctx) {
    if (ctx->handle) {
      mtl_uninit(ctx->handle);
      /* WA for reinitialization issues */
      sleep(10);
    }
    delete ctx;
    ctx = nullptr;
  }
}

/* create ptp time that will set the time to 0 */
uint64_t NoCtxTest::TestPtpSourceSinceEpoch(void* priv) {
  struct timespec spec;
  static uint64_t adjustment_ns = 0;

  if (!adjustment_ns) {
    struct timespec spec_adjustment_to_epoch;
    clock_gettime(CLOCK_REALTIME, &spec_adjustment_to_epoch);
    adjustment_ns = (uint64_t)spec_adjustment_to_epoch.tv_sec * NS_PER_S +
                    spec_adjustment_to_epoch.tv_nsec;
  }

  clock_gettime(CLOCK_MONOTONIC, &spec);
  return ((uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec) - adjustment_ns;
}

void NoCtxTestSt30p::SetUp() {
  NoCtxTest::SetUp();
  samplingModesDefault = {ST31_SAMPLING_44K, ST30_SAMPLING_96K, ST30_SAMPLING_48K};
  ptimeModesDefault = {ST31_PTIME_1_09MS, ST30_PTIME_1MS, ST30_PTIME_125US};
  channelCountsDefault = {3, 5, 7};
  fmtModesDefault = {ST31_FMT_AM824, ST30_FMT_PCM16, ST30_FMT_PCM24};
}

st30p_tx_ops NoCtxTestSt30p::CreateDefaultSt30pTxOps(int i, st30p_test_ctx* test_ctx_tx[],
                                                     st30_type type[],
                                                     st30_sampling sample[],
                                                     int channel[], st30_fmt fmt[],
                                                     st30_ptime ptime[]) {
  st30p_tx_ops ops_tx = {};
  ops_tx.name = "st30_test";
  ops_tx.priv = test_ctx_tx[i];
  ops_tx.num_port = 1;
  if (ctx->mcast_only)
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
  else
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.udp_port[MTL_SESSION_PORT_P] = 20000 + i * 2;
  ops_tx.type = type[i];
  ops_tx.sampling = sample[i];
  ops_tx.channel = channel[i];
  ops_tx.fmt = fmt[i];
  ops_tx.payload_type = ST30_TEST_PAYLOAD_TYPE;
  ops_tx.ssrc = i ? i + 0x66666666 : 0;
  ops_tx.ptime = ptime[i];
  ops_tx.pacing_way = ctx->tx_audio_pacing_way;
  ops_tx.framebuff_size =
      st30_get_packet_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling, ops_tx.channel);
  EXPECT_GE(ops_tx.framebuff_size, 0);
  ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
  ops_tx.get_next_frame = tx_audio_next_frame;
  ops_tx.notify_rtp_done = tx_rtp_done;
  ops_tx.rtp_ring_size = 1024;
  return ops_tx;
}

TEST_F(NoCtxTestSt30p, tx_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == NULL);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != NULL);

  /* by deafult don't limit cores */
  memset(ctx->lcores_list, 0, TEST_LCORE_LIST_MAX_LEN);
  ctx->para.lcores = NULL;
}
