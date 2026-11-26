/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

namespace {

int test_dma_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.dma_dev_cnt;
}

void test_dma_create_one(struct st_tests_context* ctx) {
  int base_cnt = test_dma_cnt(ctx);
  mtl_test::ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);
  EXPECT_EQ(base_cnt + 1, test_dma_cnt(ctx));
  dma.reset();
  EXPECT_EQ(base_cnt, test_dma_cnt(ctx));
}

void test_dma_create_max(struct st_tests_context* ctx) {
  struct mtl_fix_info fix;
  ASSERT_GE(mtl_get_fix_info(ctx->handle, &fix), 0);

  int base_cnt = test_dma_cnt(ctx);
  std::vector<mtl_test::ScopedUdma> dmas;
  dmas.reserve(MTL_DMA_DEV_MAX);

  while (dmas.size() < MTL_DMA_DEV_MAX) {
    mtl_test::ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
    if (!dma) break;
    dmas.push_back(std::move(dma));
    EXPECT_EQ(base_cnt + static_cast<int>(dmas.size()), test_dma_cnt(ctx));
  }

  ASSERT_GT(dmas.size(), 0u);
  EXPECT_LE(dmas.size(), static_cast<size_t>(fix.dma_dev_cnt_max));

  for (int remain = static_cast<int>(dmas.size()) - 1; remain >= 0; remain--) {
    dmas.pop_back();
    EXPECT_EQ(base_cnt + remain, test_dma_cnt(ctx));
  }
}

bool test_dma_sysfs_long(const std::string& dev, const char* attr, long& out) {
  std::ifstream file("/sys/bus/pci/devices/" + dev + "/" + attr);
  std::string value;
  if (!(file >> value)) return false;
  char* end = nullptr;
  errno = 0;
  long parsed = std::strtol(value.c_str(), &end, 0);
  if (errno != 0 || end == value.c_str() || *end != '\0') return false;
  out = parsed;
  return true;
}

struct DirCloser {
  void operator()(DIR* d) const {
    if (d) closedir(d);
  }
};

/* Intel DMA devices in the mt_dma_auto.c allowlist on vfio-pci for a NUMA node. */
int test_dma_vfio_cnt(int numa) {
  static constexpr std::array<uint16_t, 15> kDmaAutoIds = {
      0x6f20, 0x6f21, 0x6f22, 0x6f23, 0x6f24,
      0x6f25, 0x6f26, 0x6f27, 0x6f2e, 0x6f2f, /* BDX */
      0x2021, 0x0b00, 0x0b25, 0x11fb, 0x1212, /* SKX/ICX/SPR/GNR-D/DMR */
  };

  std::unique_ptr<DIR, DirCloser> dir(opendir("/sys/bus/pci/devices"));
  if (!dir) return 0;

  char link[256];
  int cnt = 0;
  for (struct dirent* ent; (ent = readdir(dir.get()));) {
    std::string dev = ent->d_name;
    long vendor = 0, device = 0, dev_numa = 0;
    if (!test_dma_sysfs_long(dev, "vendor", vendor) || vendor != 0x8086) continue;
    if (!test_dma_sysfs_long(dev, "device", device)) continue;
    if (std::find(kDmaAutoIds.begin(), kDmaAutoIds.end(),
                  static_cast<uint16_t>(device)) == kDmaAutoIds.end())
      continue;
    if (!test_dma_sysfs_long(dev, "numa_node", dev_numa) || dev_numa != numa) continue;
    std::string drv = "/sys/bus/pci/devices/" + dev + "/driver";
    ssize_t len = readlink(drv.c_str(), link, sizeof(link) - 1);
    if (len < 0) continue;
    link[len] = '\0';
    std::string name(link);
    auto slash = name.find_last_of('/');
    if (((slash == std::string::npos) ? name : name.substr(slash + 1)) == "vfio-pci")
      cnt++;
  }
  return cnt;
}

