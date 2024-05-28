/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mt_rdma.h"
#include "mt_rdma_util.h"

static int rdma_rx_send_buffer_done(struct mt_rdma_rx_ctx* ctx, uint16_t idx) {
  struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[idx];
  pthread_mutex_lock(&rx_buffer->lock);
  struct mt_rdma_message msg = {
      .magic = MT_RDMA_MSG_MAGIC,
      .type = MT_RDMA_MSG_BUFFER_DONE,
      .buf_done.buf_idx = idx,
      .buf_done.seq_num = rx_buffer->buffer.seq_num,
      .buf_done.remote_buffer = {
          .remote_addr = (uint64_t)rx_buffer->buffer.addr,
          .remote_key = rx_buffer->mr->rkey,
          .remote_meta_addr = (uint64_t)rx_buffer->buffer.user_meta,
          .remote_meta_key = ctx->meta_mr->rkey,
      }};
  int ret = rdma_post_send(ctx->id, NULL, &msg, sizeof(msg), NULL,
                           IBV_SEND_INLINE | IBV_SEND_SIGNALED);
  if (ret) {
    err("%s(%s), rdma_post_send failed: %s\n", __func__, ctx->ops_name, strerror(errno));
    pthread_mutex_unlock(&rx_buffer->lock);
    return -EIO;
  }
  rx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
  pthread_mutex_unlock(&rx_buffer->lock);
  return 0;
}

