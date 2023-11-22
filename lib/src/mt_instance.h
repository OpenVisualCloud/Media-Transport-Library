/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_INSTANCE_HEAD_H_
#define _MT_LIB_INSTANCE_HEAD_H_

#include "mt_main.h"

int mt_instance_init(struct mtl_main_impl* impl, struct mtl_init_params* p);
int mt_instance_uinit(struct mtl_main_impl* impl);

int mt_instance_get_lcore(struct mtl_main_impl* impl, unsigned int lcore_id);
int mt_instance_put_lcore(struct mtl_main_impl* impl, unsigned int lcore_id);
int mt_instance_request_xsks_map_fd(struct mtl_main_impl* impl, unsigned int ifindex);
int mt_instance_update_udp_dp_filter(struct mtl_main_impl* impl, unsigned int ifindex,
                                     uint16_t dst_port, bool add);

#endif