void expect_bytes_eq(const uint8_t* buf, size_t len, uint8_t val, const char* region) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != val) {
      EXPECT_EQ(buf[i], val) << region << " mismatch at offset " << i;
      return;
    }
  }
}

void run_dma_op_once(mtl_udma_handle dma, mtl_test::ScopedHpBuf& dst,
                     mtl_test::ScopedHpBuf& src, bool fill, uint32_t off, uint32_t len,
                     uint8_t pattern = 0) {
  ASSERT_TRUE(dst);
  ASSERT_TRUE(src);
  ASSERT_NE(dst.iova(), MTL_BAD_IOVA);
  ASSERT_NE(src.iova(), MTL_BAD_IOVA);
  ASSERT_LT(off, len);
  ASSERT_LE(len, dst.size());
  ASSERT_LE(len, src.size());

  const uint8_t sentinel = fill ? static_cast<uint8_t>(pattern ^ 0x55u) : 0xEEu;
  memset(dst.data(), sentinel, dst.size());

  const uint32_t op_len = len - off;
  std::vector<uint8_t> expected;
  int ret;
  if (fill) {
    ret = mtl_udma_fill_u8(dma, dst.iova() + off, pattern, op_len);
  } else {
    uint8_t* src_bytes = src.bytes();
    for (size_t i = 0; i < src.size(); i++) {
      /* Odd values in [0x01, 0x7F] never equal zero or the 0xEE sentinel. */
      src_bytes[i] = static_cast<uint8_t>((((i + off + pattern) * 2u) + 1u) & 0x7Fu);
    }
    expected.assign(src_bytes + off, src_bytes + len);
    ret = mtl_udma_copy(dma, dst.iova() + off, src.iova() + off, op_len);
  }
  ASSERT_GE(ret, 0);
  ASSERT_TRUE(mtl_test::udma_submit_and_wait(dma, 1));

  expect_bytes_eq(dst.bytes(), off, sentinel,
                  fill ? "fill leading canary" : "copy leading canary");
  if (fill) {
    expect_bytes_eq(dst.bytes() + off, op_len, pattern, "fill payload");
  } else {
    EXPECT_EQ(memcmp(dst.bytes() + off, expected.data(), op_len), 0)
        << "DMA copy destination mismatch";
    EXPECT_EQ(memcmp(src.bytes() + off, expected.data(), op_len), 0)
        << "DMA copy source modified";
  }
  expect_bytes_eq(dst.bytes() + len, dst.size() - len, sentinel,
                  fill ? "fill trailing canary" : "copy trailing canary");
}

void test_dma_op(struct st_tests_context* ctx, bool fill, uint32_t off, uint32_t len,
                 uint8_t pattern = 0) {
  mtl_test::ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);
  mtl_test::ScopedHpBuf dst(ctx->handle, static_cast<size_t>(len) + 64u);
  ASSERT_TRUE(dst);
  mtl_test::ScopedHpBuf src(ctx->handle, static_cast<size_t>(len) + 64u);
  ASSERT_TRUE(src);
  run_dma_op_once(dma.get(), dst, src, fill, off, len, pattern);
}

void test_dma_op_sanity(struct st_tests_context* ctx, bool fill) {
  mtl_test::ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  constexpr uint32_t kBufSize = 1024;
  mtl_test::ScopedHpBuf dst(ctx->handle, kBufSize);
  ASSERT_TRUE(dst);
  mtl_test::ScopedHpBuf src(ctx->handle, kBufSize);
  ASSERT_TRUE(src);

  static constexpr uint32_t kBounds[] = {0, 1, 3, 7, 15, 31, 33, 63, 64, 65};
  for (uint32_t b : kBounds) {
    if (b > 0) {
      ASSERT_NO_FATAL_FAILURE(run_dma_op_once(dma.get(), dst, src, fill, 0, b,
                                              static_cast<uint8_t>((b % 255u) + 1u)));
    }
    ASSERT_NO_FATAL_FAILURE(
        run_dma_op_once(dma.get(), dst, src, fill, b, kBufSize,
                        static_cast<uint8_t>(((b * 3u) % 255u) + 1u)));
  }
  ASSERT_NO_FATAL_FAILURE(run_dma_op_once(dma.get(), dst, src, fill, 33, 991, 0xA5));
}

