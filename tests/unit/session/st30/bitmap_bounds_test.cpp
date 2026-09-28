/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Out-of-frame packet indices in the ST 2110-30 RX path.
 *
 * rx_audio_session_handle_frame_pkt() derives the slot of a packet from its RTP
 * timestamp. When that index leaves the open frame, nothing may be written:
 * neither a bit past the frame bitmap, nor a payload past the frame buffer.
 * The harness keeps guard bytes after the bitmap and leaves the spare frame
 * buffers zero, so a write outside is visible.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxBitmapBoundsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxBitmapBoundsTest : public St30RxBaseTest {
 protected:
  bool guard_intact() {
    return ut30_bitmap_guard_intact(ctx_);
  }
  bool spare_frames_intact() {
    return ut30_spare_frame_storage_intact(ctx_);
  }
};

/* Baseline: a full frame fills every slot of the bitmap and writes nothing
 * outside it. */
TEST_F(St30RxBitmapBoundsTest, FullFrameStaysInsideBitmap) {
  const uint32_t base = 10000;
  for (int i = 0; i < ppf(); i++) {
    feed((uint16_t)i, base + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  }
  EXPECT_EQ(received(), (uint64_t)ppf());
  EXPECT_TRUE(guard_intact());
  EXPECT_TRUE(spare_frames_intact());
}

/* The last slot of the frame is in range and must be accepted, so the guard
 * below is not simply refusing everything. */
TEST_F(St30RxBitmapBoundsTest, LastSlotOfFrameAccepted) {
  const uint32_t base = 10000;
  feed(0, base, MTL_SESSION_PORT_P);
  EXPECT_EQ(feed(1, base + (uint32_t)(ppf() - 1) * spp(), MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(received(), 2u);
  EXPECT_TRUE(guard_intact());
}

/* Ticks per packet that do not match the frame geometry: the handler recomputes
 * the index after it reopens the frame, and that second index is not range
 * checked. It resolves past the last slot, so the bitmap size is the only thing
 * that keeps the bit write inside the bitmap and the payload copy inside the
 * frame buffer. */
TEST_F(St30RxBitmapBoundsTest, IndexPastFrameRefusedAfterReopen) {
  const uint32_t base = 10000;
  const uint32_t real_spp = spp();

  feed(0, base, MTL_SESSION_PORT_P); /* opens the frame at base */
  ut30_set_samples_per_pkt(ctx_, real_spp / 2);

  /* the last packet of the frame, now resolving to about 2x its slot */
  feed(1, base + (uint32_t)(ppf() - 1) * real_spp, MTL_SESSION_PORT_P);

  EXPECT_TRUE(guard_intact()) << "a bit was set past the frame bitmap";
  EXPECT_TRUE(spare_frames_intact()) << "payload was copied past the frame buffer";
  EXPECT_EQ(received(), 1u) << "the out-of-frame packet must not count as received";
}

/* Same mismatch, every packet of the frame. None of them may write outside. */
TEST_F(St30RxBitmapBoundsTest, WholeFrameWithBadStrideStaysInside) {
  const uint32_t base = 10000;
  const uint32_t real_spp = spp();

  feed(0, base, MTL_SESSION_PORT_P);
  ut30_set_samples_per_pkt(ctx_, real_spp / 4);
  for (int i = 1; i < ppf(); i++) {
    feed((uint16_t)i, base + (uint32_t)i * real_spp, MTL_SESSION_PORT_P);
  }

  EXPECT_TRUE(guard_intact());
  EXPECT_TRUE(spare_frames_intact());
}

/* A timestamp far ahead of the open frame opens a new frame instead of
 * addressing a slot beyond the old one. */
TEST_F(St30RxBitmapBoundsTest, ForwardTimestampJumpOpensNewFrame) {
  const uint32_t base = 10000;
  feed(0, base, MTL_SESSION_PORT_P);
  feed(1, base + 1000u * (uint32_t)ppf() * spp(), MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_TRUE(guard_intact());
  EXPECT_TRUE(spare_frames_intact());
}
