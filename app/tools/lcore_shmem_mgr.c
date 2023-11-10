/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <mtl/mtl_lcore_shm_api.h>

#include "log.h"

int main(int argc, char** argv) {
  MTL_MAY_UNUSED(argc);
  MTL_MAY_UNUSED(argv);

  mtl_lcore_shm_print();
  return 0;
}