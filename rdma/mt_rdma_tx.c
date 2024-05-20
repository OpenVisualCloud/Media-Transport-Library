/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "mt_rdma.h"

static int rdma_tx_uinit_mrs(struct mt_rdma_tx_ctx* ctx) {
  MT_SAFE_FREE(ctx->meta_mr, ibv_dereg_mr);
  MT_SAFE_FREE(ctx->recv_msgs_mr, ibv_dereg_mr);
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    MT_SAFE_FREE(tx_buffer->mr, ibv_dereg_mr);
  }
  return 0;
}

static int rdma_tx_init_mrs(struct mt_rdma_tx_ctx* ctx) {
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    struct ibv_mr* mr =
        ibv_reg_mr(ctx->pd, tx_buffer->buffer.addr, tx_buffer->buffer.capacity,
                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
      err("%s(%s), ibv_reg_mr failed for buffer %p capacity %lu\n", __func__,
          ctx->ops_name, tx_buffer->buffer.addr, tx_buffer->buffer.capacity);
      rdma_tx_uinit_mrs(ctx);
      return -ENOMEM;
    }
    tx_buffer->mr = mr;
  }

  struct ibv_mr* mr = ibv_reg_mr(ctx->pd, ctx->recv_msgs,
                                 ctx->buffer_cnt * sizeof(struct mt_rdma_message),
                                 IBV_ACCESS_LOCAL_WRITE);
  if (!mr) {
    err("%s(%s), ibv_reg_mr receive messages failed\n", __func__, ctx->ops_name);
    rdma_tx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->recv_msgs_mr = mr;

  mr = ibv_reg_mr(ctx->pd, ctx->meta_region, ctx->buffer_cnt * MT_RDMA_MSG_MAX_SIZE,
                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    err("%s(%s), ibv_reg_mr meta region failed\n", __func__, ctx->ops_name);
    rdma_tx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->meta_mr = mr;

  return 0;
}

static int rdma_tx_free_buffers(struct mt_rdma_tx_ctx* ctx) {
  rdma_tx_uinit_mrs(ctx);
  MT_SAFE_FREE(ctx->meta_region, free);
  MT_SAFE_FREE(ctx->recv_msgs, free);
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    pthread_mutex_destroy(&ctx->tx_buffers[i].lock);
  }
  MT_SAFE_FREE(ctx->tx_buffers, free);
  return 0;
}

static int rdma_tx_alloc_buffers(struct mt_rdma_tx_ctx* ctx) {
  struct mtl_rdma_tx_ops* ops = &ctx->ops;
  ctx->buffer_cnt = ops->num_buffers;

  /* alloc receive message region */
  ctx->recv_msgs = calloc(ctx->buffer_cnt, sizeof(struct mt_rdma_message));
  if (!ctx->recv_msgs) {
    err("%s(%s), messages calloc failed\n", __func__, ctx->ops_name);
    rdma_tx_free_buffers(ctx);
    return -ENOMEM;
  }

  /* alloc metadata region */
  ctx->meta_region = calloc(ctx->buffer_cnt, MT_RDMA_MSG_MAX_SIZE);
  if (!ctx->meta_region) {
    err("%s(%s), meta region calloc failed\n", __func__, ctx->ops_name);
    rdma_tx_free_buffers(ctx);
    return -ENOMEM;
  }

  ctx->tx_buffers = calloc(ctx->buffer_cnt, sizeof(struct mt_rdma_tx_buffer));
  if (!ctx->tx_buffers) {
    err("%s(%s), tx_buffers calloc failed\n", __func__, ctx->ops_name);
    rdma_tx_free_buffers(ctx);
    return -ENOMEM;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    tx_buffer->idx = i;
    tx_buffer->status =
        MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION; /* need to receive done form rx to start */
    tx_buffer->ref_count = 1;
    tx_buffer->buffer.addr = ops->buffers[i];
    tx_buffer->buffer.capacity = ops->buffer_capacity;
    tx_buffer->meta = ctx->meta_region + i * MT_RDMA_MSG_MAX_SIZE;
    pthread_mutex_init(&tx_buffer->lock, NULL);
  }

  return 0;
}

