/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C header for the ST20 redundant-combined RX (st20rc) unit tests.
 *
 * Drives rx_st20rc_frame_ready() for the P-port transport of a not-yet-ready
 * st20rc context, i.e. while st20rc_rx_create() is still creating its sessions.
 */

#ifndef _ST20RC_RX_HARNESS_H_
#define _ST20RC_RX_HARNESS_H_

#include "st20_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut20rc_ctx ut20rc_ctx;

/** Create a not-ready context whose P transport is `handle` (NULL: not yet returned). */
ut20rc_ctx* ut20rc_ctx_create(st20_rx_handle handle);
void ut20rc_ctx_destroy(ut20rc_ctx* ctx);

/** Calls rx_st20rc_frame_ready() as the P-port session does. */
int ut20rc_frame_ready(ut20rc_ctx* ctx, void* frame, struct st20_rx_frame_meta* meta);

/** Store the P handle as st20rc_rx_create() does, putting frames refused while NULL. */
void ut20rc_set_handle(ut20rc_ctx* ctx, st20_rx_handle handle);

#ifdef __cplusplus
}
#endif

#endif /* _ST20RC_RX_HARNESS_H_ */
