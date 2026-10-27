/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* Compiles lib/src/ipmx/ipmx.h as a build without enable_ipmx sees it. */

#undef MTL_HAS_IPMX
#undef MTL_HAS_USDT
#include "ipmx/ipmx_stub_harness.h"

#include "ipmx/ipmx.h"

int ut_ipmx_stub_tx_ops_check(const struct st20_tx_ops* ops) {
  return ipmx_tx_ops_check(ops);
}

int ut_ipmx_stub_rx_ops_check(const struct st20_rx_ops* ops) {
  return ipmx_rx_ops_check(ops);
}
