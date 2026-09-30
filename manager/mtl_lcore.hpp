/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __MTL_LCORE_HPP__
#define __MTL_LCORE_HPP__

#include <bitset>
#include <cstdint>
#include <mutex>

#include "mtl_mproto.h"

/* One number, declared in the public header, because a client needs to know
 * which lcore ids the manager will refuse. */
#define MTL_MAX_LCORE MTL_MANAGER_MAX_LCORE

/**
 * Which lcore each MTL instance on this host holds.
 *
 * The server uses the process-wide get_instance(). The class is also directly
 * constructible, so a test can exercise it without touching that state.
 */
class mtl_lcore {
 public:
  mtl_lcore() = default;
  mtl_lcore(const mtl_lcore&) = delete;
  mtl_lcore& operator=(const mtl_lcore&) = delete;

  static mtl_lcore& get_instance();

  /**
   * Claim an lcore.
   *
   * @return 0 on success, -EINVAL when the id is out of range, -EBUSY when
   *         another instance already holds it.
   */
  int get_lcore(uint16_t lcore_id);

  /**
   * Release an lcore.
   *
   * @return 0 on success, -EINVAL when the id is out of range or free.
   */
  int put_lcore(uint16_t lcore_id);

  /** Whether `lcore_id` is claimed. Out of range reads as not claimed. */
  bool is_used(uint16_t lcore_id) const;

  /** Number of claimed lcores. */
  size_t used_count() const;

 private:
  std::bitset<MTL_MAX_LCORE> bs;
  mutable std::mutex bs_mtx;
};

#endif