void test_dma_copy_fill_async(struct st_tests_context* ctx, bool fill) {
  mtl_handle st = ctx->handle;
  const uint16_t nb_desc = 1024;
  const int nb_elements = nb_desc * 8;
  const int element_size = 1260;
  const int fb_size = element_size * nb_elements;
  const uint8_t pattern = 0xa5;

  mtl_test::ScopedUdma dma(mtl_udma_create(st, nb_desc, MTL_PORT_P));
  ASSERT_TRUE(dma);

  mtl_test::ScopedHpBuf fb_dst(st, static_cast<size_t>(fb_size));
  ASSERT_TRUE(fb_dst);
  ASSERT_NE(fb_dst.iova(), MTL_BAD_IOVA);
  mtl_test::ScopedHpBuf fb_src(st, static_cast<size_t>(fb_size));
  ASSERT_TRUE(fb_src);
  ASSERT_NE(fb_src.iova(), MTL_BAD_IOVA);

  memset(fb_dst.data(), fill ? static_cast<uint8_t>(pattern ^ 0x55u) : 0xEEu,
         static_cast<size_t>(fb_size));
  if (fill)
    memset(fb_src.data(), pattern, static_cast<size_t>(fb_size));
  else
    st_test_rand_data(fb_src.bytes(), static_cast<size_t>(fb_size), 0);

  unsigned char fb_src_sha[SHA256_DIGEST_LENGTH];
  unsigned char fb_dst_sha[SHA256_DIGEST_LENGTH];
  SHA256(fb_src.bytes(), static_cast<size_t>(fb_size), fb_src_sha);

  /* Bound the async drain loop (10 s covers ~10 MB / 8192 descriptors). */
  const uint64_t deadline =
      st_test_get_monotonic_time() + static_cast<uint64_t>(10000) * NS_PER_MS;
  int dst_off = 0;
  int src_off = 0;
  int first_enqueued = 0;
  bool hit_enospc = false;
  while (dst_off < fb_size) {
    int enqueued = 0;
    while (src_off < fb_size) {
      int ret = fill ? mtl_udma_fill_u8(dma.get(), fb_dst.iova() + src_off, pattern,
                                        static_cast<uint32_t>(element_size))
                     : mtl_udma_copy(dma.get(), fb_dst.iova() + src_off,
                                     fb_src.iova() + src_off,
                                     static_cast<uint32_t>(element_size));
      if (ret < 0) {
        EXPECT_EQ(ret, -ENOSPC);
        hit_enospc = true;
        ASSERT_GT(src_off, dst_off);
        break;
      }
      src_off += element_size;
      enqueued++;
    }
    if (dst_off == 0 && first_enqueued == 0) {
      first_enqueued = enqueued;
      EXPECT_GE(first_enqueued, nb_desc / 2)
          << "uDMA ring accepted far fewer descriptors than requested nb_desc="
          << nb_desc;
    }
    if (enqueued > 0) {
      ASSERT_GE(mtl_udma_submit(dma.get()), 0);
    }
    uint16_t nb_dq = mtl_udma_completed(dma.get(), 32);
    dst_off += element_size * nb_dq;
    if (nb_dq == 0) {
      ASSERT_LT(st_test_get_monotonic_time(), deadline)
          << "DMA async " << (fill ? "fill" : "copy") << " timed out at " << dst_off
          << "/" << fb_size << " bytes";
    }
  }

  EXPECT_TRUE(hit_enospc);
  SHA256(fb_dst.bytes(), static_cast<size_t>(fb_size), fb_dst_sha);
  EXPECT_EQ(memcmp(fb_dst_sha, fb_src_sha, SHA256_DIGEST_LENGTH), 0);
  if (!fill) {
    unsigned char fb_src_post_sha[SHA256_DIGEST_LENGTH];
    SHA256(fb_src.bytes(), static_cast<size_t>(fb_size), fb_src_post_sha);
    EXPECT_EQ(memcmp(fb_src_post_sha, fb_src_sha, SHA256_DIGEST_LENGTH), 0)
        << "DMA async copy source buffer was modified";
  }
}

