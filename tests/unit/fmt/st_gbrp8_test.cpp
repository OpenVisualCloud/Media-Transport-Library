/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins ST_FRAME_FMT_GBRPLANAR8, the 8-bit planar RGB frame format: three full-size
 * 4:4:4 planes of one byte per sample in G, B, R order (as GBRPLANAR10LE/12LE and
 * FFmpeg gbrp). It is what a planar-RGB codec such as JPEG XS with the RCT (ISO/IEC
 * 21122-1 Annex F, Cpih 1) takes and returns, so it has no st20 transport of its own;
 * RGB8 is the packed R, G, B form ST20_FMT_RGB_8BIT carries.
 *
 * Groups, all pure calls into the public frame API, no EAL:
 *   StGbrp8Fmt      - enum value, name, planes, sampling, sizes, linesizes, transport.
 *   StGbrp8Convert  - st_frame_convert() RGB8 <-> GBRPLANAR8, packed and padded lines.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='StGbrp8*'
 */

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include <vector>

extern "C" {
#include <mtl_api.h>

#include "st2110/st_fmt.h"
}

/* Format table: the new value must extend the RGB range without moving any existing
 * value, and describe three width-byte planes. */

/* Appended before RGB_END, so the RGB range grows by one and the neighbours keep
 * their ABI values. */
TEST(StGbrp8Fmt, AppendedToTheRgbRangeWithoutRenumbering) {
  EXPECT_EQ(ST_FRAME_FMT_RGBRFC4175PG2BE12, 38);
  EXPECT_EQ(ST_FRAME_FMT_GBRPLANAR8, 39);
  EXPECT_EQ(ST_FRAME_FMT_RGB_END, 40);
  EXPECT_EQ(ST_FRAME_FMT_CODESTREAM_START, 56);
}

/* The name is what apps and RxTxApp JSON pass, so it must round-trip. */
TEST(StGbrp8Fmt, NameRoundTrips) {
  EXPECT_STREQ(st_frame_fmt_name(ST_FRAME_FMT_GBRPLANAR8), "GBRPLANAR8");
  EXPECT_EQ(st_frame_name_to_fmt("GBRPLANAR8"), ST_FRAME_FMT_GBRPLANAR8);
}

/* Three planes, all at full resolution. */
TEST(StGbrp8Fmt, ThreePlanes444) {
  EXPECT_EQ(st_frame_fmt_planes(ST_FRAME_FMT_GBRPLANAR8), 3);
  EXPECT_EQ(st_frame_fmt_get_sampling(ST_FRAME_FMT_GBRPLANAR8), ST_FRAME_SAMPLING_444);
}

/* One byte per sample, three samples per pixel: the same size as packed RGB8. */
TEST(StGbrp8Fmt, FrameSizeIsThreeBytesPerPixel) {
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_GBRPLANAR8, 1920, 1080, false), 1920u * 1080 * 3);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_GBRPLANAR8, 1920, 1080, true),
            1920u * 1080 * 3 / 2);
  EXPECT_EQ(st_frame_size(ST_FRAME_FMT_GBRPLANAR8, 1920, 1080, false),
            st_frame_size(ST_FRAME_FMT_RGB8, 1920, 1080, false));
}

/* Each plane line holds width bytes; there is no fourth plane. */
TEST(StGbrp8Fmt, LeastLinesizeIsWidthPerPlane) {
  for (uint8_t plane = 0; plane < 3; plane++)
    EXPECT_EQ(st_frame_least_linesize(ST_FRAME_FMT_GBRPLANAR8, 1921, plane), 1921u)
        << "plane " << (int)plane;
  EXPECT_EQ(st_frame_least_linesize(ST_FRAME_FMT_GBRPLANAR8, 1921, 3), 0u);
}

/* Planar is not the RGB8 wire layout, so it must never be sent without conversion. */
TEST(StGbrp8Fmt, HasNoTransportFormat) {
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_GBRPLANAR8), ST20_FMT_MAX);
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_GBRPLANAR8, ST20_FMT_RGB_8BIT));
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_RGB_8BIT), ST_FRAME_FMT_RGB8);
}

/* Conversion: frames are described by hand so each plane's linesize, and the bytes
 * past it, are under the test's control. */

class StGbrp8Convert : public ::testing::Test {
 protected:
  static constexpr uint8_t kGuard = 0xa5;

  /* Packed RGB8 frame, linesize = 3 * w + pad bytes. */
  void InitRgb8(struct st_frame* f, std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                size_t pad) {
    memset(f, 0, sizeof(*f));
    f->fmt = ST_FRAME_FMT_RGB8;
    f->width = w;
    f->height = h;
    f->linesize[0] = (size_t)w * 3 + pad;
    buf.assign(f->linesize[0] * h, kGuard);
    f->addr[0] = buf.data();
    f->buffer_size = f->data_size = buf.size();
  }

  /* GBRPLANAR8 frame, plane p linesize = w + pad, planes back to back. */
  void InitGbrp8(struct st_frame* f, std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                 size_t pad) {
    memset(f, 0, sizeof(*f));
    f->fmt = ST_FRAME_FMT_GBRPLANAR8;
    f->width = w;
    f->height = h;
    size_t plane_size = (w + pad) * h;
    buf.assign(plane_size * 3, kGuard);
    for (int p = 0; p < 3; p++) {
      f->linesize[p] = w + pad;
      f->addr[p] = buf.data() + plane_size * p;
    }
    f->buffer_size = f->data_size = buf.size();
  }

