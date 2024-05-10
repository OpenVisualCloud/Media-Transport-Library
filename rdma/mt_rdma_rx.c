/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "mt_rdma.h"

static int rdma_rx_send_buffer_done(struct mt_rdma_rx_ctx* ctx, uint16_t idx) {
  struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[idx];
  struct mt_rdma_message msg = {
      .magic = MT_RDMA_MSG_MAGIC,
      .type = MT_RDMA_MSG_BUFFER_DONE,
      .buf_done.buf_idx = idx,
      .buf_done.seq_num = 0, /* todo */
      .buf_done.rx_buf_addr = (uint64_t)rx_buffer->buffer.addr,
      .buf_done.rx_buf_key = rx_buffer->mr->rkey,
  };
  int ret = rdma_post_send(ctx->id, NULL, &msg, sizeof(msg), NULL, IBV_SEND_INLINE);
  if (ret) {
    err("%s(%s), rdma_post_send failed: %s\n", __func__, ctx->ops_name, strerror(errno));
    return -EIO;
  }
  /* post recv for next ready msg */
  void* r_msg = ctx->message_region + idx * 1024;
  ret = rdma_post_recv(ctx->id, r_msg, r_msg, 1024, ctx->message_mr);
  if (ret) {
    err("%s(%s), rdma_post_recv failed: %s\n", __func__, ctx->ops_name, strerror(errno));
    return -EIO;
  }
  rx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
  return 0;
}

static int rdma_rx_uinit_mrs(struct mt_rdma_rx_ctx* ctx) {
  MT_SAFE_FREE(ctx->message_mr, ibv_dereg_mr);
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    MT_SAFE_FREE(rx_buffer->mr, ibv_dereg_mr);
  }
  return 0;
}

static int rdma_rx_init_mrs(struct mt_rdma_rx_ctx* ctx) {
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    struct ibv_mr* mr =
        ibv_reg_mr(ctx->pd, rx_buffer->buffer.addr, rx_buffer->buffer.capacity,
                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
      err("%s(%s), ibv_reg_mr failed\n", __func__, ctx->ops_name);
      rdma_rx_uinit_mrs(ctx);
      return -ENOMEM;
    }
    rx_buffer->mr = mr;
  }

  struct ibv_mr* mr = ibv_reg_mr(ctx->pd, ctx->message_region, ctx->buffer_cnt * 1024,
                                 IBV_ACCESS_LOCAL_WRITE);
  if (!mr) {
    err("%s(%s), ibv_reg_mr message failed\n", __func__, ctx->ops_name);
    rdma_rx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->message_mr = mr;

  return 0;
}

static int rdma_rx_free_buffers(struct mt_rdma_rx_ctx* ctx) {
  rdma_rx_uinit_mrs(ctx);
  MT_SAFE_FREE(ctx->message_region, free);
  MT_SAFE_FREE(ctx->rx_buffers, free);
  return 0;
}

static int rdma_rx_alloc_buffers(struct mt_rdma_rx_ctx* ctx) {
  struct mtl_rdma_rx_ops* ops = &ctx->ops;
  ctx->buffer_cnt = ops->num_buffers;
  ctx->rx_buffers = (struct mt_rdma_rx_buffer*)calloc(ctx->buffer_cnt,
                                                      sizeof(struct mt_rdma_rx_buffer));
  if (!ctx->rx_buffers) {
    err("%s(%s), rx_buffers calloc failed\n", __func__, ctx->ops_name);
    return -ENOMEM;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    rx_buffer->idx = i;
    rx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
    rx_buffer->buffer.addr = ops->buffers[i];
    rx_buffer->buffer.capacity = ops->buffer_capacity;
  }

  /* alloc message region */
  ctx->message_region = (char*)calloc(ctx->buffer_cnt, 1024);
  if (!ctx->message_region) {
    err("%s(%s), message calloc failed\n", __func__, ctx->ops_name);
    rdma_rx_free_buffers(ctx);
    return -ENOMEM;
  }

  return 0;
}

