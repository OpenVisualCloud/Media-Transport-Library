/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_FLOW_HEAD_H_
#define _MT_LIB_FLOW_HEAD_H_

#include "mt_main.h"

int mt_flow_init(struct mtl_main_impl* impl);
int mt_flow_uinit(struct mtl_main_impl* impl);

struct mt_rx_flow_rsp* mt_rx_flow_create(struct mtl_main_impl* impl, enum mtl_port port,
                                         uint16_t q, struct mt_rxq_flow* flow);
int mt_rx_flow_free(struct mtl_main_impl* impl, enum mtl_port port,
                    struct mt_rx_flow_rsp* rsp);

/* iavf: create the port's rte_flows again after a VF reset, which frees them all */
void mt_flow_reset_cb_register(struct mtl_main_impl* impl, enum mtl_port port);
void mt_flow_reset_cb_unregister(struct mtl_main_impl* impl, enum mtl_port port);

#endif
