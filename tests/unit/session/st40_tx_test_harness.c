/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "session/st40_tx_test_harness.h"

#include "st2110/st_tx_ancillary_session.h"
#include "st2110/st_tx_ancillary_test.h"

void ut40_tx_corrupt_parity(uint8_t* buf, uint16_t udw_size) {
  tx_ancillary_corrupt_parity(buf, udw_size);
}
