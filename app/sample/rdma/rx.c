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
  pthread_mutex_lock(&mtx);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mtx);
  return 0;
}

int main(int argc, char** argv) {
  int ret = 0;
  struct mtl_rdma_init_params p = {};
  mtl_rdma_handle mrh = mtl_rdma_init(&p);
  if (!mrh) {
    printf("Failed to initialize RDMA\n");
    return -1;
  }

  void* buffers[3] = {};
  for (int i = 0; i < 3; i++) {
    buffers[i] = calloc(1, 1024);
    if (!buffers[i]) {
      printf("Failed to allocate buffer\n");
      return -1;
    }
  }

  struct mtl_rdma_rx_ops rx_ops = {
      .local_ip = "192.168.98.111",
      .ip = "192.168.98.110",
      .port = "20000",
      .num_buffers = 3,
      .buffers = buffers,
      .buffer_capacity = 1024,
      .notify_buffer_ready = rx_notify_buffer_ready,
  };

  mtl_rdma_rx_handle rx = mtl_rdma_rx_create(mrh, &rx_ops);
  if (!rx) {
    printf("Failed to create RDMA RX\n");
    return -1;
  }

  int count = 100;
  struct mtl_rdma_buffer* buffer = NULL;
  for (int i = 0; i < count; i++) {
    buffer = mtl_rdma_rx_get_buffer(rx);
    if (!buffer) {
      /* wait for buffer ready */
      pthread_mutex_lock(&mtx);
      pthread_cond_wait(&cond, &mtx);
      pthread_mutex_unlock(&mtx);
      i--;
      continue;
    }

    /* print buffer string */
    printf("Buffer %d: %sEND\n", i, (char*)buffer->addr);

    ret = mtl_rdma_rx_put_buffer(rx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      return -1;
    }
  }

  mtl_rdma_rx_free(rx);

  for (int i = 0; i < 3; i++) {
    free(buffers[i]);
  }

  mtl_rdma_uinit(mrh);

  return 0;
}