static int rdma_rx_uinit_mrs(struct mt_rdma_rx_ctx* ctx) {
  MT_SAFE_FREE(ctx->meta_mr, ibv_dereg_mr);
  MT_SAFE_FREE(ctx->recv_msgs_mr, ibv_dereg_mr);
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

  struct ibv_mr* mr = ibv_reg_mr(ctx->pd, ctx->recv_msgs,
                                 ctx->buffer_cnt * sizeof(struct mt_rdma_message),
                                 IBV_ACCESS_LOCAL_WRITE);
  if (!mr) {
    err("%s(%s), ibv_reg_mr message failed\n", __func__, ctx->ops_name);
    rdma_rx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->recv_msgs_mr = mr;

  mr = ibv_reg_mr(ctx->pd, ctx->meta_region, ctx->buffer_cnt * MT_RDMA_MSG_MAX_SIZE,
                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    err("%s(%s), ibv_reg_mr meta region failed\n", __func__, ctx->ops_name);
    rdma_rx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->meta_mr = mr;

  return 0;
}

static int rdma_rx_free_buffers(struct mt_rdma_rx_ctx* ctx) {
  rdma_rx_uinit_mrs(ctx);
  MT_SAFE_FREE(ctx->meta_region, free);
  MT_SAFE_FREE(ctx->recv_msgs, free);
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    pthread_mutex_destroy(&ctx->rx_buffers[i].lock);
  }
  MT_SAFE_FREE(ctx->rx_buffers, free);
  return 0;
}

static int rdma_rx_alloc_buffers(struct mt_rdma_rx_ctx* ctx) {
  struct mtl_rdma_rx_ops* ops = &ctx->ops;
  ctx->buffer_cnt = ops->num_buffers;

  /* alloc receive message region, send messages are inlined */
  ctx->recv_msgs = calloc(ctx->buffer_cnt, sizeof(struct mt_rdma_message));
  if (!ctx->recv_msgs) {
    err("%s(%s), message calloc failed\n", __func__, ctx->ops_name);
    rdma_rx_free_buffers(ctx);
    return -ENOMEM;
  }

  /* alloc metadata region */
  ctx->meta_region = calloc(ctx->buffer_cnt, MT_RDMA_MSG_MAX_SIZE);
  if (!ctx->meta_region) {
    err("%s(%s), meta region calloc failed\n", __func__, ctx->ops_name);
    rdma_rx_free_buffers(ctx);
    return -ENOMEM;
  }

  ctx->rx_buffers = calloc(ctx->buffer_cnt, sizeof(struct mt_rdma_rx_buffer));
  if (!ctx->rx_buffers) {
    err("%s(%s), rx_buffers calloc failed\n", __func__, ctx->ops_name);
    rdma_rx_free_buffers(ctx);
    return -ENOMEM;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[i];
    rx_buffer->idx = i;
    rx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
    rx_buffer->buffer.addr = ops->buffers[i];
    rx_buffer->buffer.capacity = ops->buffer_capacity;
    rx_buffer->buffer.user_meta = ctx->meta_region + i * MT_RDMA_MSG_MAX_SIZE;
    pthread_mutex_init(&rx_buffer->lock, NULL);
  }

  return 0;
}

static int rdma_rx_handle_wc_recv_imm(struct mt_rdma_rx_ctx* ctx, struct ibv_wc* wc) {
  struct mtl_rdma_rx_ops* ops = &ctx->ops;
  uint16_t idx = ntohl(wc->imm_data) >> 16;
  dbg("%s(%s), buffer %u write done\n", __func__, ctx->ops_name, idx);
  struct mt_rdma_rx_buffer* rx_buffer = &ctx->rx_buffers[idx];

  pthread_mutex_lock(&rx_buffer->lock);
  if (rx_buffer->status != MT_RDMA_BUFFER_STATUS_FREE) {
    err("%s(%s), buffer %u invalid status %d\n", __func__, ctx->ops_name, idx,
        rx_buffer->status);
    pthread_mutex_unlock(&rx_buffer->lock);
    return -EINVAL;
  }
  rx_buffer->buffer.user_meta_size = ntohl(wc->imm_data) & 0x0000FFFF;
  rx_buffer->status = MT_RDMA_BUFFER_STATUS_READY;
  ctx->stat_buffer_received++;
  if (ops->notify_buffer_ready) ops->notify_buffer_ready(ops->priv, &rx_buffer->buffer);
  pthread_mutex_unlock(&rx_buffer->lock);

  struct mt_rdma_message* msg = (struct mt_rdma_message*)wc->wr_id;
  int ret = rdma_post_recv(ctx->id, msg, msg, sizeof(*msg), ctx->recv_msgs_mr);
  if (ret) {
    err("%s(%s), rdma_post_recv failed: %s\n", __func__, ctx->ops_name, strerror(errno));
    return -EIO;
  }
  return 0;
}

static int rdma_rx_handle_wc(struct mt_rdma_rx_ctx* ctx, struct ibv_wc* wc) {
  if (wc->status != IBV_WC_SUCCESS) {
    err("%s(%s), work completion error: %s\n", __func__, ctx->ops_name,
        ibv_wc_status_str(wc->status));
    err("%s(%s), opcode = %d, vendor_error = 0x%x, qp_num = %u\n", __func__,
        ctx->ops_name, wc->opcode, wc->vendor_err, wc->qp_num);
    return -EIO;
  }

  switch (wc->opcode) {
    case IBV_WC_RECV_RDMA_WITH_IMM:
      return rdma_rx_handle_wc_recv_imm(ctx, wc);
    case IBV_WC_SEND:
      return 0; /* nothing to do */
    default:
      err("%s(%s), unexpected opcode: %d\n", __func__, ctx->ops_name, wc->opcode);
      return -EIO;
  }
}

/* cq poll thread */
static void* rdma_rx_cq_poll_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_rx_ctx* ctx = arg;
  struct ibv_wc wc;
  struct ibv_cq* cq = ctx->cq;
  int ms_timeout = 10;
  struct pollfd pfd = {0};

  /* change completion channel fd to non blocking mode */
  if (!ctx->cq_poll_only) {
    int flags = fcntl(ctx->cc->fd, F_GETFL);
    ret = fcntl(ctx->cc->fd, F_SETFL, flags | O_NONBLOCK);
    if (ret) {
      err("%s(%s), fcntl failed\n", __func__, ctx->ops_name);
      goto out;
    }
    pfd.fd = ctx->cc->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
  }

  info("%s(%s), started\n", __func__, ctx->ops_name);
  while (!ctx->cq_poll_stop) {
    if (!ctx->cq_poll_only) {
      ret = poll(&pfd, 1, ms_timeout);
      if (ret < 0) {
        err("%s(%s), poll failed\n", __func__, ctx->ops_name);
        goto out;
      } else if (ret == 0) {
        /* timeout */
        continue;
      }
      if (mt_rdma_handle_cq_events(ctx->cc, cq)) {
        err("%s(%s), handle cq events failed\n", __func__, ctx->ops_name);
        goto out;
      }
    }

    while (ibv_poll_cq(cq, 1, &wc)) {
      if (ctx->cq_poll_stop) break;
      if (rdma_rx_handle_wc(ctx, &wc)) goto out;
      ctx->stat_cq_poll_done++;
    }

    ctx->stat_cq_poll_empty++;
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
            if (!ctx->cq_poll_only) {
              ctx->cc = ibv_create_comp_channel(event->id->verbs);
              if (!ctx->cc) {
                err("%s(%s), ibv_create_comp_channel failed\n", __func__, ctx->ops_name);
                goto connect_err;
              }
            }
            ctx->cq = ibv_create_cq(event->id->verbs, 10, ctx, ctx->cc, 0);
            if (!ctx->cq) {
              err("%s(%s), ibv_create_cq failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            if (!ctx->cq_poll_only) {
              ret = ibv_req_notify_cq(ctx->cq, 0);
              if (ret) {
                err("%s(%s), ibv_req_notify_cq failed\n", __func__, ctx->ops_name);
                goto connect_err;
              }
            }
            struct ibv_qp_init_attr init_qp_attr = {
                .cap.max_send_wr = ctx->buffer_cnt * 2,
                .cap.max_recv_wr = ctx->buffer_cnt * 2,
                .cap.max_send_sge = 1,
                .cap.max_recv_sge = 1,
                .cap.max_inline_data = sizeof(struct mt_rdma_message),
                .sq_sig_all = 0,
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
            for (uint16_t i = 0; i < ctx->buffer_cnt; i++) { /* start receiving */
              struct mt_rdma_message* msg = &ctx->recv_msgs[i];
              ret = rdma_post_recv(ctx->id, msg, msg, sizeof(*msg), ctx->recv_msgs_mr);
              if (ret) {
                err("%s(%s), rdma_post_recv failed: %s\n", __func__, ctx->ops_name,
                    strerror(errno));
                goto connect_err;
              }
              ret = rdma_rx_send_buffer_done(ctx, i);
              if (ret) {
                err("%s(%s), rdma_rx_send_buffer_done failed\n", __func__, ctx->ops_name);
                goto connect_err;
              }
            }
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
    pthread_mutex_lock(&rx_buffer->lock);
    if (rx_buffer->status == MT_RDMA_BUFFER_STATUS_READY) {
      rx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION;
      pthread_mutex_unlock(&rx_buffer->lock);
      return &rx_buffer->buffer;
    }
    pthread_mutex_unlock(&rx_buffer->lock);
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
    if (&rx_buffer->buffer != buffer) continue;
    pthread_mutex_lock(&rx_buffer->lock);
    if (rx_buffer->status != MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION) {
      err("%s(%s), buffer %p not in consumption\n", __func__, ctx->ops_name, buffer);
      pthread_mutex_unlock(&rx_buffer->lock);
      return -EIO;
    }
    pthread_mutex_unlock(&rx_buffer->lock);
    return rdma_rx_send_buffer_done(ctx, rx_buffer->idx);
  }

  err("%s(%s), buffer %p not found\n", __func__, ctx->ops_name, buffer);
  return -EIO;
}

int mtl_rdma_rx_free(mtl_rdma_rx_handle handle) {
  struct mt_rdma_rx_ctx* ctx = handle;

  if (!ctx) {
    return 0;
  }

  if (ctx->cq_poll_thread) {
    ctx->cq_poll_stop = true;
    pthread_join(ctx->cq_poll_thread, NULL);
    ctx->cq_poll_thread = 0;

    /* print cq poll stat */
    dbg("%s(%s), cq poll done: %lu, cq poll empty: %lu\n", __func__, ctx->ops_name,
        ctx->stat_cq_poll_done, ctx->stat_cq_poll_empty);
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
  ctx->cq_poll_only = mt_rdma_low_latency(mrh);

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
