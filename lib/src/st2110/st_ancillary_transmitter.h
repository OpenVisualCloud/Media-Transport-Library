/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_ANCILLARY_TRANSMITTER_HEAD_H_
#define _ST_LIB_ANCILLARY_TRANSMITTER_HEAD_H_

#include "st_main.h"

int st_ancillary_transmitter_init(struct mtl_main_impl* impl, struct mt_sch_impl* sch,
                                  struct st_tx_ancillary_sessions_mgr* mgr,
                                  struct st_ancillary_transmitter_impl* trs);
int st_ancillary_transmitter_uinit(struct st_ancillary_transmitter_impl* trs);

#endif
