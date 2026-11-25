/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <dirent.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

namespace {

using ScopedUdma = mtl_test::ScopedHandle<mtl_udma_handle, mtl_udma_free>;

/* RAII guard for hugepage memory allocated via mtl_hp_malloc(). */
class ScopedHpBuf {
 public:
  ScopedHpBuf(mtl_handle st, size_t size, enum mtl_port port = MTL_PORT_P)
      : st_(st), ptr_(mtl_hp_malloc(st, size, port)) {
    if (ptr_) iova_ = mtl_hp_virt2iova(st_, ptr_);
  }
  ~ScopedHpBuf() {
    if (ptr_) mtl_hp_free(st_, ptr_);
  }
  ScopedHpBuf(const ScopedHpBuf&) = delete;
  ScopedHpBuf& operator=(const ScopedHpBuf&) = delete;

  void* data() const {
    return ptr_;
  }
  uint8_t* bytes() const {
    return static_cast<uint8_t*>(ptr_);
  }
  mtl_iova_t iova() const {
    return iova_;
  }
  explicit operator bool() const {
    return ptr_ != nullptr;
  }

 private:
  mtl_handle st_ = nullptr;
  void* ptr_ = nullptr;
  mtl_iova_t iova_ = MTL_BAD_IOVA;
};

/* RAII page-aligned heap buffer with 2 guard pages (head and tail). */
class AlignedHeapBuf {
 public:
  AlignedHeapBuf(size_t usable_size, size_t pg_sz)
      : raw_(static_cast<uint8_t*>(malloc(usable_size + 2 * pg_sz)), &free) {
    if (raw_) {
      aligned_ = reinterpret_cast<uint8_t*>(
          MTL_ALIGN(reinterpret_cast<uint64_t>(raw_.get()), pg_sz));
    }
  }

  uint8_t* data() const {
    return aligned_;
  }
  explicit operator bool() const {
    return aligned_ != nullptr;
  }

 private:
  std::unique_ptr<uint8_t, decltype(&free)> raw_{nullptr, &free};
  uint8_t* aligned_ = nullptr;
};

/* RAII guard for mtl_dma_map() / mtl_dma_unmap(). */
class ScopedDmaMap {
 public:
  ScopedDmaMap() = default;
  ScopedDmaMap(mtl_handle st, const void* vaddr, size_t size)
      : st_(st), vaddr_(vaddr), size_(size), iova_(mtl_dma_map(st, vaddr, size)) {
  }
  ~ScopedDmaMap() {
    if (valid()) {
      EXPECT_GE(unmap(), 0);
    }
  }
  ScopedDmaMap(const ScopedDmaMap&) = delete;
  ScopedDmaMap& operator=(const ScopedDmaMap&) = delete;
  ScopedDmaMap(ScopedDmaMap&& other) noexcept
      : st_(other.st_), vaddr_(other.vaddr_), size_(other.size_), iova_(other.iova_) {
    other.iova_ = MTL_BAD_IOVA;
  }
  ScopedDmaMap& operator=(ScopedDmaMap&& other) noexcept {
    if (this != &other) {
      if (valid()) {
        EXPECT_GE(unmap(), 0);
      }
      st_ = other.st_;
      vaddr_ = other.vaddr_;
      size_ = other.size_;
      iova_ = other.iova_;
      other.iova_ = MTL_BAD_IOVA;
    }
    return *this;
  }

  mtl_iova_t iova() const {
    return iova_;
  }
  bool valid() const {
    return iova_ != MTL_BAD_IOVA;
  }

  int unmap() {
    if (!valid()) return 0;
    mtl_iova_t iova = iova_;
    iova_ = MTL_BAD_IOVA;
    return mtl_dma_unmap(st_, vaddr_, iova, size_);
  }

 private:
  mtl_handle st_ = nullptr;
  const void* vaddr_ = nullptr;
  size_t size_ = 0;
  mtl_iova_t iova_ = MTL_BAD_IOVA;
};

/* RAII guard for mtl_dma_mem_alloc() / mtl_dma_mem_free(). */
class ScopedDmaMem {
 public:
  ScopedDmaMem(mtl_handle st, size_t size)
      : st_(st), handle_(mtl_dma_mem_alloc(st, size)) {
  }
  ~ScopedDmaMem() {
    if (handle_) mtl_dma_mem_free(st_, handle_);
  }
  ScopedDmaMem(const ScopedDmaMem&) = delete;
  ScopedDmaMem& operator=(const ScopedDmaMem&) = delete;

