/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _MT_RDMA_UTIL_HEAD_H_
#define _MT_RDMA_UTIL_HEAD_H_

#include "mt_rdma.h"

int mt_rdma_handle_cq_events(struct ibv_comp_channel* cc, struct ibv_cq* cq);

int mt_rdma_post_write_imm(struct rdma_cm_id* id, void* context, void* addr,
                           size_t length, struct ibv_mr* mr, int flags,
                           uint64_t remote_addr, uint32_t rkey, uint32_t imm_data);

#endif /* _MT_RDMA_UTIL_HEAD_H_ */