/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bounds of the mt_bitmap_* helpers that the st20/st22 video RX and the st30
 * audio RX use to dedup the packets of the open frame.
 *
 * The bitmap here is 10 bytes, but the declared size is 5 bytes (40 bits).
 * Bytes 5..9 are a guard region: a helper that trusts the index writes into
 * them, and the test sees it.
 *
 * mt_util.h is C only, so the calls go through common/mt_bitmap_harness.c.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='MtBitmapTest.*'
 */

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

extern "C" {
bool ut_bitmap_test(uint8_t* bitmap, size_t size, int idx);
bool ut_bitmap_test_and_set(uint8_t* bitmap, size_t size, int idx);
bool ut_bitmap_test_and_unset(uint8_t* bitmap, size_t size, int idx);
}

namespace {

constexpr size_t kAllocBytes = 10;
constexpr size_t kDeclaredBytes = 5;
constexpr int kValidBits = (int)kDeclaredBytes * 8; /* 0..39 */

class MtBitmapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memset(bitmap_, 0, sizeof(bitmap_));
  }

  /* true while every byte the declared size does not cover is still zero */
  bool guard_intact() const {
    for (size_t i = kDeclaredBytes; i < kAllocBytes; i++) {
      if (bitmap_[i] != 0) return false;
    }
    return true;
  }

  uint8_t bitmap_[kAllocBytes];
};

/* Each bit of the declared range still works. */
TEST_F(MtBitmapTest, InRangeSetAndTest) {
  for (int idx = 0; idx < kValidBits; idx++) {
    EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
    EXPECT_FALSE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
    EXPECT_TRUE(ut_bitmap_test(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
    EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
  }
  for (size_t i = 0; i < kDeclaredBytes; i++) EXPECT_EQ(bitmap_[i], 0xFF);
  EXPECT_TRUE(guard_intact());
}

/* The first bit past the declared size is refused and writes nothing. */
TEST_F(MtBitmapTest, FirstBitPastSizeRefused) {
  EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, kValidBits));
  EXPECT_TRUE(guard_intact());
  EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, kValidBits));
}

/* Every bit of the untouchable half is refused, and none of it is written. */
TEST_F(MtBitmapTest, AllBitsPastSizeRefused) {
  for (int idx = kValidBits; idx < (int)kAllocBytes * 8; idx++) {
    EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
    EXPECT_TRUE(ut_bitmap_test_and_unset(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
    EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, idx)) << "idx " << idx;
  }
  EXPECT_TRUE(guard_intact());
  for (size_t i = 0; i < kDeclaredBytes; i++) EXPECT_EQ(bitmap_[i], 0x00);
}

/* An index far past the buffer, the shape of a packet index that a jumped RTP
 * timestamp or sequence id produces. */
TEST_F(MtBitmapTest, FarOutOfRangeRefused) {
  EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, 100000));
  EXPECT_TRUE(ut_bitmap_test_and_unset(bitmap_, kDeclaredBytes, 100000));
  EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, 100000));
  EXPECT_TRUE(guard_intact());
}

/* A negative index reaches in front of the buffer. */
TEST_F(MtBitmapTest, NegativeIdxRefused) {
  EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, -1));
  EXPECT_TRUE(ut_bitmap_test_and_unset(bitmap_, kDeclaredBytes, -8));
  EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, -1));
  for (size_t i = 0; i < kAllocBytes; i++) EXPECT_EQ(bitmap_[i], 0x00);
}

/* A size of zero has no valid index at all. */
TEST_F(MtBitmapTest, ZeroSizeRefusesEveryIdx) {
  EXPECT_TRUE(ut_bitmap_test_and_set(bitmap_, 0, 0));
  EXPECT_FALSE(ut_bitmap_test(bitmap_, 0, 0));
  for (size_t i = 0; i < kAllocBytes; i++) EXPECT_EQ(bitmap_[i], 0x00);
}

/* A session whose bitmap allocation failed must not crash the RX path. */
TEST_F(MtBitmapTest, NullBitmapRefused) {
  EXPECT_TRUE(ut_bitmap_test_and_set(NULL, kDeclaredBytes, 0));
  EXPECT_TRUE(ut_bitmap_test_and_unset(NULL, kDeclaredBytes, 0));
  EXPECT_FALSE(ut_bitmap_test(NULL, kDeclaredBytes, 0));
}

/* test_and_unset clears only inside the declared size. */
TEST_F(MtBitmapTest, UnsetStaysInRange) {
  ut_bitmap_test_and_set(bitmap_, kDeclaredBytes, kValidBits - 1);
  EXPECT_FALSE(ut_bitmap_test_and_unset(bitmap_, kDeclaredBytes, kValidBits - 1));
  EXPECT_FALSE(ut_bitmap_test(bitmap_, kDeclaredBytes, kValidBits - 1));
  EXPECT_TRUE(ut_bitmap_test_and_unset(bitmap_, kDeclaredBytes, kValidBits - 1));
  EXPECT_TRUE(guard_intact());
}

} /* namespace */
