/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The non-temporal blocked st20p conversion. st_frame_convert_nt() converts 8 lines at a
 * time into a scratch and streams each block to the frame; it must write exactly the
 * bytes the stock st20_* converters write, and fall back to one stock call wherever it
 * cannot block: an unaligned destination, a short or missing scratch, or a v210 width
 * the kernels cannot take a line at a time. st20p_tx_convert_block() lets an app build a
 * wire-format frame the same way from blocks of its own source lines, and the sessions
 * only set up a scratch where it can be used.
 *
 * Run:   ./build_unit/tests/unit/UnitTest
 *          --gtest_filter='*St20p*NtConvert*:*St20pTxConvertBlock*:*St20p*NtScratch*'
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <tuple>

extern "C" {
#include "st2110/st_convert.h"
}

#include "pipeline/st20p_harness.h"
#include "pipeline/st20p_tx_harness.h"

namespace {

constexpr enum st_frame_fmt kBe10 = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
constexpr enum st_frame_fmt kYuv = ST_FRAME_FMT_YUV422PLANAR10LE;
constexpr enum st_frame_fmt kV210 = ST_FRAME_FMT_V210;

/* Planes back to back, each line line_pad bytes past its least linesize, the first plane
 * offset bytes past a 64-byte boundary. A v210 width that is not a multiple of 3 has no
 * whole-byte lines, so it gets linesize 0 as st_frame_init_plane_single_src() gives it.
 */
class Frame {
 public:
  Frame(enum st_frame_fmt fmt, uint32_t width, uint32_t height, size_t offset = 0,
        size_t line_pad = 0, bool interlaced = false) {
    memset(&f_, 0, sizeof(f_));
    f_.fmt = fmt;
    f_.width = width;
    f_.height = height;
    f_.interlaced = interlaced;
    const uint32_t lines = st_frame_data_height(&f_);
    const uint8_t planes = st_frame_fmt_planes(fmt);
    for (uint8_t p = 0; p < planes; p++) {
      size_t least = st_frame_least_linesize(fmt, width, p);
      f_.linesize[p] = least ? least + line_pad : 0;
      size_ += f_.linesize[p] * lines;
    }
    if (!size_) size_ = st_frame_size(fmt, width, lines, false);
    mem_ = static_cast<uint8_t*>(aligned_alloc(64, (offset + size_ + 63) / 64 * 64));
    uint8_t* addr = mem_ + offset;
    for (uint8_t p = 0; p < planes; p++) {
      f_.addr[p] = addr;
      addr += f_.linesize[p] * lines;
    }
    f_.buffer_size = f_.data_size = size_;
  }
  ~Frame() {
    free(mem_);
  }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  struct st_frame* get() {
    return &f_;
  }
  uint8_t* data() {
    return static_cast<uint8_t*>(f_.addr[0]);
  }
  size_t size() const {
    return size_;
  }
  void Fill(uint8_t value) {
    memset(data(), value, size_);
  }
  /* xorshift samples, kept to the 10 bits per sample each format carries */
  void FillRandom(uint32_t seed) {
    uint32_t x = seed;
    auto next = [&x]() {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      return x;
    };
    if (f_.fmt == kYuv) {
      uint16_t* s = reinterpret_cast<uint16_t*>(data());
      for (size_t i = 0; i < size_ / 2; i++) s[i] = next() & 0x3ff;
    } else if (f_.fmt == kV210) {
      uint32_t* s = reinterpret_cast<uint32_t*>(data());
      for (size_t i = 0; i < size_ / 4; i++) s[i] = next() & 0x3fffffff;
    } else {
      for (size_t i = 0; i < size_; i++) data()[i] = next();
    }
  }

