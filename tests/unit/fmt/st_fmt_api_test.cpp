/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * Pure-CPU format, frame, alignment, bandwidth, and frame-rate mapping unit
 * tests.
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <st20_api.h>
#include <st_pipeline_api.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace {

using ScopedMallocFrame = std::unique_ptr<struct st_frame, decltype(&st_frame_free)>;

ScopedMallocFrame make_malloc_frame(enum st_frame_fmt fmt, uint32_t w, uint32_t h) {
  return ScopedMallocFrame(st_frame_create_by_malloc(fmt, w, h, false), &st_frame_free);
}

struct st_frame make_rfc4175_frame(
    std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
    enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10) {
  struct st_frame frame = {};
  frame.fmt = fmt;
  frame.width = w;
  frame.height = h;
  frame.addr[0] = buf.data();
  frame.linesize[0] = static_cast<size_t>(w) / 2 * 5;
  frame.buffer_size = buf.size();
  frame.data_size = buf.size();
  return frame;
}

void verify_downsample(enum st_frame_fmt fmt, uint32_t src_w, uint32_t src_h,
                       size_t unit_bytes, uint32_t unit_px,
                       std::initializer_list<int> indices) {
  const uint32_t dst_w = src_w / 2;
  const uint32_t dst_h = src_h / 2;
  const uint32_t src_units = src_w / unit_px;
  const uint32_t dst_units = dst_w / unit_px;

  auto src = make_malloc_frame(fmt, src_w, src_h);
  auto dst = make_malloc_frame(fmt, dst_w, dst_h);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  ASSERT_EQ(src->linesize[0], src_units * unit_bytes);
  ASSERT_EQ(dst->linesize[0], dst_units * unit_bytes);

  auto* src_b = static_cast<uint8_t*>(src->addr[0]);
  auto* dst_b = static_cast<uint8_t*>(dst->addr[0]);
  for (uint32_t r = 0; r < src_h; ++r) {
    for (uint32_t u = 0; u < src_units; ++u) {
      uint8_t* p = src_b + r * src->linesize[0] + u * unit_bytes;
      for (size_t b = 0; b < unit_bytes; ++b) {
        p[b] = static_cast<uint8_t>(((r * src_units + u) * 31u + b * 17u + 5u) & 0xFFu);
      }
    }
  }

  for (int idx : indices) {
    std::memset(dst_b, 0, dst->buffer_size);
    ASSERT_EQ(st_frame_downsample(src.get(), dst.get(), idx), 0) << "idx=" << idx;
    for (uint32_t r = 0; r < dst_h; ++r) {
      for (uint32_t u = 0; u < dst_units; ++u) {
        const uint32_t sr = r * 2 + static_cast<uint32_t>(idx / 2);
        const uint32_t su = u * 2 + static_cast<uint32_t>(idx % 2);
        const uint8_t* expect_p = src_b + sr * src->linesize[0] + su * unit_bytes;
        const uint8_t* actual_p = dst_b + r * dst->linesize[0] + u * unit_bytes;
        EXPECT_EQ(std::memcmp(actual_p, expect_p, unit_bytes), 0)
            << "fmt=" << fmt << " idx=" << idx << " r=" << r << " u=" << u;
      }
    }
  }
}

}  // namespace

TEST(Main, bandwidth) {
  uint64_t bw_1080p = 0;
  EXPECT_GE(st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                                   false, &bw_1080p),
            0);
  EXPECT_EQ(bw_1080p / 1000 / 1000, st20_1080p59_yuv422_10bit_bandwidth_mps());

  uint64_t bw_720p = 0;
  EXPECT_GE(st20_get_bandwidth_bps(1280, 720, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                                   false, &bw_720p),
            0);
  EXPECT_GT(bw_1080p, bw_720p);
}

TEST(Main, st20_frame_size) {
  EXPECT_EQ(st20_frame_size(ST20_FMT_YUV_422_10BIT, 1920, 1080), 1920u * 1080u * 5u / 2u);
}

