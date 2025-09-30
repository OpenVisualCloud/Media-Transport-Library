/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

class st30pDefaultTimestamp : public SessionUserData {
  public:
    uint64_t lastTimestamp;

    st30pDefaultTimestamp(St30pHandler *parentHandler = nullptr)
        : lastTimestamp(0) {
      idx_tx = 0;
      idx_rx = 0;
      parent = parentHandler;
      enable_rx_modifier = true;
    }

    void  rxTestFrameModifier(void* frame,
                                                size_t frame_size) {
      st30_frame* f = (st30_frame*)frame;
      St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);
      static uint64_t framebuffTime = st10_tai_to_media_clk(st30pParent->nsPacketTime, st30pParent->clockHrtz);

      EXPECT_NEAR(f->timestamp, st10_tai_to_media_clk((idx_rx) * st30pParent->nsPacketTime, st30pParent->clockHrtz), framebuffTime) << " idx_rx: " << idx_rx;
      if (lastTimestamp != 0) {
        uint64_t diff = f->timestamp - lastTimestamp;
        EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx;
      }

      lastTimestamp = f->timestamp;
      idx_rx++;
    }
};

class st30pUserTimestamp : public st30pDefaultTimestamp {
  uint64_t startingTime = 10 * NS_PER_MS;

  public:
    st30pUserTimestamp(St30pHandler *parentHandler = nullptr)
        : st30pDefaultTimestamp(parentHandler) {
          enable_tx_modifier = true;
          enable_rx_modifier = true;
        }

    void txTestFrameModifier(void* frame, size_t frame_size) {
      st30_frame* f = (st30_frame*)frame;
      static uint64_t startingTime = 10 * NS_PER_MS;
      St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);

      f->tfmt = ST10_TIMESTAMP_FMT_TAI;
      f->timestamp = startingTime + (st30pParent->nsPacketTime * (idx_tx));
      idx_tx++;
    }

    void  rxTestFrameModifier(void* frame,
                                                size_t frame_size) {
      st30_frame* f = (st30_frame*)frame;
      St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);


      static uint64_t lastTimestamp = 0;
      idx_rx++;

      uint64_t expectedTimestamp = startingTime + (st30pParent->nsPacketTime * (idx_rx - 1));
      uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, AUDIO_CLOCK_HRTZ);

      EXPECT_EQ(f->timestamp, expected_media_clk) << " idx_rx: " << idx_rx;

      if (lastTimestamp != 0) {
        uint64_t diff = f->timestamp - lastTimestamp;
        EXPECT_TRUE(diff == st10_tai_to_media_clk(st30pParent->nsPacketTime, AUDIO_CLOCK_HRTZ)) << " idx_rx: " << idx_rx;
      }

      lastTimestamp = f->timestamp;
    }
};

TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pDefaultTimestamp* userData = new st30pDefaultTimestamp();
  St30pHandler* handler = new St30pHandler(ctx, userData);
  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}


TEST_F(NoCtxTest, st30p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pUserTimestamp* userData = new st30pUserTimestamp();
  St30pHandler* handler = new St30pHandler(ctx);
  handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  handler->setModifiers(userData);
  handler->createSession(true);

  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

