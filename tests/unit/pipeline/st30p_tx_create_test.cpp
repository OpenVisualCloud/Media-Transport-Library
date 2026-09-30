/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st30p_tx_create() NUMA placement of the pipeline context: the NIC socket by
 * default, ops.socket_id only with ST30P_TX_FLAG_FORCE_NUMA.
 */

#include <gtest/gtest.h>
#include <rte_lcore.h>
#include <rte_memory.h>

#include "pipeline/st30p_tx_harness.h"

TEST(St30PipelineTxCreate, ForceNumaPlacesCtxOnRequestedSocket) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  int requested = (int)rte_socket_id();
  ASSERT_NE(requested, SOCKET_ID_ANY);

  EXPECT_EQ(
      ut30p_tx_create_ctx_socket(SOCKET_ID_ANY, ST30P_TX_FLAG_FORCE_NUMA, requested),
      requested);
}

TEST(St30PipelineTxCreate, CtxStaysOnNicSocketWithoutForceNuma) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  int requested = (int)rte_socket_id();
  ASSERT_NE(requested, SOCKET_ID_ANY);

  /* bit 2 is ST30P_RX_FLAG_FORCE_NUMA's value and has no TX meaning */
  EXPECT_EQ(ut30p_tx_create_ctx_socket(SOCKET_ID_ANY, MTL_BIT32(2), requested),
            SOCKET_ID_ANY);
}