 private:
  struct st_frame f_;
  uint8_t* mem_ = nullptr;
  size_t size_ = 0;
};

class Scratch {
 public:
  explicit Scratch(size_t size)
      : addr_(size ? aligned_alloc(64, (size + 63) / 64 * 64) : nullptr) {
  }
  ~Scratch() {
    free(addr_);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  void* get() {
    return addr_;
  }

 private:
  void* addr_;
};

/* The public st20_* converter on the whole field or frame, or a line at a time for a
 * padded source. */
int StockConvert(struct st_frame* src, struct st_frame* dst, bool per_line = false) {
  const uint32_t w = dst->width, h = st_frame_data_height(dst);
  const uint32_t step = per_line ? 1 : h;
  for (uint32_t line = 0; line < h; line += step) {
    auto s = [&](int p) {
      return static_cast<uint8_t*>(src->addr[p]) + src->linesize[p] * line;
    };
    auto d = [&](int p) {
      return static_cast<uint8_t*>(dst->addr[p]) + dst->linesize[p] * line;
    };
    auto pg = [](uint8_t* addr) {
      return reinterpret_cast<struct st20_rfc4175_422_10_pg2_be*>(addr);
    };
    auto u16 = [](uint8_t* addr) { return reinterpret_cast<uint16_t*>(addr); };
    int ret;
    if (src->fmt == kBe10 && dst->fmt == kYuv)
      ret = st20_rfc4175_422be10_to_yuv422p10le(pg(s(0)), u16(d(0)), u16(d(1)), u16(d(2)),
                                                w, step);
    else if (src->fmt == kBe10 && dst->fmt == kV210)
      ret = st20_rfc4175_422be10_to_v210(pg(s(0)), d(0), w, step);
    else if (src->fmt == kYuv && dst->fmt == kBe10)
      ret = st20_yuv422p10le_to_rfc4175_422be10(u16(s(0)), u16(s(1)), u16(s(2)), pg(d(0)),
                                                w, step);
    else if (src->fmt == kV210 && dst->fmt == kBe10)
      ret = st20_v210_to_rfc4175_422be10(s(0), pg(d(0)), w, step);
    else
      ret = -ENOTSUP;
    if (ret < 0) return ret;
  }
  return 0;
}

int (*g_real_convert)(struct st_frame* src, struct st_frame* dst);
int g_convert_calls;

int CountingConvert(struct st_frame* src, struct st_frame* dst) {
  g_convert_calls++;
  return g_real_convert(src, dst);
}

/* st_frame_convert_nt() with the converter calls counted in g_convert_calls */
int NtConvert(struct st_frame* src, struct st_frame* dst, void* scratch,
              size_t scratch_size) {
  struct st_frame_converter converter;
  int ret = st_frame_get_converter(src->fmt, dst->fmt, &converter);
  if (ret < 0) return ret;
  g_real_convert = converter.convert_func;
  converter.convert_func = CountingConvert;
  g_convert_calls = 0;
  return st_frame_convert_nt(&converter, src, dst, scratch, scratch_size);
}

size_t BlockScratchSize(struct st_frame* dst) {
  return st_frame_nt_scratch_size(dst->fmt, dst->width);
}

/* the converter calls of the NT path: one per block of 8 lines */
int BlockCalls(struct st_frame* dst) {
  return (st_frame_data_height(dst) + 7) / 8;
}

/* The NT path also needs every dst plane on a cache line; back to back, the Cr plane of
 * 720x486 yuv422p10le starts 32 bytes off one, so that frame takes one stock call. */
int CallsIfPlanesAligned(struct st_frame* dst) {
  for (uint8_t p = 0; p < st_frame_fmt_planes(dst->fmt); p++)
    if ((uintptr_t)dst->addr[p] % 64) return 1;
  return BlockCalls(dst);
}

void ExpectSameBytes(const uint8_t* got, const uint8_t* want, size_t n) {
  if (!memcmp(got, want, n)) return;
  size_t i = 0;
  while (got[i] == want[i]) i++;
  ADD_FAILURE() << "first different byte at offset " << i << " of " << n;
}

struct Direction {
  enum st_frame_fmt src;
  enum st_frame_fmt dst;
  const char* name;
};

const Direction kDirections[] = {
    {kBe10, kYuv, "Be10ToYuv422p10le"},
    {kBe10, kV210, "Be10ToV210"},
    {kYuv, kBe10, "Yuv422p10leToBe10"},
    {kV210, kBe10, "V210ToBe10"},
};

const Direction kV210Directions[] = {
    {kBe10, kV210, "Be10ToV210"},
    {kV210, kBe10, "V210ToBe10"},
};

/* The NT output against the stock output, each over a fill the other does not use, so a
 * byte the NT path skips shows up as a difference; calls is the converter runs expected.
 */
void ExpectNtMatchesStock(Frame& src, Frame& nt, size_t scratch_size, int calls,
                          bool per_line = false) {
  Frame stock(nt.get()->fmt, nt.get()->width, nt.get()->height, 0, 0,
              nt.get()->interlaced);
  Scratch scratch(scratch_size);
  src.FillRandom(0x1234567);
  nt.Fill(0xa5);
  stock.Fill(0x5a);

  ASSERT_EQ(StockConvert(src.get(), stock.get(), per_line), 0);
  ASSERT_EQ(NtConvert(src.get(), nt.get(), scratch.get(), scratch_size), 0);
  EXPECT_EQ(g_convert_calls, calls);
  ExpectSameBytes(nt.data(), stock.data(), stock.size());
}

class St20pNtConvertTest
    : public ::testing::TestWithParam<std::tuple<Direction, uint32_t, uint32_t>> {};

/* 540 and 486 leave a 4 and a 6 line tail block; 720 wide lines are not whole cache
 * lines. */
TEST_P(St20pNtConvertTest, MatchesStockConverter) {
  const Direction d = std::get<0>(GetParam());
  const uint32_t w = std::get<1>(GetParam()), h = std::get<2>(GetParam());
  Frame src(d.src, w, h);
  Frame nt(d.dst, w, h);

  ExpectNtMatchesStock(src, nt, BlockScratchSize(nt.get()),
                       CallsIfPlanesAligned(nt.get()));
}

INSTANTIATE_TEST_SUITE_P(
    AllDirections, St20pNtConvertTest,
    ::testing::Combine(::testing::ValuesIn(kDirections), ::testing::Values(720u, 1920u),
                       ::testing::Values(1080u, 540u, 486u)),
    [](const ::testing::TestParamInfo<St20pNtConvertTest::ParamType>& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             std::to_string(std::get<1>(info.param)) + "x" +
             std::to_string(std::get<2>(info.param));
    });

class St20pNtConvertV210WidthTest
    : public ::testing::TestWithParam<std::tuple<Direction, uint32_t>> {};

/* v210 blocks need lines of whole 12-pixel-group batches and 422be10 blocks of whole
 * cache lines, a width that is a multiple of 48: 1944 is one of 24, and 1280 and 2048
 * lines are not even whole bytes. The scratch would fit any block. */
TEST_P(St20pNtConvertV210WidthTest, WidthNotAMultipleOf48FallsBackToStock) {
  const Direction d = std::get<0>(GetParam());
  const uint32_t w = std::get<1>(GetParam());
  Frame src(d.src, w, 720);
  Frame nt(d.dst, w, 720);

  ExpectNtMatchesStock(src, nt, st_frame_nt_scratch_size(kBe10, w) * 4, 1);
}

INSTANTIATE_TEST_SUITE_P(
    V210Directions, St20pNtConvertV210WidthTest,
    ::testing::Combine(::testing::ValuesIn(kV210Directions),
                       ::testing::Values(1280u, 2048u, 1944u)),
    [](const ::testing::TestParamInfo<St20pNtConvertV210WidthTest::ParamType>& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             std::to_string(std::get<1>(info.param)) + "x720";
    });

class St20pNtConvertLayoutTest : public ::testing::TestWithParam<Direction> {
 protected:
  static constexpr uint32_t kW = 1920;
  static constexpr uint32_t kH = 1080;
};

TEST_P(St20pNtConvertLayoutTest, UnalignedDestinationFallsBackToStock) {
  Frame src(GetParam().src, kW, kH);
  Frame nt(GetParam().dst, kW, kH, 8);

  ExpectNtMatchesStock(src, nt, BlockScratchSize(nt.get()), 1);
}

TEST_P(St20pNtConvertLayoutTest, PaddedSourceLinesMatchStock) {
  Frame src(GetParam().src, kW, kH, 0, 128);
  Frame nt(GetParam().dst, kW, kH);

  ExpectNtMatchesStock(src, nt, BlockScratchSize(nt.get()), BlockCalls(nt.get()), true);
}

TEST_P(St20pNtConvertLayoutTest, ScratchShortOfABlockFallsBackToStock) {
  Frame src(GetParam().src, kW, kH);
  Frame nt(GetParam().dst, kW, kH);

  ExpectNtMatchesStock(src, nt, BlockScratchSize(nt.get()) - 64, 1);
}

TEST_P(St20pNtConvertLayoutTest, NoScratchFallsBackToStock) {
  Frame src(GetParam().src, kW, kH);
  Frame nt(GetParam().dst, kW, kH);

  ExpectNtMatchesStock(src, nt, 0, 1);
}

/* An interlaced frame holds one 540-line field: 68 blocks, the last of 4 lines. */
TEST_P(St20pNtConvertLayoutTest, InterlacedFieldMatchesStock) {
  Frame src(GetParam().src, kW, kH, 0, 0, true);
  Frame nt(GetParam().dst, kW, kH, 0, 0, true);

  ExpectNtMatchesStock(src, nt, BlockScratchSize(nt.get()), 68);
}

INSTANTIATE_TEST_SUITE_P(AllDirections, St20pNtConvertLayoutTest,
                         ::testing::ValuesIn(kDirections),
                         [](const ::testing::TestParamInfo<Direction>& info) {
                           return std::string(info.param.name);
                         });

/* -EIO from the third block, -EAGAIN from any later one: only the -EIO may come back */
int FailFromTheThirdBlock(struct st_frame* src, struct st_frame* dst) {
  (void)src;
  (void)dst;
  g_convert_calls++;
  if (g_convert_calls < 3) return 0;
  return g_convert_calls == 3 ? -EIO : -EAGAIN;
}

TEST(St20pNtConvertErrorTest, ReturnsTheFirstBlockError) {
  struct st_frame_converter converter;
  ASSERT_EQ(st_frame_get_converter(kBe10, kYuv, &converter), 0);
  converter.convert_func = FailFromTheThirdBlock;
  Frame src(kBe10, 1920, 1080);
  Frame dst(kYuv, 1920, 1080);
  Scratch scratch(BlockScratchSize(dst.get()));
  g_convert_calls = 0;

  EXPECT_EQ(st_frame_convert_nt(&converter, src.get(), dst.get(), scratch.get(),
                                BlockScratchSize(dst.get())),
            -EIO);
}

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;

struct st20p_tx_ops SessionOps(enum st_frame_fmt input_fmt, uint32_t flags,
                               uint32_t width = kWidth, uint32_t height = kHeight,
                               bool interlaced = false,
                               enum st20_fmt transport_fmt = ST20_FMT_YUV_422_10BIT) {
  struct st20p_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.input_fmt = input_fmt;
  ops.transport_fmt = transport_fmt;
  ops.width = width;
  ops.height = height;
  ops.interlaced = interlaced;
  ops.flags = flags;
  return ops;
}

/* A two-framebuffer st20p TX session holding one got frame. */
class Session {
 public:
  ~Session() {
    if (ctx_) ut20p_tx_ctx_destroy(ctx_);
  }
  void Open(const struct st20p_tx_ops& ops) {
    ASSERT_EQ(ut20p_tx_init(), 0);
    ctx_ = ut20p_tx_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut20p_tx_ctx_set_wire_frames(ctx_, &ops), 0);
    frame_ = ut20p_tx_get_frame(ctx_);
    ASSERT_NE(frame_, nullptr);
  }
  void Open(enum st_frame_fmt input_fmt, uint32_t flags) {
    Open(SessionOps(input_fmt, flags));
  }
  st20p_tx_handle handle() {
    return ut20p_tx_handle(ctx_);
  }
  ut20p_tx_ctx* ctx() {
    return ctx_;
  }
  struct st_frame* frame() {
    return frame_;
  }

