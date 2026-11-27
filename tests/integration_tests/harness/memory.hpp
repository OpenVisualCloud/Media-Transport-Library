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
  if (!dma || mtl_udma_submit(dma) < 0) return false;
  uint16_t done = 0;
  const uint64_t deadline =
      st_test_get_monotonic_time() +
      static_cast<uint64_t>(timeout_ms > 0 ? timeout_ms : 0) * NS_PER_MS;
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
  ScopedHpBuf(ScopedHpBuf&& o) noexcept
      : st_(std::exchange(o.st_, nullptr)),
        size_(std::exchange(o.size_, 0)),
        ptr_(std::exchange(o.ptr_, nullptr)),
        iova_(std::exchange(o.iova_, MTL_BAD_IOVA)) {
  }
  ScopedHpBuf& operator=(ScopedHpBuf&& o) noexcept {
    if (this != &o) {
      reset();
      st_ = std::exchange(o.st_, nullptr);
      size_ = std::exchange(o.size_, 0);
      ptr_ = std::exchange(o.ptr_, nullptr);
      iova_ = std::exchange(o.iova_, MTL_BAD_IOVA);
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
  PageAlignedBuf(size_t usable_size, size_t pg_sz, bool zero = false) {
    if (pg_sz == 0 || (pg_sz & (pg_sz - 1)) != 0 ||
        pg_sz > (std::numeric_limits<size_t>::max() - usable_size) / 3) {
      return;
    }
    const size_t alloc_sz = usable_size + 3 * pg_sz;
    raw_.reset(static_cast<uint8_t*>(zero ? calloc(1, alloc_sz) : malloc(alloc_sz)));
    if (raw_) {
      aligned_ = reinterpret_cast<uint8_t*>(
          MTL_ALIGN(reinterpret_cast<uint64_t>(raw_.get()) + pg_sz, pg_sz));
    }
  }
  PageAlignedBuf(PageAlignedBuf&& o) noexcept
      : raw_(std::move(o.raw_)), aligned_(std::exchange(o.aligned_, nullptr)) {
  }
  PageAlignedBuf& operator=(PageAlignedBuf&& o) noexcept {
    if (this != &o) {
      raw_ = std::move(o.raw_);
      aligned_ = std::exchange(o.aligned_, nullptr);
    }
    return *this;
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
  ScopedDmaMap(ScopedDmaMap&& o) noexcept
      : st_(std::exchange(o.st_, nullptr)),
        vaddr_(std::exchange(o.vaddr_, nullptr)),
        size_(std::exchange(o.size_, 0)),
        iova_(std::exchange(o.iova_, MTL_BAD_IOVA)) {
  }
  ScopedDmaMap& operator=(ScopedDmaMap&& o) noexcept {
    if (this != &o) {
      if (valid()) {
        EXPECT_GE(unmap(), 0);
      }
      st_ = std::exchange(o.st_, nullptr);
      vaddr_ = std::exchange(o.vaddr_, nullptr);
      size_ = std::exchange(o.size_, 0);
      iova_ = std::exchange(o.iova_, MTL_BAD_IOVA);
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
    const mtl_iova_t iova = std::exchange(iova_, MTL_BAD_IOVA);
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
  ScopedDmaMem(ScopedDmaMem&& o) noexcept
      : st_(std::exchange(o.st_, nullptr)), handle_(std::exchange(o.handle_, nullptr)) {
  }
  ScopedDmaMem& operator=(ScopedDmaMem&& o) noexcept {
    if (this != &o) {
      reset();
      st_ = std::exchange(o.st_, nullptr);
      handle_ = std::exchange(o.handle_, nullptr);
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
  ~ScopedExtFrames() = default;
  ScopedExtFrames(const ScopedExtFrames&) = delete;
  ScopedExtFrames& operator=(const ScopedExtFrames&) = delete;
  ScopedExtFrames(ScopedExtFrames&&) noexcept = default;
  ScopedExtFrames& operator=(ScopedExtFrames&& o) noexcept {
    if (this != &o) {
      reset();
      buf_ = std::move(o.buf_);
      dma_map_ = std::move(o.dma_map_);
      frame_size_ = std::exchange(o.frame_size_, 0);
      frames_ = std::move(o.frames_);
      in_use_ = std::move(o.in_use_);
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
    const size_t base_fsize = st_frame_size(fmt, width, height, interlaced);
    const size_t pg_sz = mtl_page_size(st);
    if (planes == 0 || base_fsize == 0 || pg_sz == 0) return false;

    const size_t plane_height = static_cast<size_t>(interlaced ? height / 2 : height);
    frame_size_ = base_fsize + line_padding_size * plane_height * planes;
    const size_t map_sz =
        mtl_size_page_align(frame_size_ * static_cast<size_t>(fb_cnt), pg_sz);
    buf_ = PageAlignedBuf(map_sz, pg_sz, true);
    dma_map_ = buf_ ? ScopedDmaMap(st, buf_.data(), map_sz) : ScopedDmaMap{};
    if (!dma_map_.valid()) {
      reset();
      return false;
    }

    frames_.assign(static_cast<size_t>(fb_cnt), {});
    in_use_.reset(new std::atomic<bool>[static_cast<size_t>(fb_cnt)]);
    for (int j = 0; j < fb_cnt; j++) {
      in_use_[static_cast<size_t>(j)].store(false, std::memory_order_relaxed);
      auto& ef = frames_[static_cast<size_t>(j)];
      for (uint8_t plane = 0; plane < planes; plane++) {
        ef.linesize[plane] =
            st_frame_least_linesize(fmt, width, plane) + line_padding_size;
        if (plane == 0) {
          ef.addr[plane] = buf_.data() + static_cast<size_t>(j) * frame_size_;
          ef.iova[plane] = dma_map_.iova() + static_cast<size_t>(j) * frame_size_;
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

  void reset() {
    dma_map_ = {};
    buf_ = {};
    frame_size_ = 0;
    frames_.clear();
    in_use_.reset();
  }

  uint8_t* data() const {
    return buf_.data();
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
  /* Member order ensures dma_map_ unmaps before buf_ frees on destruction. */
  PageAlignedBuf buf_;
  ScopedDmaMap dma_map_;
  size_t frame_size_ = 0;
  std::vector<struct st_ext_frame> frames_;
  std::unique_ptr<std::atomic<bool>[]> in_use_;
};

}  // namespace mtl_test
