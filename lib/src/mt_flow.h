/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_FLOW_HEAD_H_
#define _MT_LIB_FLOW_HEAD_H_

#include "mt_main.h"

struct mt_rx_flow_rsp* mt_rx_flow_create(struct mtl_main_impl* impl, enum mtl_port port,
                                         uint16_t q, struct mt_rxq_flow* flow);
int mt_rx_flow_free(struct mtl_main_impl* impl, enum mtl_port port,
                    struct mt_rx_flow_rsp* rsp);

#endif
