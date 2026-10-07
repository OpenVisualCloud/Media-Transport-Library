/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * rv_frame_memcpy(): the RX frame copy streams whole destination cache lines
 * and copies the misaligned head and the sub-line tail normally, or copies it
 * all normally below head + 256 bytes or with a pkt lcore. Every one of those
 * splits must land exactly the source bytes and touch nothing outside the
 * destination range.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxFrameMemcpyTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "session/st20/st20_rx_test_base.h"

namespace {

constexpr size_t kGuard = 64;
constexpr size_t kMaxLen = 4096 + 64 + 33;
constexpr uint8_t kGuardByte = 0xa5;

}  // namespace

class St20RxFrameMemcpyTest : public St20RxBaseTest {
 protected:
  uint8_t* dst_buf_ = nullptr;
  uint8_t* src_buf_ = nullptr;
  size_t buf_size_ = 0;

  void SetUp() override {
    St20RxBaseTest::SetUp();
    /* 64 byte aligned, so dst_off alone sets the destination's line offset */
    buf_size_ = kGuard + 64 + kMaxLen + kGuard;
    dst_buf_ = static_cast<uint8_t*>(aligned_alloc(64, buf_size_));
    src_buf_ = static_cast<uint8_t*>(aligned_alloc(64, buf_size_));
    ASSERT_NE(dst_buf_, nullptr);
    ASSERT_NE(src_buf_, nullptr);
    for (size_t i = 0; i < buf_size_; i++) src_buf_[i] = static_cast<uint8_t>(i * 7 + 3);
  }

  void TearDown() override {
    free(dst_buf_);
    free(src_buf_);
    St20RxBaseTest::TearDown();
  }

  void check_copy(bool pkt_lcore, size_t dst_off, size_t src_off, size_t n) {
    memset(dst_buf_, kGuardByte, buf_size_);
    uint8_t* dst = dst_buf_ + kGuard + dst_off;
    const uint8_t* src = src_buf_ + kGuard + src_off;

    void* ret = ut20_frame_memcpy(ctx_, pkt_lcore, dst, src, n);

    ASSERT_EQ(ret, dst) << "dst_off " << dst_off << " src_off " << src_off << " n " << n;
    ASSERT_EQ(memcmp(dst, src, n), 0)
        << "dst_off " << dst_off << " src_off " << src_off << " n " << n;
    for (uint8_t* p = dst_buf_; p < dst; p++)
      ASSERT_EQ(*p, kGuardByte) << "wrote " << (dst - p) << " bytes before dst, dst_off "
                                << dst_off << " n " << n;
    for (uint8_t* p = dst + n; p < dst_buf_ + buf_size_; p++)
      ASSERT_EQ(*p, kGuardByte)
          << "wrote " << (p - dst - n) << " bytes past dst + n, dst_off " << dst_off
          << " n " << n;
  }

  void check_all(bool pkt_lcore) {
    const size_t fixed[] = {0, 1, 15, 63, 64, 65, 255, 256, 257, 1200, 1260, 4096 + 33};
    const size_t src_offs[] = {0, 1, 16, 33};
    for (size_t dst_off = 0; dst_off < 64; dst_off++) {
      /* the stream starts at the first line boundary at or above dst */
      const size_t head = (64 - dst_off) & 63;
      std::vector<size_t> lens(std::begin(fixed), std::end(fixed));
      /* both sides of the head + 256 threshold, and a tail of 0 and of 63 bytes */
      for (size_t extra : {255u, 256u, 257u, 256u + 63u, 256u + 64u})
        lens.push_back(head + extra);
      for (size_t src_off : src_offs) {
        for (size_t n : lens) {
          check_copy(pkt_lcore, dst_off, src_off, n);
          if (HasFatalFailure()) return;
        }
      }
    }
  }
};

TEST_F(St20RxFrameMemcpyTest, CopiesExactlyAtEveryDestinationAlignment) {
  check_all(false);
}

/* a session with a pkt lcore never streams, and must still copy exactly */
TEST_F(St20RxFrameMemcpyTest, PktLcoreCopiesExactlyAtEveryDestinationAlignment) {
  check_all(true);
}