 private:
  ut20p_tx_ctx* ctx_ = nullptr;
  struct st_frame* frame_ = nullptr;
};

int ConvertLines(Session& s, Frame& src, uint32_t line, uint32_t nb_lines) {
  struct st_frame block = *src.get();
  for (uint8_t p = 0; p < st_frame_fmt_planes(block.fmt); p++)
    block.addr[p] = static_cast<uint8_t*>(block.addr[p]) + block.linesize[p] * line;
  return st20p_tx_convert_block(s.handle(), s.frame(), &block, line, nb_lines);
}

/* Converts the whole frame or field in a 3-line first block, 8-line blocks and what is
 * left, then compares it with the stock conversion. */
void ExpectBlocksMatchStock(Session& s, Frame& src) {
  Frame stock(kBe10, kWidth, s.frame()->height, 0, 0, s.frame()->interlaced);
  const uint32_t h = st_frame_data_height(s.frame());
  src.FillRandom(0x7654321);
  stock.Fill(0x5a);
  memset(s.frame()->addr[0], 0xa5, stock.size());
  ASSERT_EQ(StockConvert(src.get(), stock.get()), 0);

  for (uint32_t line = 0, nb_lines; line < h; line += nb_lines) {
    nb_lines = line ? std::min(8u, h - line) : 3;
    ASSERT_EQ(ConvertLines(s, src, line, nb_lines), 0) << "line " << line;
  }
  ExpectSameBytes(static_cast<uint8_t*>(s.frame()->addr[0]), stock.data(), stock.size());
}

class St20pTxConvertBlockTest
    : public ::testing::TestWithParam<std::tuple<enum st_frame_fmt, uint32_t>> {};

/* 3 + 134 * 8 + 5 = 1080 lines */
TEST_P(St20pTxConvertBlockTest, BlockByBlockMatchesWholeFrame) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, std::get<1>(GetParam())));
  Frame src(std::get<0>(GetParam()), kWidth, kHeight);

  ExpectBlocksMatchStock(s, src);
}