  mtl_dma_mem_handle get() const {
    return handle_;
  }
  explicit operator bool() const {
    return handle_ != nullptr;
  }

 private:
  mtl_handle st_ = nullptr;
  mtl_dma_mem_handle handle_ = nullptr;
};

int test_dma_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.dma_dev_cnt;
}

void test_dma_create_one(struct st_tests_context* ctx) {
  int base_cnt = test_dma_cnt(ctx);
  ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);
  EXPECT_EQ(base_cnt + 1, test_dma_cnt(ctx));
  dma.reset();
  EXPECT_EQ(base_cnt, test_dma_cnt(ctx));
}

void test_dma_create_max(struct st_tests_context* ctx) {
  int base_cnt = test_dma_cnt(ctx);
  std::vector<ScopedUdma> dmas;
  dmas.reserve(MTL_DMA_DEV_MAX);

  while (dmas.size() < MTL_DMA_DEV_MAX) {
    ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
    if (!dma) break;
    dmas.push_back(std::move(dma));
    EXPECT_EQ(base_cnt + static_cast<int>(dmas.size()), test_dma_cnt(ctx));
  }

  for (int remain = static_cast<int>(dmas.size()) - 1; remain >= 0; remain--) {
    dmas.pop_back();
    EXPECT_EQ(base_cnt + remain, test_dma_cnt(ctx));
  }
}

std::string test_dma_sysfs(const std::string& dev, const char* attr) {
  std::ifstream file("/sys/bus/pci/devices/" + dev + "/" + attr);
  std::string value;
  file >> value;
  return value;
}

struct DirCloser {
  void operator()(DIR* d) const {
    if (d) closedir(d);
  }
};

/* Intel DMA devices (PCI class 0x0880) on vfio-pci on NUMA node numa. */
int test_dma_vfio_cnt(int numa) {
  std::unique_ptr<DIR, DirCloser> dir(opendir("/sys/bus/pci/devices"));
  if (!dir) return 0;

  char link[256];
  int cnt = 0;
  struct dirent* ent;
  while ((ent = readdir(dir.get()))) {
    std::string dev = ent->d_name;
    if (test_dma_sysfs(dev, "vendor") != "0x8086") continue;
    if (test_dma_sysfs(dev, "class").rfind("0x0880", 0) != 0) continue;
    if (test_dma_sysfs(dev, "numa_node") != std::to_string(numa)) continue;
    std::string driver = "/sys/bus/pci/devices/" + dev + "/driver";
    ssize_t len = readlink(driver.c_str(), link, sizeof(link) - 1);
    if (len < 0) continue;
    link[len] = '\0';
    std::string name(link);
    if (name.substr(name.find_last_of('/') + 1) == "vfio-pci") cnt++;
  }
  return cnt;
}

void run_dma_op_once(mtl_handle st, mtl_udma_handle dma, bool fill, uint32_t off,
                     uint32_t len, uint8_t pattern = 0) {
  ScopedHpBuf dst(st, len);
  ASSERT_TRUE(dst);
  ScopedHpBuf src(st, len);
  ASSERT_TRUE(src);

  int ret;
  if (fill) {
    memset(src.data(), pattern, len);
    ret = mtl_udma_fill_u8(dma, dst.iova() + off, pattern, len - off);
  } else {
    st_test_rand_data(src.bytes(), len, 0);
    ret = mtl_udma_copy(dma, dst.iova() + off, src.iova() + off, len - off);
  }
  ASSERT_GE(ret, 0);
  (void)mtl_udma_submit(dma);

  uint16_t nb_dq = 0;
  while (nb_dq < 1) {
    nb_dq = mtl_udma_completed(dma, 32);
  }

  EXPECT_EQ(memcmp(src.bytes() + off, dst.bytes() + off, len - off), 0);
}

void test_dma_op(struct st_tests_context* ctx, bool fill, uint32_t off, uint32_t len,
                 uint8_t pattern = 0) {
  ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);
  run_dma_op_once(ctx->handle, dma.get(), fill, off, len, pattern);
}

