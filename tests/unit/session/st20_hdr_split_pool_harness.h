/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the unit tests of the hdr split payload pool, the mempool that
 * hands the slots of the bound session to the PMD in frame order.
 */

#ifndef TESTS_UNIT_SESSION_ST20_HDR_SPLIT_POOL_HARNESS_H
#define TESTS_UNIT_SESSION_ST20_HDR_SPLIT_POOL_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_hs_ctx ut_hs_ctx;

/* A pool bound to a session of frames_cnt frames of mbufs_per_frame slots of slot_size
 * bytes, plus the mbufs_per_frame - 1 spare slots the session adds. The pool has
 * extra_mbufs mbufs more than the session has slots. NULL on failure. */
ut_hs_ctx* ut_hs_create(uint32_t mbufs_per_frame, uint32_t frames_cnt, uint32_t slot_size,
                        uint32_t extra_mbufs);
void ut_hs_destroy(ut_hs_ctx* ctx);

uint32_t ut_hs_slots(const ut_hs_ctx* ctx);
uint32_t ut_hs_stride(const ut_hs_ctx* ctx);
/* Free mbufs, as rte_mempool_avail_count() reports them. */
unsigned int ut_hs_avail(const ut_hs_ctx* ctx);

/* mt_hdr_split_bind() of the session, or of a session with extra_slots slots more. */
int ut_hs_bind(ut_hs_ctx* ctx, uint32_t extra_slots);
int ut_hs_unbind(ut_hs_ctx* ctx);

/* Takes one payload mbuf, as the PMD refill does. Returns its mbuf idx, -1 if refused. */
int ut_hs_get(ut_hs_ctx* ctx);
/* Takes n payload mbufs at once, as the ice bulk alloc refill does. 0 or -1. */
int ut_hs_get_bulk(ut_hs_ctx* ctx, unsigned int n);
/* Frees mbuf idx, as the RX tasklet does after the handler. */
void ut_hs_put(ut_hs_ctx* ctx, int idx);
/* Records that the handler saw the first packet of a frame in slot. */
void ut_hs_set_frame_start(ut_hs_ctx* ctx, uint32_t slot);

/* Bytes from the frame memory to the data of the held mbuf idx, by address and by the
 * iova the NIC writes to. */
int64_t ut_hs_data_offset(const ut_hs_ctx* ctx, int idx);
int64_t ut_hs_data_iova_offset(const ut_hs_ctx* ctx, int idx);
/* True if the held mbuf idx points into the scratch buffers of the pool. */
bool ut_hs_in_scratch(const ut_hs_ctx* ctx, int idx);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SESSION_ST20_HDR_SPLIT_POOL_HARNESS_H */
