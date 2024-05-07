#include <inttypes.h>
#include <mtl_rdma/mtl_rdma_api.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int buffer_acked = -3;

static int tx_notify_buffer_sent(void* priv, struct mtl_rdma_buffer* buffer) {
  (void)(priv);
  printf("Sent buffer: %s\n", (char*)buffer->addr);
  return 0;
}

static int tx_notify_buffer_done(void* priv, struct mtl_rdma_buffer* buffer) {
  (void)(priv);
  buffer_acked++;
  printf("ACKed buffer: %s\n", (char*)buffer->addr);
  pthread_mutex_lock(&mtx);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mtx);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    printf("Usage: %s <ip> <port>\n", argv[0]);
    return -1;
  }
  int ret = 0;
  void* buffers[3] = {};
  mtl_rdma_handle mrh = NULL;
  mtl_rdma_tx_handle tx = NULL;
  struct mtl_rdma_init_params p = {};
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

  struct mtl_rdma_tx_ops tx_ops = {
      .name = "rdma_tx",
      .ip = argv[1],
      .port = argv[2],
      .num_buffers = 3,
      .buffers = buffers,
      .buffer_capacity = 1024,
      .notify_buffer_done = tx_notify_buffer_done,
      .notify_buffer_sent = tx_notify_buffer_sent,
  };

  tx = mtl_rdma_tx_create(mrh, &tx_ops);
  if (!tx) {
    printf("Failed to create RDMA TX\n");
    ret = -1;
    goto out;
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
    usleep(20000); /* simulate producing */

    ret = mtl_rdma_tx_put_buffer(tx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      ret = -1;
      goto out;
    }
  }

  while (buffer_acked < count) {
    /* wait for all buffers acked */
    pthread_mutex_lock(&mtx);
    pthread_cond_wait(&cond, &mtx);
    pthread_mutex_unlock(&mtx);
  }

out:

  if (tx) mtl_rdma_tx_free(tx);

  for (int i = 0; i < 3; i++) {
    if (buffers[i]) free(buffers[i]);
  }

  if (mrh) mtl_rdma_uinit(mrh);

  return ret;
}