void test_dma_op_sanity(struct st_tests_context* ctx, bool fill) {
  ScopedUdma dma(mtl_udma_create(ctx->handle, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  for (int len = 1; len < 1024; len += 7) {
    run_dma_op_once(ctx->handle, dma.get(), fill, 0, static_cast<uint32_t>(len),
                    static_cast<uint8_t>(rand()));
  }
  for (int off = 1; off < 1024; off += 7) {
    run_dma_op_once(ctx->handle, dma.get(), fill, static_cast<uint32_t>(off), 1024,
                    static_cast<uint8_t>(rand()));
  }
}

void test_dma_copy_fill_async(struct st_tests_context* ctx, bool fill) {
  mtl_handle st = ctx->handle;
  const uint16_t nb_desc = 1024;
  const int nb_elements = nb_desc * 8;
  const int element_size = 1260;
  const int fb_size = element_size * nb_elements;
  const uint8_t pattern = 0xa5;

  ScopedUdma dma(mtl_udma_create(st, nb_desc, MTL_PORT_P));
  ASSERT_TRUE(dma);

  ScopedHpBuf fb_dst(st, static_cast<size_t>(fb_size));
  ASSERT_TRUE(fb_dst);
  ScopedHpBuf fb_src(st, static_cast<size_t>(fb_size));
  ASSERT_TRUE(fb_src);

  if (fill)
    memset(fb_src.data(), pattern, static_cast<size_t>(fb_size));
  else
    st_test_rand_data(fb_src.bytes(), static_cast<size_t>(fb_size), 0);

  unsigned char fb_src_sha[SHA256_DIGEST_LENGTH];
  unsigned char fb_dst_sha[SHA256_DIGEST_LENGTH];
  SHA256(fb_src.bytes(), static_cast<size_t>(fb_size), fb_src_sha);

  int dst_off = 0;
  int src_off = 0;
  while (dst_off < fb_size) {
    while (src_off < fb_size) {
      int ret = fill ? mtl_udma_fill_u8(dma.get(), fb_dst.iova() + src_off, pattern,
                                        static_cast<uint32_t>(element_size))
                     : mtl_udma_copy(dma.get(), fb_dst.iova() + src_off,
                                     fb_src.iova() + src_off,
                                     static_cast<uint32_t>(element_size));
      if (ret < 0) break;
      src_off += element_size;
    }
    (void)mtl_udma_submit(dma.get());
    uint16_t nb_dq = mtl_udma_completed(dma.get(), 32);
    dst_off += element_size * nb_dq;
  }

  SHA256(fb_dst.bytes(), static_cast<size_t>(fb_size), fb_dst_sha);
  EXPECT_EQ(memcmp(fb_dst_sha, fb_src_sha, SHA256_DIGEST_LENGTH), 0);
}

void test_dma_map_expect(mtl_handle st, const void* vaddr, size_t size,
                         bool expect_succ) {
  ScopedDmaMap mapping(st, vaddr, size);
  if (expect_succ) {
    EXPECT_TRUE(mapping.valid());
    EXPECT_GE(mapping.unmap(), 0);
  } else {
    EXPECT_FALSE(mapping.valid());
  }
}

void test_dma_map_pages(struct st_tests_context* ctx, size_t pages) {
  mtl_handle st = ctx->handle;
  size_t pg_sz = mtl_page_size(st);
  size_t size = pages * pg_sz;
  AlignedHeapBuf buf(size, pg_sz);
  ASSERT_TRUE(buf);
  test_dma_map_expect(st, buf.data(), size, true);
}

void test_dma_mem_alloc_free(struct st_tests_context* ctx, size_t size) {
  ScopedDmaMem dma_mem(ctx->handle, size);
  ASSERT_TRUE(dma_mem);
  ASSERT_TRUE(mtl_dma_mem_addr(dma_mem.get()) != nullptr);
  ASSERT_TRUE(mtl_dma_mem_iova(dma_mem.get()) != 0 &&
              mtl_dma_mem_iova(dma_mem.get()) != MTL_BAD_IOVA);
}

}  // namespace

TEST(Dma, create) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_create_one(ctx);
}

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

  for (int i = 0; i < ctx->para.num_ports; i++) {
    auto port = static_cast<enum mtl_port>(i);
    int numa = mtl_get_numa_id(ctx->handle, port);
    if (!test_dma_vfio_cnt(numa)) continue;
    ScopedUdma dma(mtl_udma_create(ctx->handle, 128, port));
    EXPECT_TRUE(dma) << "port " << i << " has no DMA device of NUMA " << numa;
  }
}

TEST(Dma, copy) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op(ctx, false, 0, 1024);
  test_dma_op(ctx, false, 128, 1024 * 4);
}

TEST(Dma, copy_odd) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op(ctx, false, 33, 1024);
  test_dma_op(ctx, false, 33, 1024 - 33);
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