void assert_dma_mem_valid(const mtl_test::ScopedDmaMem& m, size_t pg_sz = 0) {
  ASSERT_TRUE(m);
  ASSERT_NE(m.addr(), nullptr);
  if (pg_sz) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(m.addr()) % pg_sz, 0u);
  }
  ASSERT_NE(m.iova(), static_cast<mtl_iova_t>(0));
  ASSERT_NE(m.iova(), MTL_BAD_IOVA);
}

void test_dma_map_expect(mtl_handle st, const void* vaddr, size_t size,
                         bool expect_succ) {
  mtl_test::ScopedDmaMap mapping(st, vaddr, size);
  if (expect_succ) {
    EXPECT_TRUE(mapping.valid());
    EXPECT_GE(mapping.unmap(), 0);
  } else {
    EXPECT_EQ(mapping.iova(), MTL_BAD_IOVA);
  }
}

}  // namespace

TEST(Dma, create_max) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_create_max(ctx);
}

TEST(Dma, create_multi) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  for (int i = 0; i < 10; i++) test_dma_create_one(ctx);
}

/* --dma_auto gives each port a DMA device of its NUMA node, if the node has one. */
TEST(Dma, auto_each_port) {
  auto* ctx = st_test_ctx();
  if (!(ctx->para.flags & MTL_FLAG_DMA_AUTO)) GTEST_SKIP() << "requires --dma_auto";
  /* Not MTL_TEST_REQUIRE_DMA: st_test_dma_available is false when the probe
   * found no device, which is the fault this test looks for. */
  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  int checked_ports = 0;
  for (int i = 0; i < ctx->para.num_ports; i++) {
    auto port = static_cast<enum mtl_port>(i);
    int numa = mtl_get_numa_id(ctx->handle, port);
    if (!test_dma_vfio_cnt(numa)) continue;
    checked_ports++;
    mtl_test::ScopedUdma dma(mtl_udma_create(ctx->handle, 128, port));
    EXPECT_TRUE(dma) << "port " << i << " has no DMA device of NUMA " << numa;
  }
  if (checked_ports == 0) {
    GTEST_SKIP() << "no port NUMA node has a vfio-pci Intel DMA device";
  }
}

TEST(Dma, copy) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op(ctx, false, 0, 1024);
  test_dma_op(ctx, false, 128, 1024 * 4);
}

TEST(Dma, copy_sanity) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op_sanity(ctx, false);
}

TEST(Dma, copy_async) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_copy_fill_async(ctx, false);
}

TEST(Dma, fill) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op(ctx, true, 0, 1024, 0xa5);
  test_dma_op(ctx, true, 128, 1024 * 4, 0x5a);
}

TEST(Dma, fill_sanity) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op_sanity(ctx, true);
}

TEST(Dma, fill_async) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_copy_fill_async(ctx, true);
}

