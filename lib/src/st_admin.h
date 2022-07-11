/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_ADMIN_HEAD_H_
#define _ST_LIB_ADMIN_HEAD_H_

#include "st_main.h"

int st_admin_init(struct st_main_impl* impl);
int st_admin_uinit(struct st_main_impl* impl);

#endif
