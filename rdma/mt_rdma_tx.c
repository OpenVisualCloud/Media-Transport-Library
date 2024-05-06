/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "mt_rdma.h"

static int rdma_tx_uinit_mrs(struct mt_rdma_tx_ctx* ctx) {
  MT_SAFE_FREE(ctx->message_mr, ibv_dereg_mr);
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
      fprintf(stderr, "ibv_reg_mr failed for buffer %p capacity %lu\n",
              tx_buffer->buffer.addr, tx_buffer->buffer.capacity);
      rdma_tx_uinit_mrs(ctx);
      return -ENOMEM;
    }
    tx_buffer->mr = mr;
  }

  struct ibv_mr* mr = ibv_reg_mr(ctx->pd, ctx->message_region, ctx->buffer_cnt * 1024,
                                 IBV_ACCESS_LOCAL_WRITE);
  if (!mr) {
    fprintf(stderr, "ibv_reg_mr failed for message\n");
    rdma_tx_uinit_mrs(ctx);
    return -ENOMEM;
  }
  ctx->message_mr = mr;

  return 0;
}

static int rdma_tx_free_buffers(struct mt_rdma_tx_ctx* ctx) {
  rdma_tx_uinit_mrs(ctx);
  MT_SAFE_FREE(ctx->tx_buffers, free);
  return 0;
}

static int rdma_tx_alloc_buffers(struct mt_rdma_tx_ctx* ctx) {
  struct mtl_rdma_tx_ops* ops = &ctx->ops;
  ctx->buffer_cnt = ops->num_buffers;
  ctx->tx_buffers = (struct mt_rdma_tx_buffer*)calloc(ctx->buffer_cnt,
                                                      sizeof(struct mt_rdma_tx_buffer));
  if (!ctx->tx_buffers) {
    fprintf(stderr, "calloc failed\n");
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
  }

  /* alloc message region */
  ctx->message_region = (char*)calloc(ctx->buffer_cnt, 1024);
  if (!ctx->message_region) {
    fprintf(stderr, "calloc failed\n");
    rdma_tx_free_buffers(ctx);
    return -ENOMEM;
  }

  return 0;
}

/* cq poll thread */
static void* rdma_tx_cq_poll_thread(void* arg) {
  int ret = 0;
  struct mt_rdma_tx_ctx* ctx = arg;
  struct mtl_rdma_tx_ops* ops = &ctx->ops;
  struct ibv_wc wc;
  for (;;) {
    struct ibv_cq* cq;
    void* cq_ctx = NULL;
    ret = ibv_get_cq_event(ctx->cc, &cq, &cq_ctx);
    if (ret) {
      fprintf(stderr, "ibv_get_cq_event failed\n");
      goto out;
    }
    ibv_ack_cq_events(cq, 1);
    ret = ibv_req_notify_cq(cq, 0);
    if (ret) {
      fprintf(stderr, "ibv_req_notify_cq failed\n");
      goto out;
    }
    while (ibv_poll_cq(ctx->cq, 1, &wc)) {
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion error: %s\n", ibv_wc_status_str(wc.status));
        /* check more info */
        fprintf(stderr, "wc.vendor_error = 0x%x, wc.qp_num = %u\n", wc.vendor_err,
                wc.qp_num);
        goto out;
      }
      if (wc.opcode == IBV_WC_RECV) {
        struct mt_rdma_message* msg = (struct mt_rdma_message*)wc.wr_id;
        if (msg->magic == MT_RDMA_MSG_MAGIC) {
          if (msg->type == MT_RDMA_MSG_BUFFER_DONE) {
            uint16_t idx = msg->buf_done.buf_idx;
            struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[idx];
            tx_buffer->remote_addr = msg->buf_done.rx_buf_addr;
            tx_buffer->remote_key = msg->buf_done.rx_buf_key;
            tx_buffer->ref_count--;
            if (tx_buffer->ref_count == 0) {
              tx_buffer->status = MT_RDMA_BUFFER_STATUS_FREE;
              if (ops->notify_buffer_done) {
                ret = ops->notify_buffer_done(ops->priv, &tx_buffer->buffer);
                if (ret) {
                  fprintf(stderr, "notify_buffer_done failed\n");
                  /* todo error handle */
                }
              }
            }
            ctx->stat_buffer_acked++;
          }
        }

        rdma_post_recv(ctx->id, msg, msg, 1024, ctx->message_mr);
      } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
        struct mt_rdma_tx_buffer* tx_buffer = (struct mt_rdma_tx_buffer*)wc.wr_id;
        /* send ready message to rx, todo add user meta with sgl */
        struct mt_rdma_message msg = {
            .magic = MT_RDMA_MSG_MAGIC,
            .type = MT_RDMA_MSG_BUFFER_READY,
            .buf_ready.buf_idx = tx_buffer->idx,
            .buf_ready.seq_num = 0, /* todo */
        };
        rdma_post_send(ctx->id, NULL, &msg, sizeof(msg), NULL, IBV_SEND_INLINE);
        tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_CONSUMPTION;
        tx_buffer->ref_count++;
        if (ops->notify_buffer_sent) {
          ret = ops->notify_buffer_sent(ops->priv, &tx_buffer->buffer);
          if (ret) {
            fprintf(stderr, "notify_buffer_sent failed\n");
            /* todo error handle */
          }
        }
        ctx->stat_buffer_sent++;
      }
    }
  }