TEST(Main, fmt_equal_transport) {
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                           ST20_FMT_YUV_422_10BIT));
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_8BIT));
  EXPECT_TRUE(st_frame_fmt_equal_transport(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT));
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR10LE,
                                            ST20_FMT_YUV_422_10BIT));
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_V210, ST20_FMT_YUV_422_10BIT));
  EXPECT_FALSE(
      st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR8, ST20_FMT_YUV_422_8BIT));
  EXPECT_FALSE(st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_12BIT));
}

TEST(Main, fmt_frame_transport) {
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_422_10BIT),
            ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_422_8BIT), ST_FRAME_FMT_UYVY);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_RGB_8BIT), ST_FRAME_FMT_RGB8);
  EXPECT_EQ(st_frame_fmt_from_transport(ST20_FMT_YUV_444_16BIT), ST_FRAME_FMT_MAX);
}

TEST(Main, fmt_to_transport) {
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10),
            ST20_FMT_YUV_422_10BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_UYVY), ST20_FMT_YUV_422_8BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_RGB8), ST20_FMT_RGB_8BIT);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422PLANAR10LE), ST20_FMT_MAX);
  EXPECT_EQ(st_frame_fmt_to_transport(ST_FRAME_FMT_V210), ST20_FMT_MAX);
}

TEST(Main, frame_api) {
  const std::pair<int, int> pixel_ranges[] = {
      {ST_FRAME_FMT_YUV_START, ST_FRAME_FMT_YUV_END},
      {ST_FRAME_FMT_RGB_START, ST_FRAME_FMT_RGB_END},
  };
  for (const auto& [start, end] : pixel_ranges) {
    for (int i = start; i < end; ++i) {
      const auto fmt = static_cast<enum st_frame_fmt>(i);
      EXPECT_GT(st_frame_size(fmt, 1920, 1080, false), 0u);
      EXPECT_GT(st_frame_fmt_planes(fmt), 0);
      EXPECT_GT(st_frame_least_linesize(fmt, 1920, 0), 0u);
    }
  }
  for (int i = ST_FRAME_FMT_CODESTREAM_START; i < ST_FRAME_FMT_CODESTREAM_END; ++i) {
    const auto fmt = static_cast<enum st_frame_fmt>(i);
    EXPECT_EQ(st_frame_size(fmt, 1920, 1080, false), 0u);
    EXPECT_EQ(st_frame_fmt_planes(fmt), 1);
    EXPECT_EQ(st_frame_least_linesize(fmt, 1920, 0), 0u);
  }
  for (auto bad : {ST_FRAME_FMT_YUV_END, ST_FRAME_FMT_RGB_END,
                   ST_FRAME_FMT_CODESTREAM_END, ST_FRAME_FMT_MAX}) {
    EXPECT_EQ(st_frame_size(bad, 1920, 1080, false), 0u);
  }
}

TEST(Main, frame_name) {
  const std::pair<int, int> ranges[] = {
      {ST_FRAME_FMT_YUV_START, ST_FRAME_FMT_YUV_END},
      {ST_FRAME_FMT_RGB_START, ST_FRAME_FMT_RGB_END},
      {ST_FRAME_FMT_CODESTREAM_START, ST_FRAME_FMT_CODESTREAM_END},
  };
  for (const auto& [start, end] : ranges) {
    for (int i = start; i < end; ++i) {
      const auto fmt = static_cast<enum st_frame_fmt>(i);
      const char* name = st_frame_fmt_name(fmt);
      EXPECT_STRNE(name, "unknown");
      EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
    }
  }
  for (auto bad : {ST_FRAME_FMT_YUV_END, ST_FRAME_FMT_RGB_END,
                   ST_FRAME_FMT_CODESTREAM_END, ST_FRAME_FMT_MAX}) {
    EXPECT_STREQ(st_frame_fmt_name(bad), "unknown");
  }
  EXPECT_EQ(st_frame_name_to_fmt("unknown"), ST_FRAME_FMT_MAX);
}

TEST(Main, size_page_align) {
  constexpr size_t kPg = 4096;
  EXPECT_EQ(mtl_size_page_align(kPg, kPg), kPg);
  EXPECT_EQ(mtl_size_page_align(kPg + 100, kPg), kPg * 2);
  EXPECT_EQ(mtl_size_page_align(kPg * 4, kPg), kPg * 4);
  EXPECT_EQ(mtl_size_page_align(kPg * 4 - 1, kPg), kPg * 4);
  EXPECT_EQ(mtl_size_page_align(kPg * 4 + 1, kPg), kPg * 5);
}