/* cq poll thread */
static void* rdma_tx_cq_poll_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_tx_ctx* ctx = arg;
  struct mtl_rdma_tx_ops* ops = &ctx->ops;
  struct ibv_wc wc;
  struct ibv_cq* cq = ctx->cq;
  void* cq_ctx = NULL;
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
    ret = ctx->cq_poll_only ? 1 : poll(&pfd, 1, ms_timeout);
    if (ret < 0) {
      err("%s(%s), poll failed\n", __func__, ctx->ops_name);
      goto out;
    } else if (ret == 0) {
      /* timeout */
    } else {
      if (!ctx->cq_poll_only) {
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
      }
      while (ibv_poll_cq(cq, 1, &wc)) {
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
            if (msg->type == MT_RDMA_MSG_BUFFER_DONE) {
              uint16_t idx = msg->buf_done.buf_idx;
              dbg("%s(%s), received buffer %u done message, seq %u\n", __func__,
                  ctx->ops_name, idx, msg->buf_done.seq_num);
              struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[idx];
              pthread_mutex_lock(&tx_buffer->lock);
              if (tx_buffer->status != MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION) {
                err("%s(%s), received buffer done message with invalid status %d\n",
                    __func__, ctx->ops_name, tx_buffer->status);
                pthread_mutex_unlock(&tx_buffer->lock);
                goto out;
              }
              tx_buffer->remote_buffer = msg->buf_done.remote_buffer;
              tx_buffer->ref_count--;
              if (tx_buffer->ref_count == 0) {
                tx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
                if (ops->notify_buffer_done) {
                  ret = ops->notify_buffer_done(ops->priv, &tx_buffer->buffer);
                  if (ret) {
                    err("%s(%s), notify_buffer_done failed\n", __func__, ctx->ops_name);
                    /* todo error handle */
                  }
                }
              }
              pthread_mutex_unlock(&tx_buffer->lock);
              ctx->stat_buffer_acked++;
            }
          }

          ret = rdma_post_recv(ctx->id, msg, msg, sizeof(*msg), ctx->recv_msgs_mr);
          if (ret) {
            err("%s(%s), rdma_post_recv failed: %s\n", __func__, ctx->ops_name,
                strerror(errno));
            goto out;
          }
        } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
          struct mt_rdma_tx_buffer* tx_buffer = (struct mt_rdma_tx_buffer*)wc.wr_id;
          pthread_mutex_lock(&tx_buffer->lock);
          if (tx_buffer->status != MT_RDMA_BUFFER_STATUS_IN_TRANSMISSION) {
            err("%s(%s), buffer write done with invalid status %d\n", __func__,
                ctx->ops_name, tx_buffer->status);
            pthread_mutex_unlock(&tx_buffer->lock);
            goto out;
          }
          dbg("%s(%s), buffer %d write done\n", __func__, ctx->ops_name, tx_buffer->idx);
          tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION;
          tx_buffer->ref_count++;
          if (ops->notify_buffer_sent) {
            ret = ops->notify_buffer_sent(ops->priv, &tx_buffer->buffer);
            if (ret) {
              err("%s(%s), notify_buffer_sent failed\n", __func__, ctx->ops_name);
              /* todo: error handle */
            }
          }
          pthread_mutex_unlock(&tx_buffer->lock);
          ctx->stat_buffer_sent++;
        }
        ctx->stat_cq_poll_done++;
      }
      ctx->stat_cq_poll_empty++;
    }
  }

out:
  info("%s(%s), exited\n", __func__, ctx->ops_name);
  return NULL;
}

