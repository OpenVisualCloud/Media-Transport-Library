/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ADMIN_HEAD_H_
#define _MT_LIB_ADMIN_HEAD_H_

#include "mt_main.h"

/* the admin thread reads each port link this often, for the TX hang detectors */
#define MT_ADMIN_LINK_POLL_US ((uint64_t)100 * US_PER_MS)

int mt_admin_init(struct mtl_main_impl* impl);
int mt_admin_uinit(struct mtl_main_impl* impl);

#endif