TEST(Main, frame_rate_to_st_fps) {
  const struct {
    double rate;
    enum st_fps expected;
  } cases[] = {
      {22.00, ST_FPS_MAX},      {22.97, ST_FPS_MAX},      {22.98, ST_FPS_P23_98},
      {23.00, ST_FPS_P23_98},   {23.98, ST_FPS_P23_98},   {23.99, ST_FPS_P23_98},
      {24.00, ST_FPS_P24},      {24.99, ST_FPS_P24},      {25.00, ST_FPS_P25},
      {26.00, ST_FPS_P25},      {27.00, ST_FPS_MAX},      {28.00, ST_FPS_MAX},
      {28.50, ST_FPS_MAX},      {29.97, ST_FPS_P29_97},   {29.99, ST_FPS_P29_97},
      {30.00, ST_FPS_P30},      {31.00, ST_FPS_P30},      {31.01, ST_FPS_MAX},
      {32.00, ST_FPS_MAX},      {48.00, ST_FPS_MAX},      {49.00, ST_FPS_P50},
      {49.50, ST_FPS_P50},      {50.00, ST_FPS_P50},      {50.50, ST_FPS_P50},
      {51.00, ST_FPS_P50},      {52.00, ST_FPS_MAX},      {58.93, ST_FPS_MAX},
      {58.94, ST_FPS_P59_94},   {59.94, ST_FPS_P59_94},   {59.99, ST_FPS_P59_94},
      {60.00, ST_FPS_P60},      {61.00, ST_FPS_P60},      {61.01, ST_FPS_MAX},
      {62.00, ST_FPS_MAX},      {98.99, ST_FPS_MAX},      {99.00, ST_FPS_P100},
      {100.00, ST_FPS_P100},    {101.00, ST_FPS_P100},    {101.01, ST_FPS_MAX},
      {118.87, ST_FPS_MAX},     {118.88, ST_FPS_P119_88}, {119.88, ST_FPS_P119_88},
      {119.99, ST_FPS_P119_88}, {120.00, ST_FPS_P120},    {120.01, ST_FPS_P120},
      {121.00, ST_FPS_P120},    {121.01, ST_FPS_MAX},     {122.00, ST_FPS_MAX},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(st_frame_rate_to_st_fps(c.rate), c.expected) << "rate=" << c.rate;
  }
}

TEST(StFmtDrawLogo, Rfc4175SubRectPreservesBorder) {
  constexpr uint32_t kW = 16, kH = 8;
  constexpr uint8_t kBorder = 0xA5;
  const size_t f_stride = static_cast<size_t>(kW) / 2 * 5;
  const struct {
    uint32_t x, y, lw, lh;
  } rects[] = {{4, 2, 6, 4}, {0, 0, kW, kH}};

  for (const auto& r : rects) {
    const size_t l_stride = static_cast<size_t>(r.lw) / 2 * 5;
    std::vector<uint8_t> f_buf(f_stride * kH, kBorder);
    std::vector<uint8_t> l_buf(l_stride * r.lh);
    for (size_t i = 0; i < l_buf.size(); ++i) {
      uint8_t v = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
      l_buf[i] = (v == kBorder) ? static_cast<uint8_t>(0x5Au) : v;
    }
    struct st_frame frame = make_rfc4175_frame(f_buf, kW, kH);
    struct st_frame logo = make_rfc4175_frame(l_buf, r.lw, r.lh);
    ASSERT_EQ(st_draw_logo(&frame, &logo, r.x, r.y), 0);

    const size_t xb0 = static_cast<size_t>(r.x) / 2 * 5;
    const size_t xb1 = static_cast<size_t>(r.x + r.lw) / 2 * 5;
    for (uint32_t row = 0; row < kH; ++row) {
      for (size_t col = 0; col < f_stride; ++col) {
        const bool in = (row >= r.y && row < r.y + r.lh && col >= xb0 && col < xb1);
        const uint8_t exp = in ? l_buf[(row - r.y) * l_stride + (col - xb0)] : kBorder;
        EXPECT_EQ(f_buf[row * f_stride + col], exp) << "row=" << row << " col=" << col;
      }
    }
  }
}