/* cq poll thread */
static void* rdma_rx_cq_poll_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_rx_ctx* ctx = arg;
  struct mtl_rdma_rx_ops* ops = &ctx->ops;
  struct ibv_wc wc;
  struct ibv_cq* cq = ctx->cq;
  void* cq_ctx = NULL;
  int ms_timeout = 10;

  /* change completion channel fd to non blocking mode */
  int flags = fcntl(ctx->cc->fd, F_GETFL);
  ret = fcntl(ctx->cc->fd, F_SETFL, flags | O_NONBLOCK);
  if (ret) {
    err("%s(%s), fcntl failed\n", __func__, ctx->ops_name);
    goto out;
  }
  struct pollfd pfd = {
      .fd = ctx->cc->fd,
      .events = POLLIN,
      .revents = 0,
  };

  info("%s(%s), started\n", __func__, ctx->ops_name);
  while (!ctx->cq_poll_stop) {
    ret = poll(&pfd, 1, ms_timeout);
    if (ret < 0) {
      err("%s(%s), poll failed\n", __func__, ctx->ops_name);
      goto out;
    } else if (ret == 0) {
      /* timeout */
    } else {
      ret = ibv_get_cq_event(ctx->cc, &cq, &cq_ctx);
      if (ret) {
        err("%s(%s), ibv_get_cq_event failed\n", __func__, ctx->ops_name);
        goto out;
      }
      ibv_ack_cq_events(cq, 1);
      ret = ibv_req_notify_cq(cq, 0);
      if (ret) {
        err("%s(%s), ibv_req_notify_cq failed\n", __func__, ctx->ops_name);
        goto out;
      }
      while (ibv_poll_cq(ctx->cq, 1, &wc)) {
        if (wc.status != IBV_WC_SUCCESS) {
          err("%s(%s), work completion error: %s\n", __func__, ctx->ops_name,
              ibv_wc_status_str(wc.status));
          /* check more info */
          err("%s(%s), wc.opcode = %d, wc.vendor_error = 0x%x, wc.qp_num = %u\n",
              __func__, ctx->ops_name, wc.opcode, wc.vendor_err, wc.qp_num);
          goto out;
        }
        if (wc.opcode == IBV_WC_RECV) {
          struct mt_rdma_message* msg = (struct mt_rdma_message*)wc.wr_id;
          if (msg->magic == MT_RDMA_MSG_MAGIC) {
            if (msg->type == MT_RDMA_MSG_BUFFER_READY) {
              uint16_t idx = msg->buf_ready.buf_idx;
              struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[idx];
              rx_buffer->status = MT_RDMA_BUFFER_STATUS_READY;
              ctx->stat_buffer_received++;
              /* what about other meta? */
              if (ops->notify_buffer_ready) {
                ret = ops->notify_buffer_ready(ops->priv, &rx_buffer->buffer);
                if (ret) {
                  err("%s(%s), notify_buffer_ready failed\n", __func__, ctx->ops_name);
                  /* todo: error handle */
                }
              }
            } else if (msg->type == MT_RDMA_MSG_BYE) {
              info("%s(%s), received bye message\n", __func__, ctx->ops_name);
              /* todo: handle tx bye, notice that cq poll thread may stop before receiving
               * bye message */
            }
          }
        } else if (wc.opcode == IBV_WC_SEND) {
          if (wc.wr_id == MT_RDMA_MSG_BYE) {
            info("%s(%s), sent bye message, shutdown cq thread\n", __func__,
                 ctx->ops_name);
            goto out;
          }
        }
      }
    }
  }

out:
  info("%s(%s), exited\n", __func__, ctx->ops_name);
  return NULL;
}

