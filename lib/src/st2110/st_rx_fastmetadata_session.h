/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#ifndef _ST_LIB_RX_FASTMETADATA_SESSION_HEAD_H_
#define _ST_LIB_RX_FASTMETADATA_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_FASTMETADATA_BURST_SIZE (128)

#define ST_RX_FASTMETADATA_PREFIX "RF_"

int st_rx_fastmetadata_sessions_sch_uinit(struct mtl_sch_impl* sch);

#endif /* _ST_LIB_RX_FASTMETADATA_SESSION_HEAD_H_ */