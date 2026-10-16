/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>

#include "main/tsc_calibrate_harness.h"

/* mtl_init() reads socket[MTL_PORT_P] after its per-port loop, so that loop must run. */
TEST(MtlInitParams, NoPortIsRejected) {
  EXPECT_TRUE(ut_mtl_init_fails_with_num_ports(0));
}

/* MTL_PORT_MAX sizes every per-port array of the parameters and of the instance. */
TEST(MtlInitParams, MorePortsThanMtlPortMaxAreRejected) {
  EXPECT_TRUE(ut_mtl_init_fails_with_num_ports(MTL_PORT_MAX + 1));
}
