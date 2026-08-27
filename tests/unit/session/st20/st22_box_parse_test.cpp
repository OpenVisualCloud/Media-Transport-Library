/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-22 first-packet box parsing (rv_parse_st22_boxes).
 *
 * Both lengths these tests drive are attacker-controlled on the wire: the JPEG
 * 2000 box lengths (lbox) and the datagram length itself. Each must be bounded
 * against the bytes the packet actually carries BEFORE it is used as a pointer
 * offset, otherwise a single crafted packet walks the parse pointer out of the
 * mbuf.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

/* On-the-wire lbox of a well-formed pair (struct st22_jpvs / st22_colr). */
static constexpr uint32_t kJpvsLen = 42;
static constexpr uint32_t kColrLen = 18;
static constexpr uint16_t kBoxesLen = kJpvsLen + kColrLen;
/* A box header is lbox(4) + tbox(4). */
static constexpr uint16_t kBoxHdrLen = 8;

class St22RxBoxParseTest : public St20RxBaseTest {
 protected:
  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_st22(ctx_);
  }

  uint64_t idx_dropped() {
    return ut20_stat_idx_dropped(ctx_);
  }
  uint64_t st22_boxes() {
    return ut20_stat_st22_boxes(ctx_);
  }
  uint64_t wrong_len() {
    return ut20_stat_wrong_len(ctx_);
  }
  uint64_t frames_ready() {
    return ut20_st22_frames_ready(ctx_);
  }
  size_t last_frame_size() {
    return ut20_st22_last_frame_size(ctx_);
  }

  /* First packet of a frame carrying `payload_length` bytes of `payload`. */
  int feed_first(const void* payload, uint16_t payload_length) {
    return ut20_feed_st22_pkt(ctx_, 100, 1000, 0, false, payload, payload_length,
                              MTL_SESSION_PORT_P);
  }
};

/* Positive control: one well-formed marker pkt completes a frame, and the size
 * delivered is the payload minus the boxes, so box bytes are skipped not copied. */
