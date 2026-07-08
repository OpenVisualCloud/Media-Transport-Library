/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API exposing the ST 2110-40 (ancillary) TX BAD_PARITY
 * corrective-transform (tx_ancillary_corrupt_parity(), st_tx_ancillary_test.h)
 * to the C++ gtest layer.
 */

#ifndef _ST40_TX_TEST_HARNESS_H_
#define _ST40_TX_TEST_HARNESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wraps tx_ancillary_corrupt_parity(): strips parity bits from did/sdid/
 * data_count/each UDW word of the RFC 8331 sub-packet at `buf` and
 * recomputes its checksum in place. */
void ut40_tx_corrupt_parity(uint8_t* buf, uint16_t udw_size);

#ifdef __cplusplus
}
#endif

#endif
