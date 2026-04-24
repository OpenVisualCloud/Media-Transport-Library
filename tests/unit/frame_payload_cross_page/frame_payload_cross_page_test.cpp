// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>
#include <stdbool.h>
#include <stdint.h>

extern "C" {
struct test_page {
  size_t va_offset;
  uint64_t iova;
  size_t len;
};
bool test_payload_cross_page(const struct test_page* pages, int n_pages, size_t offset,
                             size_t len);
}

namespace {

// tv_frame_payload_cross_page decides whether a [offset, offset+len)
// payload would DMA across an IOVA-discontiguous boundary. A "true"
// here forces the TX path to fall back to a chained mbuf instead of
// the zero-copy ext-buf single segment, so a wrong answer either
// silently drops bytes (false negative -> NIC reads from a wrong
// physical page) or wastes performance (false positive). Both are
// real bugs the unit tests should pin down.
//
// Two pages of 4 KiB each. iovas chosen non-contiguous so a payload
// that straddles the va boundary also straddles the iova boundary.
constexpr size_t kPage = 4096;

const struct test_page kTwoPagesNonContig[2] = {
    {/*va*/ 0, /*iova*/ 0x10000, /*len*/ kPage},
    {/*va*/ kPage, /*iova*/ 0x90000, /*len*/ kPage},
};

// Same VA layout but iovas physically adjacent — DMA can stream
// across without a chain.
const struct test_page kTwoPagesContig[2] = {
    {0, 0x10000, kPage},
    {kPage, 0x10000 + kPage, kPage},
};

TEST(FramePayloadCrossPage, EmptyPageTableAlwaysReturnsFalse) {
  // page_table_len == 0 is the IOVA:VA mode fast path.
  // Even a payload that "crosses" page boundaries in a hypothetical
  // table must report no-cross when no table is registered.
  EXPECT_FALSE(test_payload_cross_page(nullptr, 0, /*offset=*/0, /*len=*/kPage * 2));
  EXPECT_FALSE(test_payload_cross_page(nullptr, 0, /*offset=*/kPage - 1, /*len=*/2));
}

TEST(FramePayloadCrossPage, PayloadEntirelyInsideOnePageDoesNotCross) {
  // Pure single-page lookup; no boundary involved.
  EXPECT_FALSE(test_payload_cross_page(kTwoPagesNonContig, 2, /*offset=*/0,
                                       /*len=*/kPage));
  EXPECT_FALSE(test_payload_cross_page(kTwoPagesNonContig, 2, /*offset=*/100,
                                       /*len=*/200));
  // Last byte of page 0 inclusive — must still be classified as "in
  // page 0", not as crossing.
  EXPECT_FALSE(test_payload_cross_page(kTwoPagesNonContig, 2, /*offset=*/kPage - 100,
                                       /*len=*/100));
}

TEST(FramePayloadCrossPage, PayloadCrossingNonContiguousBoundaryReturnsTrue) {
  // Payload spans the last byte of page 0 and the first byte of page
  // 1. With non-contiguous iovas, this is the canonical "must chain"
  // case.
  EXPECT_TRUE(test_payload_cross_page(kTwoPagesNonContig, 2,
                                      /*offset=*/kPage - 1, /*len=*/2));
  EXPECT_TRUE(test_payload_cross_page(kTwoPagesNonContig, 2,
                                      /*offset=*/kPage - 100, /*len=*/200));
}

TEST(FramePayloadCrossPage, PayloadCrossingPhysicallyContiguousBoundaryReturnsFalse) {
  // The whole point of the iova-difference check (rather than a pure
  // VA-page check) is that two VA pages backed by adjacent physical
  // memory still allow zero-copy DMA. A naive
  //   (offset / page_size) != ((offset+len-1) / page_size)
  // implementation would mis-report this as a crossing.
  EXPECT_FALSE(test_payload_cross_page(kTwoPagesContig, 2,
                                       /*offset=*/kPage - 1, /*len=*/2));
  EXPECT_FALSE(test_payload_cross_page(kTwoPagesContig, 2,
                                       /*offset=*/kPage - 100, /*len=*/200));
}

TEST(FramePayloadCrossPage, BoundaryExactlyAtPageEndDoesNotCross) {
  // Payload occupies bytes [kPage - len, kPage - 1] — its last byte
  // is the last byte of page 0. No cross.
  for (size_t len : {size_t{1}, size_t{2}, size_t{64}, size_t{1500}, kPage - 1}) {
    EXPECT_FALSE(test_payload_cross_page(kTwoPagesNonContig, 2,
                                         /*offset=*/kPage - len, len))
        << "len=" << len;
  }
}

TEST(FramePayloadCrossPage, SingleBytePayloadNeverCrosses) {
  // len == 1 implies offset+len-1 == offset; the iova-diff is 0,
  // matches len-1 == 0. Should always be false regardless of where
  // it lands or how the page table looks.
  for (size_t off : {size_t{0}, size_t{1}, kPage - 1, kPage, kPage + 1, 2 * kPage - 1}) {
    EXPECT_FALSE(test_payload_cross_page(kTwoPagesNonContig, 2, off, /*len=*/1))
        << "off=" << off;
    EXPECT_FALSE(test_payload_cross_page(kTwoPagesContig, 2, off, /*len=*/1))
        << "off=" << off;
  }
}

TEST(FramePayloadCrossPage, ThreePagesAllPhysicallyContiguousNoCross) {
  // A jumbo payload that spans three VA pages but lands on three
  // physically adjacent pages should not need a chain.
  const struct test_page pages[3] = {
      {0 * kPage, 0x40000 + 0 * kPage, kPage},
      {1 * kPage, 0x40000 + 1 * kPage, kPage},
      {2 * kPage, 0x40000 + 2 * kPage, kPage},
  };
  EXPECT_FALSE(test_payload_cross_page(pages, 3, /*offset=*/100,
                                       /*len=*/2 * kPage + 500));
}

TEST(FramePayloadCrossPage, ThreePagesMiddlePageRelocatedCrosses) {
  // Pages 0 and 2 are adjacent; page 1 sits elsewhere in iova space.
  // A payload that touches the middle page should report a cross
  // even though the start and end iovas happen to be in adjacent
  // physical regions.
  const struct test_page pages[3] = {
      {0 * kPage, 0x40000 + 0 * kPage, kPage},
      {1 * kPage, 0x90000, kPage},  // displaced
      {2 * kPage, 0x40000 + 2 * kPage, kPage},
  };
  EXPECT_TRUE(test_payload_cross_page(pages, 3, /*offset=*/kPage - 1,
                                      /*len=*/2));
  EXPECT_TRUE(test_payload_cross_page(pages, 3, /*offset=*/2 * kPage - 1,
                                      /*len=*/2));
}

TEST(FramePayloadCrossPage, IovaDifferenceMatchingLenMinusOneIsTreatedAsContiguous) {
  // White-box check of the iova-arithmetic branch: if page 1 sits
  // exactly len-1 bytes after page 0's used range, the arithmetic
  // (iova_end - iova_start == len - 1) holds even though the bytes
  // come from two different page_table entries. Function returns
  // false. This documents the algorithm's contract — it trusts
  // iova adjacency, not page-table identity.
  const struct test_page pages[2] = {
      {0, 0x10000, kPage},
      {kPage, 0x10000 + kPage, kPage},  // adjacent in iova
  };
  EXPECT_FALSE(test_payload_cross_page(pages, 2, /*offset=*/kPage - 8,
                                       /*len=*/16));
}

}  // namespace