TEST(Dma, fill_odd) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);
  test_dma_op(ctx, true, 33, 1024, 0x5a);
  test_dma_op(ctx, true, 33, 1024 - 33, 0xa5);
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

TEST(Dma, map) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  test_dma_map_pages(ctx, 64);
  test_dma_map_pages(ctx, 512);
}

TEST(Dma, map_fail) {
  auto* ctx = st_test_ctx();
  mtl_handle st = ctx->handle;
  size_t pg_sz = mtl_page_size(st);
  std::unique_ptr<uint8_t, decltype(&free)> buf(static_cast<uint8_t*>(malloc(pg_sz / 2)),
                                                &free);
  ASSERT_TRUE(buf != nullptr);
  test_dma_map_expect(st, buf.get(), pg_sz / 2, false);
}

TEST(Dma, map_continues) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  mtl_handle st = ctx->handle;
  size_t pg_sz = mtl_page_size(st);
  const size_t size = 64 * pg_sz;
  const int count = 10;
  AlignedHeapBuf buf(size * static_cast<size_t>(count), pg_sz);
  ASSERT_TRUE(buf);

  std::vector<ScopedDmaMap> maps;
  maps.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    maps.emplace_back(st, buf.data() + static_cast<size_t>(i) * size, size);
    EXPECT_TRUE(maps.back().valid());
  }
  for (auto& m : maps) {
    EXPECT_GE(m.unmap(), 0);
  }
}

TEST(Dma, map_remap) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  mtl_handle st = ctx->handle;
  size_t pg_sz = mtl_page_size(st);
  const size_t size = 64 * pg_sz;
  AlignedHeapBuf buf(size, pg_sz);
  ASSERT_TRUE(buf);

  uint8_t* align = buf.data();
  ScopedDmaMap mapping(st, align, size);
  EXPECT_TRUE(mapping.valid());
  const mtl_iova_t iova = mapping.iova();

  /* Overlapping and duplicate map calls must fail. */
  EXPECT_EQ(mtl_dma_map(st, align, size), MTL_BAD_IOVA);
  EXPECT_EQ(mtl_dma_map(st, align + pg_sz, size - pg_sz), MTL_BAD_IOVA);
  EXPECT_EQ(mtl_dma_map(st, align - pg_sz, size), MTL_BAD_IOVA);

  /* Partial or shifted unmap calls must fail. */
  EXPECT_LT(mtl_dma_unmap(st, align, iova, size - pg_sz), 0);
  EXPECT_LT(mtl_dma_unmap(st, align + pg_sz, iova + pg_sz, size - pg_sz), 0);

  /* Exact unmap succeeds once, then second unmap fails. */
  EXPECT_GE(mapping.unmap(), 0);
  EXPECT_LT(mtl_dma_unmap(st, align, iova, size), 0);
}

TEST(Dma, map_copy) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_DMA(ctx);

  mtl_handle st = ctx->handle;
  ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  ASSERT_TRUE(dma);

  size_t pg_sz = mtl_page_size(st);
  const size_t copy_size = 64 * pg_sz;
  AlignedHeapBuf dst(copy_size, pg_sz);
  AlignedHeapBuf src(copy_size, pg_sz);
  ASSERT_TRUE(dst);
  ASSERT_TRUE(src);
  st_test_rand_data(src.data(), copy_size, 0);

  ScopedDmaMap src_map(st, src.data(), copy_size);
  ScopedDmaMap dst_map(st, dst.data(), copy_size);
  ASSERT_TRUE(src_map.valid());
  ASSERT_TRUE(dst_map.valid());

  ASSERT_GE(mtl_udma_copy(dma.get(), dst_map.iova(), src_map.iova(),
                          static_cast<uint32_t>(copy_size)),
            0);
  (void)mtl_udma_submit(dma.get());
  uint16_t nb_dq = 0;
  while (nb_dq < 1) {
    nb_dq = mtl_udma_completed(dma.get(), 32);
  }

  EXPECT_EQ(memcmp(src.data(), dst.data(), copy_size), 0);
  EXPECT_GE(src_map.unmap(), 0);
  EXPECT_GE(dst_map.unmap(), 0);
}

TEST(Dma, mem_alloc_free) {
  auto* ctx = st_test_ctx();
  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  for (size_t sz : {111u, 2222u, 33333u, 444444u}) {
    test_dma_mem_alloc_free(ctx, sz);
  }
}