  static uint8_t* Px(struct st_frame* f, int plane, uint32_t x, uint32_t y) {
    return (uint8_t*)f->addr[plane] + f->linesize[plane] * y + x;
  }
};

/* Packed R, G, B lands as G in plane 0, B in plane 1, R in plane 2. */
TEST_F(StGbrp8Convert, Rgb8ToGbrp8PlaneOrderIsGbr) {
  std::vector<uint8_t> src_buf, dst_buf;
  struct st_frame src, dst;
  InitRgb8(&src, src_buf, 2, 1, 0);
  InitGbrp8(&dst, dst_buf, 2, 1, 0);
  const uint8_t rgb[] = {0x10, 0x20, 0x30, 0x41, 0x52, 0x63};
  memcpy(src.addr[0], rgb, sizeof(rgb));

  ASSERT_EQ(st_frame_convert(&src, &dst), 0);
  EXPECT_EQ(*Px(&dst, 0, 0, 0), 0x20);
  EXPECT_EQ(*Px(&dst, 1, 0, 0), 0x30);
  EXPECT_EQ(*Px(&dst, 2, 0, 0), 0x10);
  EXPECT_EQ(*Px(&dst, 0, 1, 0), 0x52);
  EXPECT_EQ(*Px(&dst, 1, 1, 0), 0x63);
  EXPECT_EQ(*Px(&dst, 2, 1, 0), 0x41);
}

/* The inverse puts plane 0, 1, 2 back as the G, B, R bytes of each packed pixel. */
TEST_F(StGbrp8Convert, Gbrp8ToRgb8RestoresRgbOrder) {
  std::vector<uint8_t> src_buf, dst_buf;
  struct st_frame src, dst;
  InitGbrp8(&src, src_buf, 1, 1, 0);
  InitRgb8(&dst, dst_buf, 1, 1, 0);
  *Px(&src, 0, 0, 0) = 0x20;
  *Px(&src, 1, 0, 0) = 0x30;
  *Px(&src, 2, 0, 0) = 0x10;

  ASSERT_EQ(st_frame_convert(&src, &dst), 0);
  const uint8_t* rgb = (const uint8_t*)dst.addr[0];
  EXPECT_EQ(rgb[0], 0x10);
  EXPECT_EQ(rgb[1], 0x20);
  EXPECT_EQ(rgb[2], 0x30);
}

/* 8-bit to 8-bit is a pure byte shuffle, so a round trip over every byte value at an
 * odd size must be bit-exact. */
TEST_F(StGbrp8Convert, RoundTripIsBitExact) {
  const uint32_t w = 37, h = 11;
  std::vector<uint8_t> rgb_buf, gbrp_buf, back_buf;
  struct st_frame rgb, gbrp, back;
  InitRgb8(&rgb, rgb_buf, w, h, 0);
  InitGbrp8(&gbrp, gbrp_buf, w, h, 0);
  InitRgb8(&back, back_buf, w, h, 0);
  for (size_t i = 0; i < rgb_buf.size(); i++) rgb_buf[i] = (uint8_t)(i * 7 + 3);

  ASSERT_EQ(st_frame_convert(&rgb, &gbrp), 0);
  ASSERT_EQ(st_frame_convert(&gbrp, &back), 0);
  EXPECT_EQ(back_buf, rgb_buf);
}

/* With line padding on both sides each line starts at its own linesize, and the
 * padding bytes are left as they were. */
TEST_F(StGbrp8Convert, PaddedLinesRoundTripAndKeepPadding) {
  const uint32_t w = 5, h = 3;
  std::vector<uint8_t> rgb_buf, gbrp_buf, back_buf;
  struct st_frame rgb, gbrp, back;
  InitRgb8(&rgb, rgb_buf, w, h, 4);
  InitGbrp8(&gbrp, gbrp_buf, w, h, 3);
  InitRgb8(&back, back_buf, w, h, 2);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t i = 0; i < w * 3; i++) *Px(&rgb, 0, i, y) = (uint8_t)(y * 16 + i);

  ASSERT_EQ(st_frame_convert(&rgb, &gbrp), 0);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      EXPECT_EQ(*Px(&gbrp, 0, x, y), *Px(&rgb, 0, x * 3 + 1, y)) << x << "," << y;
      EXPECT_EQ(*Px(&gbrp, 1, x, y), *Px(&rgb, 0, x * 3 + 2, y)) << x << "," << y;
      EXPECT_EQ(*Px(&gbrp, 2, x, y), *Px(&rgb, 0, x * 3, y)) << x << "," << y;
    }
    for (int p = 0; p < 3; p++)
      for (uint32_t x = w; x < gbrp.linesize[p]; x++)
        EXPECT_EQ(*Px(&gbrp, p, x, y), kGuard) << "plane " << p << " line " << y;
  }

  ASSERT_EQ(st_frame_convert(&gbrp, &back), 0);
  for (uint32_t y = 0; y < h; y++) {
    EXPECT_EQ(memcmp(Px(&back, 0, 0, y), Px(&rgb, 0, 0, y), w * 3), 0) << "line " << y;
    for (uint32_t x = w * 3; x < back.linesize[0]; x++)
      EXPECT_EQ(*Px(&back, 0, x, y), kGuard) << "line " << y;
  }
}