out:
  return NULL;
}

/* connect thread */
static void* rdma_tx_connect_thread(void* arg) {
  struct mt_rdma_tx_ctx* ctx = arg;
  struct rdma_cm_event* event;
  struct pollfd pfd;
  pfd.fd = ctx->ec->fd;
  pfd.events = POLLIN;

  while (!ctx->connect_stop) {
    int ret = poll(&pfd, 1, 200);
    if (ret > 0) {
      ret = rdma_get_cm_event(ctx->ec, &event);
      if (!ret) {
        switch (event->event) {
          case RDMA_CM_EVENT_CONNECT_REQUEST:
            ctx->pd = ibv_alloc_pd(event->id->verbs);
            if (!ctx->pd) {
              fprintf(stderr, "ibv_alloc_pd failed\n");
              goto connect_err;
            }
            ctx->cc = ibv_create_comp_channel(event->id->verbs);
            if (!ctx->cc) {
              fprintf(stderr, "ibv_create_comp_channel failed\n");
              goto connect_err;
            }
            ctx->cq = ibv_create_cq(event->id->verbs, 10, ctx, ctx->cc, 0);
            if (!ctx->cq) {
              fprintf(stderr, "ibv_create_cq failed\n");
              goto connect_err;
            }
            ret = ibv_req_notify_cq(ctx->cq, 0);
            if (ret) {
              fprintf(stderr, "ibv_req_notify_cq failed\n");
              goto connect_err;
            }
            struct ibv_qp_init_attr init_qp_attr = {
                .cap.max_send_wr = ctx->buffer_cnt * 2,
                .cap.max_recv_wr = ctx->buffer_cnt,
                .cap.max_send_sge = 2, /* gather message and meta */
                .cap.max_recv_sge = 2, /* scatter message and meta */
                .cap.max_inline_data = 64,
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .qp_type = IBV_QPT_RC,
            };
            ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
            if (ret) {
              fprintf(stderr, "rdma_create_qp failed\n");
              goto connect_err;
            }
            ctx->qp = event->id->qp;

            ret = rdma_tx_init_mrs(ctx);
            if (ret) {
              fprintf(stderr, "rdma_tx_init_mrs failed\n");
              goto connect_err;
            }

            struct rdma_conn_param conn_param = {
                .initiator_depth = 1,
                .responder_resources = 1,
                .rnr_retry_count = 7,
            };
            ret = rdma_accept(event->id, &conn_param);
            if (ret) {
              fprintf(stderr, "rdma_accept failed\n");
              goto connect_err;
            }
            ctx->id = event->id;

            for (int i = 0; i < ctx->buffer_cnt; i++) { /* post receive done msg */
              void* msg = ctx->message_region + i * 1024;
              rdma_post_recv(ctx->id, msg, msg, 1024, ctx->message_mr);
            }

            /* create poll thread */
            ctx->cq_poll_stop = false;
            ret = pthread_create(&ctx->cq_poll_thread, NULL, rdma_tx_cq_poll_thread, ctx);
            if (ret) {
              fprintf(stderr, "pthread_create failed\n");
              goto connect_err;
            }

            ctx->connected = true;
            break;
          case RDMA_CM_EVENT_DISCONNECTED:
            printf("RX disconnected.\n");
            ctx->connected = false;
            break;
          default:
            break;
        }
        rdma_ack_cm_event(event);
      }
    } else if (ret == 0) {
      /* poll timeout */
    } else {
      break;
    }
  }

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
    if (tx_buffer->status == MT_RDMA_BUFFER_STATUS_FREE) {
      tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_PRODUCTION;
      return &tx_buffer->buffer;
    }
  }
  return NULL;
}

