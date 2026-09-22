/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_MTL_MTL_HARNESS_H
#define TESTS_UNIT_MTL_MTL_HARNESS_H

#include <stdbool.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_mtl_ctx ut_mtl_ctx;

ut_mtl_ctx* ut_mtl_create_ctx(int num_ports);
void ut_mtl_destroy_ctx(ut_mtl_ctx* ctx);
mtl_handle ut_mtl_handle(ut_mtl_ctx* ctx);
void ut_mtl_set_hw_rx_timestamp(ut_mtl_ctx* ctx, enum mtl_port port, bool active);
void ut_mtl_set_invalid_handle_type(ut_mtl_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