TEST(Dma, fill_u64_pattern) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  mtl_handle st = ctx->handle;
  mtl_test::ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  const uint64_t pattern = 0x0123456789ABCDEFULL;
  const uint8_t canary = 0xAAu;
  for (uint32_t len : {8u, 64u, 1024u, 4096u}) {
    const size_t total = static_cast<size_t>(len) + 16u;
    mtl_test::ScopedHpBuf dst(st, total);
    ASSERT_TRUE(dst);
    ASSERT_NE(dst.iova(), MTL_BAD_IOVA);

    std::vector<uint8_t> expected(len);
    for (size_t pos = 0; pos < len; pos += sizeof(pattern)) {
      memcpy(expected.data() + pos, &pattern, sizeof(pattern));
    }

    for (uint32_t off : {0u, 8u}) {
      memset(dst.data(), canary, total);
      ASSERT_GE(mtl_udma_fill(dma.get(), dst.iova() + off, pattern, len), 0);
      ASSERT_TRUE(mtl_test::udma_submit_and_wait(dma.get(), 1));

      expect_bytes_eq(dst.bytes(), off, canary, "u64 fill leading canary");
      EXPECT_EQ(memcmp(expected.data(), dst.bytes() + off, len), 0)
          << "u64 pattern mismatch for length " << len << " offset " << off;
      expect_bytes_eq(dst.bytes() + off + len, total - (off + len), canary,
                      "u64 fill trailing canary");
    }
  }
}

TEST(Dma, map) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  mtl_handle st = ctx->handle;
  const size_t pg_sz = mtl_page_size(st);
  for (size_t pages : {64u, 512u}) {
    const size_t size = pages * pg_sz;
    mtl_test::PageAlignedBuf buf(size, pg_sz);
    ASSERT_TRUE(buf);
    test_dma_map_expect(st, buf.data(), size, true);
  }
}

TEST(Dma, map_fail_and_remap) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  mtl_handle st = ctx->handle;
  const size_t pg_sz = mtl_page_size(st);
  const size_t size = 64 * pg_sz;
  mtl_test::PageAlignedBuf buf(size + pg_sz, pg_sz);
  ASSERT_TRUE(buf);
  uint8_t* align = buf.data() + pg_sz;

  /* Invalid size and unaligned virtual address map calls must fail. */
  test_dma_map_expect(st, align, 0, false);
  test_dma_map_expect(st, align, pg_sz / 2, false);
  test_dma_map_expect(st, align, pg_sz + 1, false);
  test_dma_map_expect(st, align + 1, pg_sz, false);

  mtl_test::ScopedDmaMap mapping(st, align, size);
  ASSERT_TRUE(mapping.valid());
  const mtl_iova_t iova = mapping.iova();

  /* Overlapping and duplicate map calls must fail. */
  EXPECT_EQ(mtl_dma_map(st, align, size), MTL_BAD_IOVA);
  EXPECT_EQ(mtl_dma_map(st, align + pg_sz, size - pg_sz), MTL_BAD_IOVA);
  EXPECT_EQ(mtl_dma_map(st, align - pg_sz, size), MTL_BAD_IOVA);

  /* Invalid, partial, or shifted unmap calls must fail. */
  EXPECT_LT(mtl_dma_unmap(st, align + 1, iova, size), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova, 0), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova, pg_sz / 2), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova + pg_sz, size), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova, size - pg_sz), 0);
  EXPECT_LT(mtl_dma_unmap(st, align + pg_sz, iova + pg_sz, size - pg_sz), 0);

  /* Exact unmap succeeds once, then second unmap fails; re-mapping succeeds. */
  EXPECT_GE(mapping.unmap(), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova, size), 0);

  mtl_test::ScopedDmaMap remapping(st, align, size);
  EXPECT_TRUE(remapping.valid());
  EXPECT_NE(remapping.iova(), static_cast<mtl_iova_t>(0));
  EXPECT_GE(remapping.unmap(), 0);
}

TEST(Dma, map_continues) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  mtl_handle st = ctx->handle;
  size_t pg_sz = mtl_page_size(st);
  const size_t size = 64 * pg_sz;
  const int count = 10;
  mtl_test::PageAlignedBuf buf(size * static_cast<size_t>(count), pg_sz);
  ASSERT_TRUE(buf);

  std::vector<mtl_test::ScopedDmaMap> maps;
  maps.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    maps.emplace_back(st, buf.data() + static_cast<size_t>(i) * size, size);
    EXPECT_TRUE(maps.back().valid());
  }
  for (auto& m : maps) {
    EXPECT_GE(m.unmap(), 0);
  }
}

