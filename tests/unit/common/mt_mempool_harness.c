/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C shim for mt_mempool_test.cpp: mt_util.h pulls in mt_main.h, which is C only.
 */

#include <stdlib.h>

#include "mt_util.h"

struct mtl_main_impl* ut_mempool_impl_create(void) {
  return calloc(1, sizeof(struct mtl_main_impl));
}

void ut_mempool_impl_destroy(struct mtl_main_impl* impl) {
  free(impl);
}

struct rte_mempool* ut_mempool_create(struct mtl_main_impl* impl, const char* name,
                                      unsigned int n, unsigned int cache_size,
                                      uint16_t priv_size, uint16_t element_size) {
  return mt_mempool_create_by_ops(impl, name, n, cache_size, priv_size, element_size,
                                  MT_MEMPOOL_OPS_DEFAULT, (int)rte_socket_id());
}

int ut_mempool_free(struct rte_mempool* mp) {
  return mt_mempool_free(mp);
}
