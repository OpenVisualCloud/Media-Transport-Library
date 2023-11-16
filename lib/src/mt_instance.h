/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_INSTANCE_HEAD_H_
#define _MT_LIB_INSTANCE_HEAD_H_

#include "mt_main.h"

int mt_instance_init(struct mtl_main_impl* impl);
int mt_instance_uinit(struct mtl_main_impl* impl);

int mt_instance_get_lcore(struct mtl_main_impl* impl, unsigned int lcore_id);
int mt_instance_put_lcore(struct mtl_main_impl* impl, unsigned int lcore_id);

#endif