TEST(Dma, map_copy) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  mtl_handle st = ctx->handle;
  mtl_test::ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  size_t pg_sz = mtl_page_size(st);
  const size_t copy_size = 64 * pg_sz;
  mtl_test::PageAlignedBuf dst(copy_size, pg_sz);
  mtl_test::PageAlignedBuf src(copy_size, pg_sz);
  ASSERT_TRUE(dst);
  ASSERT_TRUE(src);
  st_test_rand_data(src.data(), copy_size, 0);
  memset(dst.data(), 0xEE, copy_size);
  std::vector<uint8_t> expected(src.data(), src.data() + copy_size);

  mtl_test::ScopedDmaMap src_map(st, src.data(), copy_size);
  mtl_test::ScopedDmaMap dst_map(st, dst.data(), copy_size);
  ASSERT_TRUE(src_map.valid());
  ASSERT_TRUE(dst_map.valid());

  ASSERT_GE(mtl_udma_copy(dma.get(), dst_map.iova(), src_map.iova(),
                          static_cast<uint32_t>(copy_size)),
            0);
  ASSERT_TRUE(mtl_test::udma_submit_and_wait(dma.get(), 1));

  EXPECT_EQ(memcmp(dst.data(), expected.data(), copy_size), 0)
      << "DMA map copy destination mismatch";
  EXPECT_EQ(memcmp(src.data(), expected.data(), copy_size), 0)
      << "DMA map copy source modified";
  EXPECT_GE(src_map.unmap(), 0);
  EXPECT_GE(dst_map.unmap(), 0);
}

TEST(Dma, mem_alloc_free) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  mtl_handle st = ctx->handle;
  const size_t pg_sz = mtl_page_size(st);
  ASSERT_GE(pg_sz, 4096u);
  ASSERT_EQ(pg_sz & (pg_sz - 1u), 0u);
  for (size_t sz : {111u, 2222u, 33333u, 444444u}) {
    mtl_test::ScopedDmaMem dma_mem(st, sz);
    ASSERT_NO_FATAL_FAILURE(assert_dma_mem_valid(dma_mem, pg_sz));
    auto* bytes = static_cast<uint8_t*>(dma_mem.addr());
    expect_bytes_eq(bytes, sz, 0u, "dma_mem zero-init");
    bytes[0] = 0x5Au;
    bytes[sz - 1] = 0xA5u;
    EXPECT_EQ(bytes[0], 0x5Au);
    EXPECT_EQ(bytes[sz - 1], 0xA5u);
  }
  EXPECT_EQ(mtl_test::ScopedDmaMem(st, 0).get(), nullptr);

  /* Loop past MT_MAP_MAX_ITEMS (256) to catch no-op mtl_dma_mem_free leaks. */
  for (int i = 0; i < 300; i++) {
    mtl_test::ScopedDmaMem dma_mem(st, pg_sz);
    ASSERT_TRUE(dma_mem) << "dma_mem alloc failed at iteration " << i;
    ASSERT_NO_FATAL_FAILURE(assert_dma_mem_valid(dma_mem, pg_sz));
  }
}