INSTANTIATE_TEST_SUITE_P(
    SourceFormats, St20pTxConvertBlockTest,
    ::testing::Combine(::testing::Values(kYuv, kV210),
                       ::testing::Values(0u, (uint32_t)ST20P_TX_FLAG_DISABLE_NT_CONVERT)),
    [](const ::testing::TestParamInfo<St20pTxConvertBlockTest::ParamType>& info) {
      return std::string(std::get<0>(info.param) == kYuv ? "Yuv422p10le" : "V210") +
             (std::get<1>(info.param) ? "_NtDisabled" : "_Nt");
    });

/* An interlaced frame is one 540-line field: 3 + 67 * 8 + 1 = 540 lines. */
TEST(St20pTxConvertBlockFieldTest, FieldLinesBlockByBlockMatchStock) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(SessionOps(kBe10, 0, kWidth, kHeight, true)));
  Frame src(kYuv, kWidth, kHeight, 0, 0, true);

  ExpectBlocksMatchStock(s, src);
}

TEST(St20pTxConvertBlockFieldTest, RejectsLinesPastTheFieldWithEinval) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(SessionOps(kBe10, 0, kWidth, kHeight, true)));
  Frame src(kYuv, kWidth, kHeight, 0, 0, true);

  EXPECT_EQ(ConvertLines(s, src, kHeight / 2 - 4, 8), -EINVAL);
}

