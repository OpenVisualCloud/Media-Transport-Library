/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "harness/sessions.hpp"

namespace mtl_test {

using ScopedUdma = ScopedHandle<mtl_udma_handle, mtl_udma_free>;

/* Submit queued uDMA operations and wait for expected completions with a deadline. */
inline bool udma_submit_and_wait(mtl_udma_handle dma, uint16_t expected,
                                 int timeout_ms = 5000) {
  if (!dma) return false;
  if (mtl_udma_submit(dma) < 0) return false;
  uint16_t done = 0;
  const int clamped_timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
  const uint64_t deadline = st_test_get_monotonic_time() +
                            static_cast<uint64_t>(clamped_timeout_ms) * NS_PER_MS;
  while (done < expected) {
    done += mtl_udma_completed(dma, static_cast<uint16_t>(expected - done));
    if (done >= expected) return true;
    if (st_test_get_monotonic_time() >= deadline) return false;
  }
  return true;
}

/* Move-only RAII guard for hugepage memory allocated via mtl_hp_malloc/zmalloc. */
class ScopedHpBuf {
 public:
  ScopedHpBuf() = default;
  ScopedHpBuf(mtl_handle st, size_t size, enum mtl_port port = MTL_PORT_P,
              bool zero = false)
      : st_(st),
        size_(size),
        ptr_(st ? (zero ? mtl_hp_zmalloc(st, size, port) : mtl_hp_malloc(st, size, port))
                : nullptr) {
    if (ptr_) {
      iova_ = mtl_hp_virt2iova(st_, ptr_);
    } else {
      size_ = 0;
    }
  }
  ~ScopedHpBuf() {
    reset();
  }
  ScopedHpBuf(const ScopedHpBuf&) = delete;
  ScopedHpBuf& operator=(const ScopedHpBuf&) = delete;
  ScopedHpBuf(ScopedHpBuf&& other) noexcept
      : st_(other.st_), size_(other.size_), ptr_(other.ptr_), iova_(other.iova_) {
    other.st_ = nullptr;
    other.size_ = 0;
    other.ptr_ = nullptr;
    other.iova_ = MTL_BAD_IOVA;
  }
  ScopedHpBuf& operator=(ScopedHpBuf&& other) noexcept {
    if (this != &other) {
      reset();
      st_ = other.st_;
      size_ = other.size_;
      ptr_ = other.ptr_;
      iova_ = other.iova_;
      other.st_ = nullptr;
      other.size_ = 0;
      other.ptr_ = nullptr;
      other.iova_ = MTL_BAD_IOVA;
    }
    return *this;
  }

  void reset() {
    if (ptr_ && st_) mtl_hp_free(st_, ptr_);
    st_ = nullptr;
    size_ = 0;
    ptr_ = nullptr;
    iova_ = MTL_BAD_IOVA;
  }

  void* data() const {
    return ptr_;
  }
  uint8_t* bytes() const {
    return static_cast<uint8_t*>(ptr_);
  }
  size_t size() const {
    return size_;
  }
  mtl_iova_t iova() const {
    return iova_;
  }
  explicit operator bool() const {
    return ptr_ != nullptr;
  }

