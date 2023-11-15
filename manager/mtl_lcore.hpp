/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <bitset>
#include <iostream>
#include <mutex>

#define MTL_MAX_LCORE 128

class mtl_lcore {
 private:
  std::bitset<MTL_MAX_LCORE> bs;
  std::mutex bs_mtx;

 public:
  mtl_lcore();
  ~mtl_lcore();
  int get_lcore(uint16_t lcore_id);
  int put_lcore(uint16_t lcore_id);
};

mtl_lcore::mtl_lcore() { bs.reset(); }

mtl_lcore::~mtl_lcore() {}

int mtl_lcore::get_lcore(uint16_t lcore_id) {
  std::lock_guard<std::mutex> lock(bs_mtx);
  if (bs.test(lcore_id))
    return -1;
  else
    bs.set(lcore_id, true);
  return 0;
}

int mtl_lcore::put_lcore(uint16_t lcore_id) {
  std::lock_guard<std::mutex> lock(bs_mtx);
  if (!bs.test(lcore_id))
    return -1;
  else
    bs.set(lcore_id, false);
  return 0;
}