/* the NT path stages a block in the scratch, then gives the scratch back */
TEST(St20pTxConvertBlockScratchTest, UsesTheScratchAndReleasesIt) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  Frame src(kYuv, kWidth, kHeight);
  src.FillRandom(0x2468ace);
  const size_t size = ut20p_tx_nt_scratch_size(s.ctx());
  ASSERT_GT(size, 0u);
  uint8_t* scratch = static_cast<uint8_t*>(ut20p_tx_nt_scratch(s.ctx()));
  memset(scratch, 0x77, size);

  ASSERT_EQ(ConvertLines(s, src, 0, 8), 0);
  EXPECT_LT(std::count(scratch, scratch + size, 0x77), (long)size);
  EXPECT_FALSE(ut20p_tx_nt_scratch_held(s.ctx()));
}

/* as when another app thread converts: the scratch stays untouched and stays held */
TEST(St20pTxConvertBlockHeldScratchTest, ConvertsWithoutTheScratchAndLeavesItHeld) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  Frame src(kYuv, kWidth, kHeight);
  const size_t size = ut20p_tx_nt_scratch_size(s.ctx());
  ASSERT_GT(size, 0u);
  uint8_t* scratch = static_cast<uint8_t*>(ut20p_tx_nt_scratch(s.ctx()));
  memset(scratch, 0x77, size);
  ut20p_tx_hold_nt_scratch(s.ctx(), true);

  ExpectBlocksMatchStock(s, src);
  EXPECT_TRUE(ut20p_tx_nt_scratch_held(s.ctx()));
  EXPECT_EQ(std::count(scratch, scratch + size, 0x77), (long)size);
}

TEST(St20pTxConvertBlockErrorTest, RejectsBadArgumentsWithEinval) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  Frame src(kYuv, kWidth, kHeight);
  Frame narrow(kYuv, kWidth / 2, kHeight);
  struct st_frame* f = s.frame();
  struct st_frame no_plane = *src.get();
  no_plane.addr[1] = nullptr;
  struct st_frame short_lines = *src.get();
  short_lines.linesize[2] -= 2;

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), nullptr, src.get(), 0, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, nullptr, 0, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, src.get(), 0, 0), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, src.get(), kHeight - 4, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, src.get(), UINT32_MAX, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, narrow.get(), 0, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, &no_plane, 0, 8), -EINVAL);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), f, &short_lines, 0, 8), -EINVAL);
}