TEST(StFmtDrawLogo, RejectsOutOfBoundsAndInvalidFormats) {
  std::vector<uint8_t> f_buf(16 / 2 * 5 * 8, 0xA5);
  const std::vector<uint8_t> untouched = f_buf;
  std::vector<uint8_t> l_buf(8 / 2 * 5 * 4, 0x3C);
  struct st_frame frame = make_rfc4175_frame(f_buf, 16, 8);
  struct st_frame logo = make_rfc4175_frame(l_buf, 8, 4);

  EXPECT_EQ(st_draw_logo(&frame, &logo, 10, 0), -EINVAL);
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 6), -EINVAL);
  logo.fmt = ST_FRAME_FMT_UYVY;
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 0), -EINVAL);
  frame.fmt = ST_FRAME_FMT_UYVY;
  EXPECT_EQ(st_draw_logo(&frame, &logo, 0, 0), -EINVAL);
  EXPECT_EQ(f_buf, untouched);
}

TEST(StFmtNameToCodec, MapsAllKnownCodecs) {
  const std::pair<const char*, enum st22_codec> known[] = {
      {"jpegxs", ST22_CODEC_JPEGXS}, {"h264_cbr", ST22_CODEC_H264_CBR},
      {"h264", ST22_CODEC_H264},     {"h265_cbr", ST22_CODEC_H265_CBR},
      {"h265", ST22_CODEC_H265},
  };
  for (const auto& [name, codec] : known) EXPECT_EQ(st_name_to_codec(name), codec);
}

TEST(StFmtNameToCodec, RejectsUnknownEmptyAndWrongCase) {
  for (const char* bad : {"", "unknown", "JPEGXS", "H264", "H265_CBR", "jpegxs_extra"}) {
    EXPECT_EQ(st_name_to_codec(bad), ST22_CODEC_MAX) << bad;
  }
}

TEST(Main, st20_pgroup_and_fmt_name) {
  for (int i = 0; i < ST20_FMT_MAX; ++i) {
    const auto fmt = static_cast<enum st20_fmt>(i);
    struct st20_pgroup pg = {};
    ASSERT_EQ(st20_get_pgroup(fmt, &pg), 0) << "fmt=" << i;
    EXPECT_EQ(pg.fmt, fmt);
    EXPECT_GT(pg.size, 0u);
    EXPECT_GT(pg.coverage, 0u);
    ASSERT_NE(pg.name, nullptr);
    const char* name = st20_fmt_name(fmt);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, pg.name);
    EXPECT_STRNE(name, "unknown");
    EXPECT_EQ(st20_name_to_fmt(name), fmt);
  }

  const struct {
    enum st20_fmt fmt;
    uint32_t size, coverage;
  } canonical[] = {
      {ST20_FMT_YUV_422_10BIT, 5u, 2u},
      {ST20_FMT_YUV_422_8BIT, 4u, 2u},
      {ST20_FMT_RGB_8BIT, 3u, 1u},
      {ST20_FMT_YUV_444_10BIT, 15u, 4u},
  };
  for (const auto& c : canonical) {
    struct st20_pgroup pg = {};
    ASSERT_EQ(st20_get_pgroup(c.fmt, &pg), 0);
    EXPECT_EQ(pg.size, c.size);
    EXPECT_EQ(pg.coverage, c.coverage);
  }

  struct st20_pgroup invalid_pg = {};
  EXPECT_LT(st20_get_pgroup(ST20_FMT_MAX, &invalid_pg), 0);
  EXPECT_STREQ(st20_fmt_name(ST20_FMT_MAX), "unknown");
  EXPECT_EQ(st20_name_to_fmt("unknown"), ST20_FMT_MAX);
  EXPECT_EQ(st20_name_to_fmt("not_a_real_fmt"), ST20_FMT_MAX);
}

