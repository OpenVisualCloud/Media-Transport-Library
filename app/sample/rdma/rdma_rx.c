/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <inttypes.h>
#include <mtl_rdma/mtl_rdma_api.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int rx_notify_buffer_ready(void* priv, struct mtl_rdma_buffer* buffer) {
  (void)(priv);
  (void)(buffer);
  pthread_mutex_lock(&mtx);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mtx);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    printf("Usage: %s <local_ip> <ip> <port>\n", argv[0]);
    return -1;
  }
  int ret = 0;
  void* buffers[3] = {};
  mtl_rdma_handle mrh = NULL;
  mtl_rdma_rx_handle rx = NULL;
  struct mtl_rdma_init_params p = {
      .log_level = MTL_RDMA_LOG_LEVEL_INFO,
  };
  mrh = mtl_rdma_init(&p);
  if (!mrh) {
    printf("Failed to initialize RDMA\n");
    ret = -1;
    goto out;
  }

  for (int i = 0; i < 3; i++) {
    buffers[i] = calloc(1, 1024);
    if (!buffers[i]) {
      printf("Failed to allocate buffer\n");
      ret = -1;
      goto out;
    }
  }

  struct mtl_rdma_rx_ops rx_ops = {
      .name = "rdma_rx",
      .local_ip = argv[1],
      .ip = argv[2],
      .port = argv[3],
      .num_buffers = 3,
      .buffers = buffers,
      .buffer_capacity = 1024,
      .notify_buffer_ready = rx_notify_buffer_ready,
  };

  rx = mtl_rdma_rx_create(mrh, &rx_ops);
  if (!rx) {
    printf("Failed to create RDMA RX\n");
    ret = -1;
    goto out;
  }

  int total = 100;
  int buffer_consumed = 0;
  struct mtl_rdma_buffer* buffer = NULL;
  while (buffer_consumed < total) {
    buffer = mtl_rdma_rx_get_buffer(rx);
    if (!buffer) {
      /* wait for buffer ready */
      pthread_mutex_lock(&mtx);
      pthread_cond_wait(&cond, &mtx);
      pthread_mutex_unlock(&mtx);
      continue;
    }

    /* print buffer string */
    printf("Received buffer %d: %s\n", buffer_consumed, (char*)buffer->addr);
    usleep(10000); /* simulate consuming */

    ret = mtl_rdma_rx_put_buffer(rx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      ret = -1;
      goto out;
    }

    buffer_consumed++;
  }

out:
  if (rx) mtl_rdma_rx_free(rx);

  for (int i = 0; i < 3; i++) {
    if (buffers[i]) free(buffers[i]);
  }

  if (mrh) mtl_rdma_uinit(mrh);

  return 0;
}