/* connect thread */
static void* rdma_rx_connect_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_rx_ctx* ctx = arg;
  struct rdma_cm_event* event;
  struct pollfd pfd = {
      .fd = ctx->ec->fd,
      .events = POLLIN,
  };

  info("%s(%s), started\n", __func__, ctx->ops_name);
  while (!ctx->connect_stop) {
    ret = poll(&pfd, 1, 200);
    if (ret < 0) {
      err("%s(%s), poll failed: %s\n", __func__, ctx->ops_name, strerror(errno));
      goto connect_err;
    } else if (ret == 0) {
      /* timeout */
    } else {
      ret = rdma_get_cm_event(ctx->ec, &event);
      if (!ret) {
        switch (event->event) {
          case RDMA_CM_EVENT_ADDR_RESOLVED:
            ret = rdma_resolve_route(ctx->id, 2000);
            if (ret) {
              err("%s(%s), rdma_resolve_route failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            break;
          case RDMA_CM_EVENT_ROUTE_RESOLVED:
            ctx->pd = ibv_alloc_pd(event->id->verbs);
            if (!ctx->pd) {
              err("%s(%s), ibv_alloc_pd failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            ctx->cc = ibv_create_comp_channel(event->id->verbs);
            if (!ctx->cc) {
              err("%s(%s), ibv_create_comp_channel failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            ctx->cq = ibv_create_cq(event->id->verbs, 10, ctx, ctx->cc, 0);
            if (!ctx->cq) {
              err("%s(%s), ibv_create_cq failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            ret = ibv_req_notify_cq(ctx->cq, 0);
            if (ret) {
              err("%s(%s), ibv_req_notify_cq failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            struct ibv_qp_init_attr init_qp_attr = {
                .cap.max_send_wr = ctx->buffer_cnt * 2,
                .cap.max_recv_wr = ctx->buffer_cnt * 2,
                .cap.max_send_sge = 2, /* gather message and meta */
                .cap.max_recv_sge = 2, /* scatter message and meta */
                .cap.max_inline_data =
                    64, /* todo: include metadata size, if that size is larger than 64, we
                           should consider not using inline for msg */
                .sq_sig_all = 1,
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .qp_type = IBV_QPT_RC,
            };
            ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
            if (ret) {
              err("%s(%s), rdma_create_qp failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            ctx->qp = event->id->qp;
            ret = rdma_rx_init_mrs(ctx);
            if (ret) {
              err("%s(%s), rdma_tx_init_mrs failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            struct rdma_conn_param conn_param = {
                .initiator_depth = 1,
                .responder_resources = 1,
                .rnr_retry_count = 7 /* infinite retry */,
            };
            ret = rdma_connect(event->id, &conn_param);
            if (ret) {
              err("%s(%s), rdma_connect failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            break;
          case RDMA_CM_EVENT_ESTABLISHED:
            for (uint16_t i = 0; i < ctx->buffer_cnt; i++) /* start receiving */
              rdma_rx_send_buffer_done(ctx, i);
            ctx->connected = true;

            ctx->cq_poll_stop = false;
            ret = pthread_create(&ctx->cq_poll_thread, NULL, rdma_rx_cq_poll_thread, ctx);
            if (ret) {
              err("%s(%s), pthread_create failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            info("%s(%s), connected\n", __func__, ctx->ops_name);
            break;
          case RDMA_CM_EVENT_ADDR_ERROR:
          case RDMA_CM_EVENT_ROUTE_ERROR:
          case RDMA_CM_EVENT_CONNECT_ERROR:
          case RDMA_CM_EVENT_UNREACHABLE:
          case RDMA_CM_EVENT_REJECTED:
            err("%s(%s), event: %s, error: %d\n", __func__, ctx->ops_name,
                rdma_event_str(event->event), event->status);
            break;
          default:
            break;
        }
        rdma_ack_cm_event(event);
      }
    }
  }

  info("%s(%s), exited\n", __func__, ctx->ops_name);
  return NULL;

connect_err:
  rdma_ack_cm_event(event);
  /* add more error handling */
  return NULL;
}

struct mtl_rdma_buffer* mtl_rdma_rx_get_buffer(mtl_rdma_rx_handle handle) {
  struct mt_rdma_rx_ctx* ctx = handle;
  if (!ctx->connected) {
    return NULL;
  }
  /* find a ready buffer */
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    if (rx_buffer->status == MT_RDMA_BUFFER_STATUS_READY) {
      rx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION;
      return &rx_buffer->buffer;
    }
  }

  return NULL;
}

int mtl_rdma_rx_put_buffer(mtl_rdma_rx_handle handle, struct mtl_rdma_buffer* buffer) {
  struct mt_rdma_rx_ctx* ctx = handle;
  if (!ctx->connected) {
    return -EIO;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    if (&rx_buffer->buffer == buffer) {
      return rdma_rx_send_buffer_done(ctx, rx_buffer->idx);
    }
  }

  return -EIO;
}

int mtl_rdma_rx_free(mtl_rdma_rx_handle handle) {
  struct mt_rdma_rx_ctx* ctx = handle;

  if (!ctx) {
    return 0;
  }

  if (ctx->cq_poll_thread) {
    struct mt_rdma_message msg = {
        .magic = MT_RDMA_MSG_MAGIC,
        .type = MT_RDMA_MSG_BYE,
    };
    /* send bye to tx? and wake up cq event */
    if (rdma_post_send(ctx->id, (void*)MT_RDMA_MSG_BYE, &msg, sizeof(msg), NULL,
                       IBV_SEND_INLINE)) {
      err("%s(%s), rdma_post_send failed: %s\n", __func__, ctx->ops_name,
          strerror(errno));
    }
    ctx->cq_poll_stop = true;
    pthread_join(ctx->cq_poll_thread, NULL);
    ctx->cq_poll_thread = 0;
  }

  if (ctx->connect_thread) {
    ctx->connect_stop = true;
    pthread_join(ctx->connect_thread, NULL);
    ctx->connect_thread = 0;
  }

  rdma_rx_free_buffers(ctx);

  if (ctx->id && ctx->qp) {
    rdma_destroy_qp(ctx->id);
    ctx->qp = NULL;
  }

  MT_SAFE_FREE(ctx->cq, ibv_destroy_cq);
  MT_SAFE_FREE(ctx->cc, ibv_destroy_comp_channel);
  MT_SAFE_FREE(ctx->pd, ibv_dealloc_pd);
  MT_SAFE_FREE(ctx->id, rdma_destroy_id);
  MT_SAFE_FREE(ctx->ec, rdma_destroy_event_channel);

  free(ctx);

  return 0;
}

mtl_rdma_rx_handle mtl_rdma_rx_create(mtl_rdma_handle mrh, struct mtl_rdma_rx_ops* ops) {
  int ret = 0;
  struct mt_rdma_rx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    err("%s(%s), malloc mt_rdma_rx_ctx failed\n", __func__, ops->name);
    return NULL;
  }
  ctx->ops = *ops;
  snprintf(ctx->ops_name, 32, "%s", ops->name);

  ret = rdma_rx_alloc_buffers(ctx);
  if (ret) {
    err("%s(%s), rdma_rx_alloc_buffers failed\n", __func__, ops->name);
    goto out;
  }

  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    err("%s(%s), rdma_create_event_channel failed\n", __func__, ops->name);
    goto out;
  }

  ret = rdma_create_id(ctx->ec, &ctx->id, ctx, RDMA_PS_TCP);
  if (ret) {
    err("%s(%s), rdma_create_id failed\n", __func__, ops->name);
    goto out;
  }

  struct rdma_addrinfo hints = {};
  struct rdma_addrinfo *res, *rai;
  hints.ai_port_space = RDMA_PS_TCP;
  hints.ai_flags = RAI_PASSIVE;
  ret = rdma_getaddrinfo(ops->local_ip, NULL, &hints, &res);
  if (ret) {
    err("%s(%s), rdma_getaddrinfo failed\n", __func__, ops->name);
    goto out;
  }
  hints.ai_src_addr = res->ai_src_addr;
  hints.ai_src_len = res->ai_src_len;
  hints.ai_flags &= ~RAI_PASSIVE;
  ret = rdma_getaddrinfo(ops->ip, ops->port, &hints, &rai);
  rdma_freeaddrinfo(res);
  if (ret) {
    err("%s(%s), rdma_getaddrinfo failed\n", __func__, ops->name);
    goto out;
  }

  ret = rdma_resolve_addr(ctx->id, rai->ai_src_addr, rai->ai_dst_addr, 2000);
  rdma_freeaddrinfo(rai);
  if (ret) {
    err("%s(%s), rdma_resolve_addr failed\n", __func__, ops->name);
    goto out;
  }

  ctx->connect_stop = false;
  ret = pthread_create(&ctx->connect_thread, NULL, rdma_rx_connect_thread, ctx);
  if (ret) {
    err("%s(%s), pthread_create failed\n", __func__, ops->name);
    goto out;
  }

  return ctx;

out:
  mtl_rdma_rx_free(ctx);
  return NULL;
}