TEST_F(St22RxBoxParseTest, WellFormedMarkerPktCompletesFrame) {
  uint8_t payload[kBoxesLen + 40];
  memset(payload, 0xA5, sizeof(payload));
  ASSERT_EQ(ut20_st22_build_boxes(payload, kJpvsLen, kColrLen), kBoxesLen);

  ASSERT_EQ(ut20_feed_st22_pkt(ctx_, 100, 1000, 0, true, payload, sizeof(payload),
                               MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(frames_ready(), 1u);
  EXPECT_EQ(last_frame_size(), 40u);
  EXPECT_EQ(idx_dropped(), 0u);
  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(wrong_len(), 0u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
}

/* An lbox of 0xF0000000 must be rejected where it is read, not after the parse
 * pointer has already been advanced by ~4 GiB and dereferenced (CWE-125). */
TEST_F(St22RxBoxParseTest, OversizedJpvsBoxRejected) {
  uint8_t payload[kBoxesLen] = {0};
  ut20_st22_build_boxes(payload, 0xF0000000, kColrLen);

  /* only the 8 byte jpvs box hdr is on the wire */
  EXPECT_EQ(feed_first(payload, kBoxHdrLen), -EIO);
  EXPECT_EQ(idx_dropped(), 1u);
  EXPECT_EQ(st22_boxes(), 0u);
  /* must die in the box parser, not by luck in a later bounds check */
  EXPECT_EQ(offset_dropped(), 0u);
}

/* An lbox under the 512 byte total cap but past the end of this packet is still
 * out of bounds for the colr read that follows it. */
TEST_F(St22RxBoxParseTest, JpvsBoxLargerThanPayloadRejected) {
  uint8_t payload[kBoxesLen] = {0};
  ut20_st22_build_boxes(payload, 512, kColrLen);

  EXPECT_EQ(feed_first(payload, kBoxHdrLen), -EIO);
  EXPECT_EQ(idx_dropped(), 1u);
  EXPECT_EQ(offset_dropped(), 0u);
}

/* An lbox smaller than the box header it sits in cannot describe a box. */
TEST_F(St22RxBoxParseTest, JpvsBoxSmallerThanBoxHeaderRejected) {
  uint8_t payload[kBoxesLen] = {0};
  ut20_st22_build_boxes(payload, kBoxHdrLen - 4, kColrLen);

  EXPECT_EQ(feed_first(payload, kBoxHdrLen), -EIO);
  EXPECT_EQ(idx_dropped(), 1u);
}

/* The same floor applies to the second box, whose length is added to
 * st22_box_hdr_length and so shifts where the codestream is taken to start. */
TEST_F(St22RxBoxParseTest, ColrBoxSmallerThanBoxHeaderRejected) {
  uint8_t payload[kBoxesLen] = {0};
  ut20_st22_build_boxes(payload, kJpvsLen, kBoxHdrLen - 4);

  EXPECT_EQ(feed_first(payload, kJpvsLen + kBoxHdrLen), -EIO);
  EXPECT_EQ(idx_dropped(), 1u);
}

/* The colr length must be bounded by what is left AFTER jpvs: colr=45 clears
 * both the 512 byte cap and the 50 byte payload, only the 8 bytes left reject it. */
TEST_F(St22RxBoxParseTest, ColrBoxLargerThanRemainingPayloadRejected) {
  uint8_t payload[kBoxesLen] = {0};
  ut20_st22_build_boxes(payload, kJpvsLen, 45);

  EXPECT_EQ(feed_first(payload, kJpvsLen + kBoxHdrLen), -EIO);
  EXPECT_EQ(idx_dropped(), 1u);
  EXPECT_EQ(offset_dropped(), 0u);
}

/* A payload too short to hold a box header carries no boxes: the tbox compare
 * must stop at data_len, though a full box hdr sits past it in the mbuf. */
TEST_F(St22RxBoxParseTest, PayloadTooShortForBoxHeaderIgnored) {
  const uint8_t payload[kBoxHdrLen] = {0xF0, 0x00, 0x00, 0x00, 'j', 'p', 'v', 's'};

  EXPECT_EQ(ut20_feed_st22_pkt_data_len(ctx_, 100, 1000, payload, sizeof(payload),
                                        ut20_st22_hdr_len() + kBoxHdrLen - 4,
                                        MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(st22_boxes(), 0u);
  EXPECT_EQ(idx_dropped(), 0u);
}

/* The bytes left after jpvs must be bounded before the colr tbox is compared;
 * here only 4 of the well-formed colr sitting behind jpvs are on the wire. */
TEST_F(St22RxBoxParseTest, ColrBoxPastPayloadEndIgnored) {
  uint8_t payload[kBoxesLen] = {0};
  ASSERT_EQ(ut20_st22_build_boxes(payload, kJpvsLen, kColrLen), kBoxesLen);

  EXPECT_EQ(
      ut20_feed_st22_pkt_data_len(ctx_, 100, 1000, payload, sizeof(payload),
                                  ut20_st22_hdr_len() + kJpvsLen + 4, MTL_SESSION_PORT_P),
      0);
  EXPECT_EQ(st22_boxes(), 1u);
  EXPECT_EQ(idx_dropped(), 0u);
}

/* Exact fit: a real sender's 60 byte jpvs+colr pair leaves colr filling the 18
 * bytes left after jpvs, so the colr bound must be `>` and not `>=`. */
TEST_F(St22RxBoxParseTest, BoxesExactlyFillingPayloadAccepted) {
  uint8_t payload[kBoxesLen] = {0};
  ASSERT_EQ(ut20_st22_build_boxes(payload, kJpvsLen, kColrLen), kBoxesLen);

  EXPECT_EQ(feed_first(payload, kBoxesLen), 0);
  EXPECT_EQ(st22_boxes(), 1u);
  EXPECT_EQ(idx_dropped(), 0u);
}

/* st22_box_hdr_length is measured against the pkt that declared it, yet every
 * pkt_counter 0 pkt is stripped by it, so a shorter later one underflows. */
TEST_F(St22RxBoxParseTest, SecondFirstPktShorterThanBoxesRejected) {
  uint8_t payload[kBoxesLen + 40];
  memset(payload, 0xA5, sizeof(payload));
  ASSERT_EQ(ut20_st22_build_boxes(payload, kJpvsLen, kColrLen), kBoxesLen);
  ASSERT_EQ(feed_first(payload, sizeof(payload)), 0);

  /* marker, so the payload_length equality check does not reject it first */
  EXPECT_EQ(ut20_feed_st22_pkt(ctx_, 101, 1000, 0, true, payload, 10, MTL_SESSION_PORT_P),
            -EIO);
  EXPECT_EQ(wrong_len(), 1u);
  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(frames_ready(), 0u);
}

/* A datagram shorter than the 58 byte header underflows payload_length to ~64K,
 * which is what the box bounds above are measured against. Reachable on the
 * kernel socket datapath, which sizes data_len from recvfrom. */
TEST_F(St22RxBoxParseTest, TruncatedPktBelowHeaderRejected) {
  EXPECT_EQ(
      ut20_feed_st22_pkt_data_len(ctx_, 100, 1000, nullptr, 0, 50, MTL_SESSION_PORT_P),
      -EIO);
  EXPECT_EQ(wrong_len(), 1u);
  EXPECT_EQ(st22_boxes(), 0u);
}
