/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_TX_ANCILLARY_SESSION_HEAD_H_
#define _ST_LIB_TX_ANCILLARY_SESSION_HEAD_H_

#include "st_main.h"

int st_tx_ancillary_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                      struct st_tx_ancillary_sessions_mgr* mgr);
int st_tx_ancillary_sessions_mgr_uinit(struct st_tx_ancillary_sessions_mgr* mgr);

struct st_tx_ancillary_session_impl* st_tx_ancillary_sessions_mgr_attach(
    struct st_tx_ancillary_sessions_mgr* mgr, struct st40_tx_ops* ops);
int st_tx_ancillary_sessions_mgr_detach(struct st_tx_ancillary_sessions_mgr* mgr,
                                        struct st_tx_ancillary_session_impl* s);

int st_tx_ancillary_sessions_mgr_update(struct st_tx_ancillary_sessions_mgr* mgr);

void st_tx_ancillary_sessions_stat(struct st_main_impl* impl);

static inline void tx_ancillary_session_lock(struct st_tx_ancillary_sessions_mgr* mgr,
                                             int sidx) {
  rte_spinlock_lock(&mgr->mutex[sidx]);
}

static inline int tx_ancillary_session_try_lock(struct st_tx_ancillary_sessions_mgr* mgr,
                                                int sidx) {
  return rte_spinlock_trylock(&mgr->mutex[sidx]);
}

static inline void tx_ancillary_session_unlock(struct st_tx_ancillary_sessions_mgr* mgr,
                                               int sidx) {
  rte_spinlock_unlock(&mgr->mutex[sidx]);
}

int tx_ancillary_session_rtp_pool_free(struct st_tx_ancillary_session_impl* s);

#endif