TEST(St20pTxConvertBlockErrorTest, RejectsAFrameOfAnotherSessionWithEinval) {
  Session s, other;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  ASSERT_NO_FATAL_FAILURE(other.Open(kBe10, 0));
  Frame src(kYuv, kWidth, kHeight);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), other.frame(), src.get(), 0, 8), -EINVAL);
}

TEST(St20pTxConvertBlockErrorTest, RejectsAFrameAlreadyPutWithEio) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  Frame src(kYuv, kWidth, kHeight);
  ASSERT_EQ(ut20p_tx_put_frame(s.ctx(), s.frame()), 0);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), src.get(), 0, 8), -EIO);
}

TEST(St20pTxConvertBlockErrorTest, RejectsASessionThatConvertsWithEio) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kYuv, 0));
  Frame src(kYuv, kWidth, kHeight);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), src.get(), 0, 8), -EIO);
}

TEST(St20pTxConvertBlockErrorTest, RejectsAnExtFrameSessionWithEio) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, ST20P_TX_FLAG_EXT_FRAME));
  Frame src(kYuv, kWidth, kHeight);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), src.get(), 0, 8), -EIO);
}

TEST(St20pTxConvertBlockErrorTest, RejectsAnUnsupportedSourceFormatWithEnotsup) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kBe10, 0));
  Frame y210(ST_FRAME_FMT_Y210, kWidth, kHeight);
  Frame be10(kBe10, kWidth, kHeight);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), y210.get(), 0, 8), -ENOTSUP);
  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), be10.get(), 0, 8), -ENOTSUP);
}

TEST(St20pTxConvertBlockErrorTest, RejectsV210AtAWidthNotAMultipleOf48WithEnotsup) {
  Session s;
  ASSERT_NO_FATAL_FAILURE(s.Open(SessionOps(kBe10, 0, 1280, 720)));
  Frame v210(kV210, 1280, 720);

  EXPECT_EQ(st20p_tx_convert_block(s.handle(), s.frame(), v210.get(), 0, 8), -ENOTSUP);
}

/* one block of 8 output lines of 1920 wide 422be10: 8 * 4800 bytes */
constexpr size_t kBe10BlockSize = 38400;

size_t TxScratchSize(const struct st20p_tx_ops& ops) {
  EXPECT_EQ(ut20p_tx_init(), 0);
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(2);
  EXPECT_NE(ctx, nullptr);
  if (!ctx) return SIZE_MAX;
  EXPECT_EQ(ut20p_tx_ctx_set_wire_frames(ctx, &ops), 0);
  size_t size = ut20p_tx_nt_scratch_size(ctx);
  ut20p_tx_ctx_destroy(ctx);
  return size;
}

TEST(St20pTxNtScratchTest, DeriveWireFrameGetsAnOutputBlock) {
  EXPECT_EQ(TxScratchSize(SessionOps(kBe10, 0)), kBe10BlockSize);
}

TEST(St20pTxNtScratchTest, DisableFlagGetsNone) {
  EXPECT_EQ(TxScratchSize(SessionOps(kBe10, ST20P_TX_FLAG_DISABLE_NT_CONVERT)), 0u);
  EXPECT_EQ(TxScratchSize(SessionOps(kYuv, ST20P_TX_FLAG_DISABLE_NT_CONVERT)), 0u);
}

TEST(St20pTxNtScratchTest, ExtFrameDeriveGetsNone) {
  EXPECT_EQ(TxScratchSize(SessionOps(kBe10, ST20P_TX_FLAG_EXT_FRAME)), 0u);
}

TEST(St20pTxNtScratchTest, DeriveOtherTransportGetsNone) {
  EXPECT_EQ(TxScratchSize(SessionOps(ST_FRAME_FMT_YUV444RFC4175PG4BE10, 0, kWidth,
                                     kHeight, false, ST20_FMT_YUV_444_10BIT)),
            0u);
}

TEST(St20pTxNtScratchTest, ConvertingSessionGetsTheOutputBlockOnly) {
  EXPECT_EQ(TxScratchSize(SessionOps(kYuv, 0)), kBe10BlockSize);
  EXPECT_EQ(TxScratchSize(SessionOps(kV210, 0)), kBe10BlockSize);
}