 private:
  mtl_handle st_ = nullptr;
  size_t size_ = 0;
  void* ptr_ = nullptr;
  mtl_iova_t iova_ = MTL_BAD_IOVA;
};

/* RAII page-aligned heap buffer with 2 guard pages (head and tail). */
class PageAlignedBuf {
 public:
  PageAlignedBuf() = default;
  PageAlignedBuf(size_t usable_size, size_t pg_sz) {
    if (pg_sz == 0 || (pg_sz & (pg_sz - 1)) != 0 ||
        pg_sz > (std::numeric_limits<size_t>::max() - usable_size) / 3) {
      return;
    }
    raw_.reset(static_cast<uint8_t*>(malloc(usable_size + 3 * pg_sz)));
    if (raw_) {
      aligned_ = reinterpret_cast<uint8_t*>(
          MTL_ALIGN(reinterpret_cast<uint64_t>(raw_.get()) + pg_sz, pg_sz));
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

/* Move-only RAII guard for mtl_dma_map() / mtl_dma_unmap(). */
class ScopedDmaMap {
 public:
  ScopedDmaMap() = default;
  ScopedDmaMap(mtl_handle st, const void* vaddr, size_t size)
      : st_(st),
        vaddr_(vaddr),
        size_(size),
        iova_(st ? mtl_dma_map(st, vaddr, size) : MTL_BAD_IOVA) {
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
    other.st_ = nullptr;
    other.vaddr_ = nullptr;
    other.size_ = 0;
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
      other.st_ = nullptr;
      other.vaddr_ = nullptr;
      other.size_ = 0;
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
    if (!valid() || !st_) {
      iova_ = MTL_BAD_IOVA;
      return 0;
    }
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

/* Move-only RAII guard for mtl_dma_mem_alloc() / mtl_dma_mem_free(). */
class ScopedDmaMem {
 public:
  ScopedDmaMem() = default;
  ScopedDmaMem(mtl_handle st, size_t size)
      : st_(st), handle_(st ? mtl_dma_mem_alloc(st, size) : nullptr) {
  }
  ~ScopedDmaMem() {
    reset();
  }
  ScopedDmaMem(const ScopedDmaMem&) = delete;
  ScopedDmaMem& operator=(const ScopedDmaMem&) = delete;
  ScopedDmaMem(ScopedDmaMem&& other) noexcept : st_(other.st_), handle_(other.handle_) {
    other.st_ = nullptr;
    other.handle_ = nullptr;
  }
  ScopedDmaMem& operator=(ScopedDmaMem&& other) noexcept {
    if (this != &other) {
      reset();
      st_ = other.st_;
      handle_ = other.handle_;
      other.st_ = nullptr;
      other.handle_ = nullptr;
    }
    return *this;
  }

  void reset() {
    if (handle_ && st_) mtl_dma_mem_free(st_, handle_);
    st_ = nullptr;
    handle_ = nullptr;
  }

  mtl_dma_mem_handle get() const {
    return handle_;
  }
  void* addr() const {
    return handle_ ? mtl_dma_mem_addr(handle_) : nullptr;
  }
  mtl_iova_t iova() const {
    return handle_ ? mtl_dma_mem_iova(handle_) : MTL_BAD_IOVA;
  }
  explicit operator bool() const {
    return handle_ != nullptr;
  }

 private:
  mtl_handle st_ = nullptr;
  mtl_dma_mem_handle handle_ = nullptr;
};

/*
 * Move-only RAII owner for page-aligned, DMA-mapped external frame buffers
 * with per-slot atomic in-use tracking for dynamic external frame callbacks.
 */
class ScopedExtFrames {
 public:
  ScopedExtFrames() = default;
  ~ScopedExtFrames() {
    reset();
  }
  ScopedExtFrames(const ScopedExtFrames&) = delete;
  ScopedExtFrames& operator=(const ScopedExtFrames&) = delete;
  ScopedExtFrames(ScopedExtFrames&& other) noexcept
      : st_(other.st_),
        raw_(other.raw_),
        aligned_(other.aligned_),
        iova_(other.iova_),
        map_sz_(other.map_sz_),
        frame_size_(other.frame_size_),
        frames_(std::move(other.frames_)),
        in_use_(std::move(other.in_use_)) {
    other.st_ = nullptr;
    other.raw_ = nullptr;
    other.aligned_ = nullptr;
    other.iova_ = MTL_BAD_IOVA;
    other.map_sz_ = 0;
    other.frame_size_ = 0;
  }
  ScopedExtFrames& operator=(ScopedExtFrames&& other) noexcept {
    if (this != &other) {
      reset();
      st_ = other.st_;
      raw_ = other.raw_;
      aligned_ = other.aligned_;
      iova_ = other.iova_;
      map_sz_ = other.map_sz_;
      frame_size_ = other.frame_size_;
      frames_ = std::move(other.frames_);
      in_use_ = std::move(other.in_use_);
      other.st_ = nullptr;
      other.raw_ = nullptr;
      other.aligned_ = nullptr;
      other.iova_ = MTL_BAD_IOVA;
      other.map_sz_ = 0;
      other.frame_size_ = 0;
    }
    return *this;
  }

  /* Allocate multi-plane video external frames for ST 20p / ST 22p. */
  bool init(mtl_handle st, enum st_frame_fmt fmt, int width, int height, bool interlaced,
            int fb_cnt, size_t line_padding_size = 0) {
    reset();
    if (!st || width <= 0 || height <= 0 || fb_cnt <= 0) return false;
    if (interlaced && height < 2) return false;
    const uint8_t planes = st_frame_fmt_planes(fmt);
    if (planes == 0) return false;
    const size_t base_fsize = st_frame_size(fmt, width, height, interlaced);
    if (base_fsize == 0) return false;
    const size_t plane_height = static_cast<size_t>(interlaced ? height / 2 : height);
    const size_t fsize = base_fsize + line_padding_size * plane_height * planes;
    if (!alloc_backing(st, fsize, fb_cnt)) return false;

    for (int j = 0; j < fb_cnt; j++) {
      auto& ef = frames_[static_cast<size_t>(j)];
      for (uint8_t plane = 0; plane < planes; plane++) {
        ef.linesize[plane] =
            st_frame_least_linesize(fmt, width, plane) + line_padding_size;
        if (plane == 0) {
          ef.addr[plane] = aligned_ + static_cast<size_t>(j) * frame_size_;
          ef.iova[plane] = iova_ + static_cast<size_t>(j) * frame_size_;
        } else {
          ef.addr[plane] = static_cast<uint8_t*>(ef.addr[plane - 1]) +
                           ef.linesize[plane - 1] * plane_height;
          ef.iova[plane] = ef.iova[plane - 1] + ef.linesize[plane - 1] * plane_height;
        }
      }
      ef.size = frame_size_;
      ef.opaque = nullptr;
    }
    return true;
  }

  /* Allocate single-plane byte-addressed external frames (for ST 22p codestream). */
  bool init_bytes(mtl_handle st, size_t frame_size, int fb_cnt) {
    reset();
    if (!st || fb_cnt <= 0 || frame_size == 0) return false;
    if (!alloc_backing(st, frame_size, fb_cnt)) return false;

    for (int j = 0; j < fb_cnt; j++) {
      auto& ef = frames_[static_cast<size_t>(j)];
      ef.addr[0] = aligned_ + static_cast<size_t>(j) * frame_size_;
      ef.iova[0] = iova_ + static_cast<size_t>(j) * frame_size_;
      ef.size = frame_size_;
      ef.opaque = nullptr;
    }
    return true;
  }

  void reset() {
    if (st_ && aligned_ && iova_ != MTL_BAD_IOVA) {
      EXPECT_GE(mtl_dma_unmap(st_, aligned_, iova_, map_sz_), 0);
    }
    if (raw_) st_test_free(raw_);
    st_ = nullptr;
    raw_ = nullptr;
    aligned_ = nullptr;
    iova_ = MTL_BAD_IOVA;
    map_sz_ = 0;
    frame_size_ = 0;
    frames_.clear();
    in_use_.reset();
  }

  uint8_t* data() const {
    return aligned_;
  }
  size_t frame_size() const {
    return frame_size_;
  }
  size_t count() const {
    return frames_.size();
  }
  struct st_ext_frame* frames() {
    return frames_.data();
  }
  std::atomic<bool>* in_use_slot(size_t idx) {
    return (in_use_ && idx < frames_.size()) ? &in_use_[idx] : nullptr;
  }
  bool try_acquire_slot(size_t idx) {
    auto* slot = in_use_slot(idx);
    if (!slot) return false;
    bool expected = false;
    return slot->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }
  void release_slot(size_t idx) {
    if (auto* slot = in_use_slot(idx)) {
      slot->store(false, std::memory_order_release);
    }
  }

 private:
  bool alloc_backing(mtl_handle st, size_t fsize, int fb_cnt) {
    if (!st || fsize == 0 || fb_cnt <= 0) return false;
    const size_t pg_sz = mtl_page_size(st);
    if (pg_sz == 0) return false;
    st_ = st;
    frame_size_ = fsize;
    const size_t total_sz = frame_size_ * static_cast<size_t>(fb_cnt);
    map_sz_ = mtl_size_page_align(total_sz, pg_sz);
    raw_ = st_test_zmalloc(map_sz_ + pg_sz);
    if (!raw_) {
      reset();
      return false;
    }
    aligned_ =
        reinterpret_cast<uint8_t*>(MTL_ALIGN(reinterpret_cast<uint64_t>(raw_), pg_sz));
    iova_ = mtl_dma_map(st, aligned_, map_sz_);
    if (iova_ == MTL_BAD_IOVA) {
      reset();
      return false;
    }
    frames_.assign(static_cast<size_t>(fb_cnt), {});
    in_use_.reset(new std::atomic<bool>[static_cast<size_t>(fb_cnt)]);
    for (int j = 0; j < fb_cnt; j++) {
      in_use_[static_cast<size_t>(j)].store(false, std::memory_order_relaxed);
    }
    return true;
  }

  mtl_handle st_ = nullptr;
  void* raw_ = nullptr;
  uint8_t* aligned_ = nullptr;
  mtl_iova_t iova_ = MTL_BAD_IOVA;
  size_t map_sz_ = 0;
  size_t frame_size_ = 0;
  std::vector<struct st_ext_frame> frames_;
  std::unique_ptr<std::atomic<bool>[]> in_use_;
};

}  // namespace mtl_test