TEST(Main, frame_sanity_check) {
  auto frame = make_malloc_frame(ST_FRAME_FMT_YUV422PLANAR10LE, 64, 32);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(st_frame_sanity_check(frame.get()), 0);

  auto check_bad = [&](auto mutate) {
    struct st_frame bad = *frame;
    mutate(bad);
    EXPECT_LT(st_frame_sanity_check(&bad), 0);
  };
  check_bad([](st_frame& f) { f.fmt = ST_FRAME_FMT_MAX; });
  check_bad([](st_frame& f) { f.addr[0] = nullptr; });
  check_bad([](st_frame& f) { f.addr[1] = nullptr; });
  check_bad([&](st_frame& f) {
    f.linesize[0] = st_frame_least_linesize(frame->fmt, frame->width, 0) - 1;
  });
  check_bad([](st_frame& f) { f.data_size = f.buffer_size + 1; });
  check_bad([](st_frame& f) { f.data_size = f.buffer_size - 1; });
}

TEST(Main, frame_downsample) {
  /* Width 32 -> linesize 80 >= 5 + 64 so AVX-512 VBMI2 64-byte loads stay in-bounds. */
  verify_downsample(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 32, 8, 5, 2, {0, 1, 2, 3});
  verify_downsample(ST_FRAME_FMT_RGB8, 16, 8, 3, 1, {0, 3});

  auto src = make_malloc_frame(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 32, 8);
  auto dst = make_malloc_frame(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 16, 4);
  ASSERT_TRUE(src && dst);
  EXPECT_LT(st_frame_downsample(src.get(), dst.get(), -1), 0);
  EXPECT_LT(st_frame_downsample(src.get(), dst.get(), 4), 0);

  auto check_bad = [&](auto mutate) {
    struct st_frame bad = *dst;
    mutate(bad);
    EXPECT_LT(st_frame_downsample(src.get(), &bad, 0), 0);
  };
  check_bad([](st_frame& f) { f.fmt = ST_FRAME_FMT_RGB8; });
  check_bad([](st_frame& f) { f.width = 12; });
  check_bad([](st_frame& f) { f.height = 6; });

  auto p_src = make_malloc_frame(ST_FRAME_FMT_YUV422PLANAR10LE, 16, 8);
  auto p_dst = make_malloc_frame(ST_FRAME_FMT_YUV422PLANAR10LE, 8, 4);
  ASSERT_TRUE(p_src && p_dst);
  EXPECT_LT(st_frame_downsample(p_src.get(), p_dst.get(), 0), 0);
}

TEST(Main, rxp_txp_para_setters) {
  char ip_p[] = "239.168.85.20", ip_r[] = "239.168.85.21", ip_bad[] = "999.0.0.1";
  const uint8_t exp_p[4] = {239, 168, 85, 20}, exp_r[4] = {239, 168, 85, 21};

  struct st_rx_port rxp = {};
  ASSERT_EQ(st_rxp_para_ip_set(&rxp, MTL_PORT_P, ip_p), 0);
  ASSERT_EQ(st_rxp_para_ip_set(&rxp, MTL_PORT_R, ip_r), 0);
  EXPECT_EQ(std::memcmp(rxp.ip_addr[MTL_SESSION_PORT_P], exp_p, 4), 0);
  EXPECT_EQ(std::memcmp(rxp.ip_addr[MTL_SESSION_PORT_R], exp_r, 4), 0);
  EXPECT_LT(st_rxp_para_ip_set(&rxp, MTL_PORT_P, ip_bad), 0);

  struct st_tx_port txp = {};
  ASSERT_EQ(st_txp_para_dip_set(&txp, MTL_PORT_P, ip_p), 0);
  ASSERT_EQ(st_txp_para_dip_set(&txp, MTL_PORT_R, ip_r), 0);
  EXPECT_EQ(std::memcmp(txp.dip_addr[MTL_SESSION_PORT_P], exp_p, 4), 0);
  EXPECT_EQ(std::memcmp(txp.dip_addr[MTL_SESSION_PORT_R], exp_r, 4), 0);
  EXPECT_LT(st_txp_para_dip_set(&txp, MTL_PORT_P, ip_bad), 0);
}
