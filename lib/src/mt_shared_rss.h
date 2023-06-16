/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_SHARED_RSS_HEAD_H_
#define _MT_LIB_SHARED_RSS_HEAD_H_

#include "mt_main.h"

int mt_srss_init(struct mtl_main_impl* impl);

int mt_srss_uinit(struct mtl_main_impl* impl);

struct mt_srss_entry* mt_srss_get(struct mtl_main_impl* impl, enum mtl_port port,
                                  struct mt_rxq_flow* flow);

int mt_srss_put(struct mt_srss_entry* entry);

#endif