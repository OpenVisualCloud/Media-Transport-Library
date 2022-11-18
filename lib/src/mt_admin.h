/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_ADMIN_HEAD_H_
#define _ST_LIB_ADMIN_HEAD_H_

#include "mt_main.h"

int mt_admin_init(struct mtl_main_impl* impl);
int mt_admin_uinit(struct mtl_main_impl* impl);

#endif
