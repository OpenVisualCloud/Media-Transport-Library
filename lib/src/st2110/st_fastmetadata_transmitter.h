/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#ifndef _ST_LIB_FASTMETADATA_TRANSMITTER_HEAD_H_
#define _ST_LIB_FASTMETADATA_TRANSMITTER_HEAD_H_

#include "st_main.h"

int st_fastmetadata_transmitter_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                                     struct st_tx_fastmetadata_sessions_mgr* mgr,
                                     struct st_fastmetadata_transmitter_impl* trs);
int st_fastmetadata_transmitter_uinit(struct st_fastmetadata_transmitter_impl* trs);

#endif