TEST(St20pTxNtScratchTest, V210WidthNotAMultipleOf48GetsNone) {
  EXPECT_EQ(TxScratchSize(SessionOps(kV210, 0, 1280, 720)), 0u);
}

TEST(St20pTxNtScratchTest, ConverterWithoutNtGetsNone) {
  EXPECT_EQ(TxScratchSize(SessionOps(ST_FRAME_FMT_Y210, 0)), 0u);
}

/* one block of 8 output lines of 1920 wide yuv422p10le: 8 * (3840 + 1920 + 1920) bytes */
constexpr size_t kYuvBlockSize = 61440;

/* A two-framebuffer st20p RX session converting the transport to output_fmt. */
class RxSession {
 public:
  ~RxSession() {
    if (ctx_) ut20p_ctx_destroy(ctx_);
  }
  void Open(enum st_frame_fmt output_fmt, uint32_t width, uint32_t flags) {
    ASSERT_EQ(ut20p_init(), 0);
    ctx_ = ut20p_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut20p_ctx_set_converting_frames(ctx_, output_fmt, width, kHeight, flags),
              0);
  }
  ut20p_ctx* ctx() {
    return ctx_;
  }

 private:
  ut20p_ctx* ctx_ = nullptr;
};

int RxScratch(enum st_frame_fmt output_fmt, uint32_t width, uint32_t flags,
              size_t* size) {
  RxSession s;
  s.Open(output_fmt, width, flags);
  if (::testing::Test::HasFatalFailure()) return -EIO;
  *size = ut20p_nt_scratch_size(s.ctx());
  return 0;
}

TEST(St20pRxNtScratchTest, NtConverterGetsAnOutputBlock) {
  size_t size = SIZE_MAX;
  ASSERT_EQ(RxScratch(kYuv, kWidth, 0, &size), 0);
  EXPECT_EQ(size, kYuvBlockSize);
}

TEST(St20pRxNtScratchTest, DisableFlagGetsNone) {
  size_t size = SIZE_MAX;
  ASSERT_EQ(RxScratch(kYuv, kWidth, ST20P_RX_FLAG_DISABLE_NT_CONVERT, &size), 0);
  EXPECT_EQ(size, 0u);
}

TEST(St20pRxNtScratchTest, AutoDetectGetsNone) {
  size_t size = SIZE_MAX;
  ASSERT_EQ(RxScratch(kYuv, kWidth, ST20P_RX_FLAG_AUTO_DETECT, &size), 0);
  EXPECT_EQ(size, 0u);
}

TEST(St20pRxNtScratchTest, V210WidthNotAMultipleOf48GetsNoneAndStillCreates) {
  size_t size = SIZE_MAX;
  ASSERT_EQ(RxScratch(kV210, 1280, 0, &size), 0);
  EXPECT_EQ(size, 0u);
}

TEST(St20pRxNtScratchTest, ConverterWithoutNtGetsNone) {
  size_t size = SIZE_MAX;
  ASSERT_EQ(RxScratch(ST_FRAME_FMT_UYVY, kWidth, 0, &size), 0);
  EXPECT_EQ(size, 0u);
}

/* get_frame converts with the real 422be10 to yuv422p10le converter through the scratch,
 * then gives the scratch back */
TEST(St20pRxNtConvertTest, GetFrameUsesTheScratchAndReleasesIt) {
  RxSession s;
  ASSERT_NO_FATAL_FAILURE(s.Open(kYuv, kWidth, 0));
  const size_t size = ut20p_nt_scratch_size(s.ctx());
  ASSERT_EQ(size, kYuvBlockSize);
  uint8_t* scratch = static_cast<uint8_t*>(ut20p_nt_scratch(s.ctx()));
  memset(scratch, 0x77, size);
  ut20p_set_frame_ready(s.ctx(), 0);

  struct st_frame* frame = ut20p_get_frame(s.ctx());
  ASSERT_NE(frame, nullptr);
  EXPECT_LT(std::count(scratch, scratch + size, 0x77), (long)size);
  EXPECT_FALSE(ut20p_nt_scratch_held(s.ctx()));
  EXPECT_EQ(ut20p_put_frame(s.ctx(), frame), 0);
}

}  // namespace
