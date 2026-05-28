/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_RX_COMMON_HEAD_H_
#define _ST_LIB_RX_COMMON_HEAD_H_

#include <stddef.h>
#include <stdint.h>

/* Hysteresis helpers for RX back-pressure warn lines. Single-writer
 * (mt_stat thread). `arm` increments the counter and writes
 * " (sustained Nx)" into suffix_out once the busy condition persists for
 * 3+ consecutive intervals, else writes "". `reset` zeroes the counter. */
void st_rx_backpressure_arm(uint32_t* consecutive, char* suffix_out, size_t cap);
void st_rx_backpressure_reset(uint32_t* consecutive);

#endif