/* connect thread */
static void* rdma_tx_connect_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_tx_ctx* ctx = arg;
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
          case RDMA_CM_EVENT_CONNECT_REQUEST:
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

            ret = rdma_tx_init_mrs(ctx);
            if (ret) {
              err("%s(%s), rdma_tx_init_mrs failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }

            struct rdma_conn_param conn_param = {
                .initiator_depth = 1,
                .responder_resources = 1,
                .rnr_retry_count = 7,
            };
            ret = rdma_accept(event->id, &conn_param);
            if (ret) {
              err("%s(%s), rdma_accept failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }
            ctx->id = event->id;

            for (int i = 0; i < ctx->buffer_cnt; i++) { /* post receive done msg */
              struct mt_rdma_message* msg = &ctx->recv_msgs[i];
              ret = rdma_post_recv(ctx->id, msg, msg, sizeof(*msg), ctx->recv_msgs_mr);
              if (ret) {
                err("%s(%s), rdma_post_recv failed: %s\n", __func__, ctx->ops_name,
                    strerror(errno));
                goto connect_err;
              }
            }
            ctx->connected = true;

            /* create poll thread */
            ctx->cq_poll_stop = false;
            ret = pthread_create(&ctx->cq_poll_thread, NULL, rdma_tx_cq_poll_thread, ctx);
            if (ret) {
              err("%s(%s), pthread_create failed\n", __func__, ctx->ops_name);
              goto connect_err;
            }

            info("%s(%s), connected\n", __func__, ctx->ops_name);

            break;
          case RDMA_CM_EVENT_DISCONNECTED:
            info("%s(%s), RX disconnected.\n", __func__, ctx->ops_name);
            ctx->connected = false;
            /* todo: handle resources clearing */
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

struct mtl_rdma_buffer* mtl_rdma_tx_get_buffer(mtl_rdma_tx_handle handle) {
  struct mt_rdma_tx_ctx* ctx = handle;
  if (!ctx->connected) {
    return NULL;
  }

  /* change to use buffer_producer_idx to act as a queue */
  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    pthread_mutex_lock(&tx_buffer->lock);
    if (tx_buffer->status == MT_RDMA_BUFFER_STATUS_FREE) {
      tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_PRODUCTION;
      pthread_mutex_unlock(&tx_buffer->lock);
      return &tx_buffer->buffer;
    }
    pthread_mutex_unlock(&tx_buffer->lock);
  }
  return NULL;
}

int mtl_rdma_tx_put_buffer(mtl_rdma_tx_handle handle, struct mtl_rdma_buffer* buffer) {
  struct mt_rdma_tx_ctx* ctx = handle;
  if (!ctx->connected) {
    return -EIO;
  }

  if (buffer->size > buffer->capacity) {
    err("%s(%s), buffer size is too large\n", __func__, ctx->ops_name);
    return -EIO;
  }

  if (buffer->user_meta_size > MT_RDMA_USER_META_MAX_SIZE) {
    err("%s(%s), user meta size is too large\n", __func__, ctx->ops_name);
    return -EIO;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    pthread_mutex_lock(&tx_buffer->lock);
    if (&tx_buffer->buffer == buffer) {
      if (tx_buffer->status != MT_RDMA_BUFFER_STATUS_IN_PRODUCTION) {
        err("%s(%s), buffer %p is not in production\n", __func__, ctx->ops_name, buffer);
        pthread_mutex_unlock(&tx_buffer->lock);
        return -EIO;
      }
      /* write buffer to rx immediately */
      int ret = rdma_post_write(ctx->id, tx_buffer, buffer->addr, buffer->size,
                                tx_buffer->mr, 0, tx_buffer->remote_buffer.remote_addr,
                                tx_buffer->remote_buffer.remote_key);
      if (ret) {
        err("%s(%s), rdma_post_write failed: %s\n", __func__, ctx->ops_name,
            strerror(errno));
        pthread_mutex_unlock(&tx_buffer->lock);
        return -EIO;
      }

      /* write metadata to rx */
      memcpy(tx_buffer->meta, buffer->user_meta, buffer->user_meta_size);
      struct ibv_sge sge = {
          .addr = (uint64_t)(uintptr_t)tx_buffer->meta,
          .length = (uint32_t)buffer->user_meta_size,
          .lkey = ctx->meta_mr->lkey,
      };
      struct ibv_send_wr wr, *bad;
      wr.wr_id = (uintptr_t)tx_buffer;
      wr.next = NULL;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      wr.send_flags = IBV_SEND_SIGNALED;
      wr.imm_data =
          htonl((uint32_t)tx_buffer->idx << 16 | tx_buffer->buffer.user_meta_size);
      wr.wr.rdma.remote_addr = tx_buffer->remote_buffer.remote_meta_addr;
      wr.wr.rdma.rkey = tx_buffer->remote_buffer.remote_meta_key;
      ret = ibv_post_send(ctx->qp, &wr, &bad);
      if (ret) {
        err("%s(%s), ibv_post_send failed: %s\n", __func__, ctx->ops_name,
            strerror(errno));
        pthread_mutex_unlock(&tx_buffer->lock);
        return -EIO;
      }

      dbg("%s(%s), send meta for buffer %d\n", __func__, ctx->ops_name, i);

      tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_TRANSMISSION;
      pthread_mutex_unlock(&tx_buffer->lock);
      return 0;
    }
    pthread_mutex_unlock(&tx_buffer->lock);
  }

  err("%s(%s), buffer %p not found\n", __func__, ctx->ops_name, buffer);
  return -EIO;
}

int mtl_rdma_tx_free(mtl_rdma_tx_handle handle) {
  struct mt_rdma_tx_ctx* ctx = handle;
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

  rdma_tx_free_buffers(ctx);

  if (ctx->id && ctx->qp) {
    rdma_destroy_qp(ctx->id);
    ctx->qp = NULL;
  }

  MT_SAFE_FREE(ctx->cq, ibv_destroy_cq);
  MT_SAFE_FREE(ctx->cc, ibv_destroy_comp_channel);
  MT_SAFE_FREE(ctx->pd, ibv_dealloc_pd);
  MT_SAFE_FREE(ctx->listen_id, rdma_destroy_id);
  MT_SAFE_FREE(ctx->ec, rdma_destroy_event_channel);

  free(ctx);

  return 0;
}

mtl_rdma_tx_handle mtl_rdma_tx_create(mtl_rdma_handle mrh, struct mtl_rdma_tx_ops* ops) {
  int ret = 0;
  struct mt_rdma_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    err("%s(%s), malloc mt_rdma_tx_ctx failed\n", __func__, ops->name);
    return NULL;
  }
  ctx->ops = *ops;
  ctx->buffer_seq_num = 0;
  snprintf(ctx->ops_name, 32, "%s", ops->name);
  ctx->cq_poll_only = mt_rdma_low_latency(mrh);

  ret = rdma_tx_alloc_buffers(ctx);
  if (ret) {
    err("%s(%s), rdma_tx_alloc_buffers failed\n", __func__, ops->name);
    goto out;
  }

  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    err("%s(%s), rdma_create_event_channel failed\n", __func__, ops->name);
    goto out;
  }

  struct rdma_cm_id* listen_id = NULL;
  ret = rdma_create_id(ctx->ec, &listen_id, ctx, RDMA_PS_TCP);
  if (ret) {
    err("%s(%s), rdma_create_id failed\n", __func__, ops->name);
    goto out;
  }
  ctx->listen_id = listen_id;

  struct rdma_addrinfo hints = {.ai_port_space = RDMA_PS_TCP, .ai_flags = RAI_PASSIVE};
  struct rdma_addrinfo* rai;
  ret = rdma_getaddrinfo(ops->ip, ops->port, &hints, &rai);
  if (ret) {
    err("%s(%s), rdma_getaddrinfo failed\n", __func__, ops->name);
    goto out;
  }

  ret = rdma_bind_addr(listen_id, rai->ai_src_addr);
  rdma_freeaddrinfo(rai);
  if (ret) {
    err("%s(%s), rdma_bind_addr failed\n", __func__, ops->name);
    goto out;
  }

  ret = rdma_listen(listen_id, 0);
  if (ret) {
    err("%s(%s), rdma_listen failed\n", __func__, ops->name);
    goto out;
  }

  ctx->connect_stop = false;
  ret = pthread_create(&ctx->connect_thread, NULL, rdma_tx_connect_thread, ctx);
  if (ret) {
    err("%s(%s), pthread_create failed\n", __func__, ops->name);
    goto out;
  }

  return ctx;

out:
  mtl_rdma_tx_free(ctx);
  return NULL;
}