TEST(Dma, mem_alloc_copy) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  mtl_handle st = ctx->handle;
  mtl_test::ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  for (uint32_t copy_size : {1024u, 65536u}) {
    mtl_test::ScopedDmaMem src(st, copy_size), dst(st, copy_size);
    ASSERT_NO_FATAL_FAILURE(assert_dma_mem_valid(src));
    ASSERT_NO_FATAL_FAILURE(assert_dma_mem_valid(dst));

    auto* src_bytes = static_cast<uint8_t*>(src.addr());
    auto* dst_bytes = static_cast<uint8_t*>(dst.addr());
    st_test_rand_data(src_bytes, copy_size, 0);
    memset(dst_bytes, 0xEE, copy_size);
    std::vector<uint8_t> expected(src_bytes, src_bytes + copy_size);

    ASSERT_GE(mtl_udma_copy(dma.get(), dst.iova(), src.iova(), copy_size), 0);
    ASSERT_TRUE(mtl_test::udma_submit_and_wait(dma.get(), 1));
    EXPECT_EQ(memcmp(dst_bytes, expected.data(), copy_size), 0)
        << "DMA mem copy destination mismatch for size " << copy_size;
    EXPECT_EQ(memcmp(src_bytes, expected.data(), copy_size), 0)
        << "DMA mem copy source modified for size " << copy_size;
  }
}

TEST(Dma, create_default_desc_and_completed_edges) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  mtl_handle st = ctx->handle;
  mtl_test::ScopedUdma dma(mtl_udma_create(st, 0, MTL_PORT_P));
  ASSERT_TRUE(dma);
  EXPECT_EQ(mtl_udma_completed(dma.get(), 32), 0u);

  mtl_test::ScopedHpBuf dst(st, 1024);
  mtl_test::ScopedHpBuf src(st, 1024);
  ASSERT_NO_FATAL_FAILURE(run_dma_op_once(dma.get(), dst, src, true, 0, 1024, 0x5Au));
  EXPECT_EQ(mtl_udma_completed(dma.get(), 32), 0u);
}

TEST(Dma, hp_virt2iova_and_page_size) {
  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;

  const size_t pg = mtl_page_size(st);
  EXPECT_GT(pg, 0u);
#ifndef WINDOWSENV
  EXPECT_EQ(pg, static_cast<size_t>(sysconf(_SC_PAGESIZE)));
#endif

  constexpr size_t kAllocSize = 4096;
  /* Poison a hugepage block, free it, then verify mtl_hp_zmalloc zeroes memory. */
  {
    mtl_test::ScopedHpBuf poisoned(st, kAllocSize, MTL_PORT_P, false);
    ASSERT_TRUE(poisoned);
    memset(poisoned.data(), 0xA5, kAllocSize);
  }

  mtl_test::ScopedHpBuf hp_zero(st, kAllocSize, MTL_PORT_P, true);
  ASSERT_TRUE(hp_zero);
  expect_bytes_eq(hp_zero.bytes(), kAllocSize, 0u, "hp_zmalloc zero-init");

  const mtl_iova_t iova = mtl_hp_virt2iova(st, hp_zero.data());
  EXPECT_NE(iova, static_cast<mtl_iova_t>(0));
  EXPECT_NE(iova, MTL_BAD_IOVA);

  /* Second distinct hugepage allocation must produce a distinct valid IOVA. */
  mtl_test::ScopedHpBuf hp_second(st, kAllocSize, MTL_PORT_P, false);
  ASSERT_TRUE(hp_second);
  const mtl_iova_t iova2 = mtl_hp_virt2iova(st, hp_second.data());
  EXPECT_NE(iova2, static_cast<mtl_iova_t>(0));
  EXPECT_NE(iova2, MTL_BAD_IOVA);
  EXPECT_NE(iova2, iova);

  /* Only NULL is safe as an invalid pointer to rte_malloc_virt2iova. */
  EXPECT_EQ(mtl_hp_virt2iova(st, nullptr), MTL_BAD_IOVA);

  /* Out-of-range port must be rejected by hugepage allocators. */
  const auto bad_port = static_cast<enum mtl_port>(MTL_PORT_MAX);
  EXPECT_FALSE(mtl_test::ScopedHpBuf(st, kAllocSize, bad_port, false));
  EXPECT_FALSE(mtl_test::ScopedHpBuf(st, kAllocSize, bad_port, true));
}
