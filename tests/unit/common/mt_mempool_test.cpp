/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_mempool_create_by_ops(): the pool header (struct rte_mempool and the per-lcore
 * caches every mbuf alloc and free reads) sits where the allocator puts a memzone of its
 * length, so pools with a per-lcore cache must still differ in that length to use
 * different cache sets.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtMempoolTest.*'
 */

#include <gtest/gtest.h>
#include <rte_errno.h>
#include <rte_mbuf.h>

#include <set>
#include <string>
#include <vector>

#include "common/ut_common.h"

extern "C" {
struct mtl_main_impl;
struct mtl_main_impl* ut_mempool_impl_create(void);
void ut_mempool_impl_destroy(struct mtl_main_impl* impl);
struct rte_mempool* ut_mempool_create(struct mtl_main_impl* impl, const char* name,
                                      unsigned int n, unsigned int cache_size,
                                      uint16_t priv_size, uint16_t element_size);
int ut_mempool_free(struct rte_mempool* mp);
}

namespace {

constexpr unsigned int kCacheLine = 64;
constexpr unsigned int kPage = 4096;
constexpr unsigned int kLinesPerPage = kPage / kCacheLine;
constexpr unsigned int kCacheSize = 128; /* MT_MBUF_CACHE_SIZE */
constexpr uint16_t kPrivSize = 32;       /* sizeof(struct mt_muf_priv_data) */
constexpr uint16_t kElementSize = 1280;  /* a multiple of kCacheSize, kept as is */

unsigned int line_in_page(const void* p) {
  return ((uintptr_t)p % kPage) / kCacheLine;
}

class MtMempoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0);
    impl_ = ut_mempool_impl_create();
    ASSERT_NE(impl_, nullptr);
  }
  void TearDown() override {
    for (struct rte_mempool* mp : pools_) ut_mempool_free(mp);
    ut_mempool_impl_destroy(impl_);
  }
  struct rte_mempool* create(uint16_t priv_size) {
    struct rte_mempool* mp =
        ut_mempool_create(impl_, "ut_color", 1023, kCacheSize, priv_size, kElementSize);
    if (mp) pools_.push_back(mp);
    return mp;
  }
  struct mtl_main_impl* impl_ = nullptr;
  std::vector<struct rte_mempool*> pools_;
};

/* each pool is freed before the next, so every header is placed against the same free
 * memory boundary, as on a freshly mapped hugepage */
TEST_F(MtMempoolTest, CachedPoolsInTurnTakeEveryCacheLineOfAPage) {
  std::set<unsigned int> lines;
  for (unsigned int i = 0; i < kLinesPerPage; i++) {
    struct rte_mempool* mp =
        ut_mempool_create(impl_, "ut_color", 1023, kCacheSize, kPrivSize, kElementSize);
    ASSERT_NE(mp, nullptr);
    lines.insert(line_in_page(mp));
    std::string name = mp->name;
    ut_mempool_free(mp);
    ASSERT_EQ(rte_mempool_lookup(name.c_str()), nullptr);
  }
  EXPECT_EQ(lines.size(), kLinesPerPage);
}

TEST_F(MtMempoolTest, EveryMbufHasTheAskedPrivAndDataRoom) {
  const uint16_t data_room = kElementSize + RTE_PKTMBUF_HEADROOM;
  struct rte_mempool* mp = create(kPrivSize);
  ASSERT_NE(mp, nullptr);
  EXPECT_EQ(rte_pktmbuf_priv_size(mp), kPrivSize);
  EXPECT_EQ(rte_pktmbuf_data_room_size(mp), data_room);

  std::vector<struct rte_mbuf*> mbufs(mp->size);
  ASSERT_EQ(rte_pktmbuf_alloc_bulk(mp, mbufs.data(), mp->size), 0);
  for (struct rte_mbuf* m : mbufs) {
    EXPECT_EQ(m->priv_size, kPrivSize);
    EXPECT_EQ(m->buf_len, data_room);
    EXPECT_EQ(rte_pktmbuf_headroom(m), RTE_PKTMBUF_HEADROOM);
  }
  rte_pktmbuf_free_bulk(mbufs.data(), mbufs.size());
  EXPECT_EQ(rte_mempool_avail_count(mp), mp->size);
}

TEST_F(MtMempoolTest, AnUnalignedPrivSizeIsRefused) {
  EXPECT_EQ(create(RTE_MBUF_PRIV_ALIGN / 2), nullptr);
  EXPECT_EQ(rte_errno, EINVAL);
}

}  // namespace
