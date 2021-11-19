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

#ifndef _ST_LIB_ANCILLARY_TRANSMITTER_HEAD_H_
#define _ST_LIB_ANCILLARY_TRANSMITTER_HEAD_H_

#include "st_main.h"

int st_ancillary_transmitter_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_tx_ancillary_sessions_mgr* mgr,
                                  struct st_ancillary_transmitter_impl* trs);
int st_ancillary_transmitter_uinit(struct st_ancillary_transmitter_impl* trs);

#endif