int mtl_rdma_tx_put_buffer(mtl_rdma_tx_handle handle, struct mtl_rdma_buffer* buffer) {
  struct mt_rdma_tx_ctx* ctx = handle;
  if (!ctx->connected) {
    return -1;
  }

  for (int i = 0; i < ctx->buffer_cnt; i++) {
    struct mt_rdma_tx_buffer* tx_buffer = &ctx->tx_buffers[i];
    if (&tx_buffer->buffer == buffer) {
      /* write to rx immediately */
      rdma_post_write(ctx->id, tx_buffer, buffer->addr, buffer->size, tx_buffer->mr,
                      IBV_SEND_SIGNALED, tx_buffer->remote_addr, tx_buffer->remote_key);
      tx_buffer->status = MT_RDMA_BUFFER_STATUS_IN_TRANSMISSION;
      return 0;
    }
  }
  return -1;
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
  }

  if (ctx->connect_thread) {
    ctx->connect_stop = true;
    pthread_join(ctx->connect_thread, NULL);
    ctx->connect_thread = 0;
  }

  if (ctx->id && ctx->qp) {
    rdma_destroy_qp(ctx->id);
    ctx->qp = NULL;
  }

  MT_SAFE_FREE(ctx->cq, ibv_destroy_cq);
  MT_SAFE_FREE(ctx->cc, ibv_destroy_comp_channel);
  MT_SAFE_FREE(ctx->pd, ibv_dealloc_pd);
  MT_SAFE_FREE(ctx->listen_id, rdma_destroy_id);
  MT_SAFE_FREE(ctx->ec, rdma_destroy_event_channel);

  rdma_tx_free_buffers(ctx);
  free(ctx);

  return 0;
}

mtl_rdma_tx_handle mtl_rdma_tx_create(mtl_rdma_handle mrh, struct mtl_rdma_tx_ops* ops) {
  int ret = 0;
  struct mt_rdma_tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    fprintf(stderr, "malloc mt_rdma_tx_ctx failed\n");
    return NULL;
  }
  ctx->ops = *ops;
  ctx->buffer_producer_idx = 0;
  ctx->buffer_seq_num = 0;

  ret = rdma_tx_alloc_buffers(ctx);
  if (ret) {
    fprintf(stderr, "rdma_tx_alloc_buffers failed\n");
    goto out;
  }

  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto out;
  }

  struct rdma_cm_id* listen_id = NULL;
  ret = rdma_create_id(ctx->ec, &listen_id, ctx, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed\n");
    goto out;
  }
  ctx->listen_id = listen_id;

  struct rdma_addrinfo hints = {.ai_port_space = RDMA_PS_TCP, .ai_flags = RAI_PASSIVE};
  struct rdma_addrinfo* rai;
  ret = rdma_getaddrinfo(ops->ip, ops->port, &hints, &rai);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }

  ret = rdma_bind_addr(listen_id, rai->ai_src_addr);
  if (ret) {
    fprintf(stderr, "rdma_bind_addr failed\n");
    goto out;
  }

  ret = rdma_listen(listen_id, 0);
  if (ret) {
    fprintf(stderr, "rdma_listen failed\n");
    goto out;
  }

  ctx->connect_stop = false;
  ret = pthread_create(&ctx->connect_thread, NULL, rdma_tx_connect_thread, ctx);
  if (ret) {
    fprintf(stderr, "pthread_create failed\n");
    goto out;
  }

  return ctx;

out:
  mtl_rdma_tx_free(ctx);
  return NULL;
}