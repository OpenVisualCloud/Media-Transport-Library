/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mt_rdma_util.h"

int mt_rdma_handle_cq_events(struct ibv_comp_channel* cc, struct ibv_cq* cq) {
  void* cq_ctx = NULL;
  int ret = ibv_get_cq_event(cc, &cq, &cq_ctx);
  if (ret) {
    err("%s, ibv_get_cq_event failed\n", __func__);
    return -EIO;
  }
  ibv_ack_cq_events(cq, 1);
  ret = ibv_req_notify_cq(cq, 0);
  if (ret) {
    err("%s, ibv_req_notify_cq failed\n", __func__);
    return -EIO;
  }
  return 0;
}

int mt_rdma_post_write_imm(struct rdma_cm_id* id, void* context, void* addr,
                           size_t length, struct ibv_mr* mr, int flags,
                           uint64_t remote_addr, uint32_t rkey, uint32_t imm_data) {
  struct ibv_send_wr wr, *bad_wr;

  struct ibv_sge sge = {
      .addr = (uint64_t)addr,
      .length = length,
      .lkey = mr->lkey,
  };

  wr.wr_id = (uint64_t)context;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  wr.send_flags = flags;
  wr.imm_data = imm_data;
  wr.wr.rdma.remote_addr = remote_addr;
  wr.wr.rdma.rkey = rkey;

  return ibv_post_send(id->qp, &wr, &bad_wr);
}