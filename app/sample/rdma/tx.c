#include <inttypes.h>
#include <mtl_rdma/mtl_rdma_api.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int tx_notify_buffer_done(void* priv, struct mtl_rdma_buffer* buffer) {
  // printf("Buffer %p done\n", buffer);
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

  struct mtl_rdma_tx_ops tx_ops = {
      .ip = "192.168.98.110",
      .port = "20000",
      .num_buffers = 3,
      .buffers = buffers,
      .buffer_capacity = 1024,
      .notify_buffer_done = tx_notify_buffer_done,
  };

  mtl_rdma_tx_handle tx = mtl_rdma_tx_create(mrh, &tx_ops);
  if (!tx) {
    printf("Failed to create RDMA TX\n");
    return -1;
  }

  int count = 100;
  struct mtl_rdma_buffer* buffer = NULL;
  for (int i = 0; i < count; i++) {
    buffer = mtl_rdma_tx_get_buffer(tx);
    if (!buffer) {
      /* wait for buffer done */
      pthread_mutex_lock(&mtx);
      pthread_cond_wait(&cond, &mtx);
      pthread_mutex_unlock(&mtx);
      i--;
      continue;
    }

    snprintf((char*)buffer->addr, buffer->capacity, "Hello, RDMA! %d", i);
    buffer->size = strlen((char*)buffer->addr) + 1;

    ret = mtl_rdma_tx_put_buffer(tx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      return -1;
    }

    usleep(20000);
  }

  mtl_rdma_tx_free(tx);

  for (int i = 0; i < 3; i++) {
    free(buffers[i]);
  }

  mtl_rdma_uinit(mrh);

  return 0;
}