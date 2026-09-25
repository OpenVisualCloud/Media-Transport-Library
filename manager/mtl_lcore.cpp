/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mtl_lcore.hpp"

#include <cerrno>

mtl_lcore& mtl_lcore::get_instance() {
  static mtl_lcore instance;
  return instance;
}

int mtl_lcore::get_lcore(uint16_t lcore_id) {
  if (lcore_id >= MTL_MAX_LCORE) return -EINVAL;

  std::lock_guard<std::mutex> lock(bs_mtx);
  if (bs.test(lcore_id)) return -EBUSY;

  bs.set(lcore_id, true);
  return 0;
}

int mtl_lcore::put_lcore(uint16_t lcore_id) {
  if (lcore_id >= MTL_MAX_LCORE) return -EINVAL;

  std::lock_guard<std::mutex> lock(bs_mtx);
  if (!bs.test(lcore_id)) return -EINVAL;

  bs.set(lcore_id, false);
  return 0;
}

bool mtl_lcore::is_used(uint16_t lcore_id) const {
  if (lcore_id >= MTL_MAX_LCORE) return false;

  std::lock_guard<std::mutex> lock(bs_mtx);
  return bs.test(lcore_id);
}

size_t mtl_lcore::used_count() const {
  std::lock_guard<std::mutex> lock(bs_mtx);
  return bs.count();
}
