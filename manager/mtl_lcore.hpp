/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef __MTL_LCORE_HPP__
#define __MTL_LCORE_HPP__

#include <bitset>
#include <mutex>

#define MTL_MAX_LCORE 128

class mtl_lcore {
private:
  std::bitset<MTL_MAX_LCORE> bs;
  std::mutex bs_mtx;

  mtl_lcore() { bs.reset(); }
  ~mtl_lcore() {}

public:
  mtl_lcore(const mtl_lcore &) = delete;
  mtl_lcore &operator=(const mtl_lcore &) = delete;

  static mtl_lcore &get_instance() {
    static mtl_lcore instance;
    return instance;
  }

  int get_lcore(uint16_t lcore_id);
  int put_lcore(uint16_t lcore_id);
};

int mtl_lcore::get_lcore(uint16_t lcore_id) {
  if (lcore_id >= MTL_MAX_LCORE)
    return -1;
  std::lock_guard<std::mutex> lock(bs_mtx);
  if (bs.test(lcore_id))
    return -1;
  else
    bs.set(lcore_id, true);
  return 0;
}

int mtl_lcore::put_lcore(uint16_t lcore_id) {
  if (lcore_id >= MTL_MAX_LCORE)
    return -1;
  std::lock_guard<std::mutex> lock(bs_mtx);
  if (!bs.test(lcore_id))
    return -1;
  else
    bs.set(lcore_id, false);
  return 0;
}

#endif