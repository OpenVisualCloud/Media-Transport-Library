/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include <inttypes.h>
#include <mtl_rdma/mtl_rdma_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TARGET_FPS 30
#define NANOSECONDS_IN_SECOND 1000000000
#define DESIRED_FRAME_DURATION (NANOSECONDS_IN_SECOND / TARGET_FPS)

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static atomic_bool keep_running = true;

static int frames_sent = 0;
static int frames_acked = -3;

static void control_fps(struct timespec *start_time) {
  struct timespec end_time;
  long long elapsed_time, time_to_wait;

  clock_gettime(CLOCK_MONOTONIC, &end_time);

  elapsed_time =
      (end_time.tv_sec - start_time->tv_sec) * NANOSECONDS_IN_SECOND +
      (end_time.tv_nsec - start_time->tv_nsec);
  time_to_wait = DESIRED_FRAME_DURATION - elapsed_time;

  if (time_to_wait > 0) {
    struct timespec sleep_time;
    sleep_time.tv_sec = time_to_wait / NANOSECONDS_IN_SECOND;
    sleep_time.tv_nsec = time_to_wait % NANOSECONDS_IN_SECOND;
    nanosleep(&sleep_time, NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, start_time);
}

static int tx_notify_buffer_sent(void *priv, struct mtl_rdma_buffer *buffer) {
  (void)(priv);
  (void)(buffer);
  frames_sent++;
  return 0;
}

static int tx_notify_buffer_done(void *priv, struct mtl_rdma_buffer *buffer) {
  (void)(priv);
  (void)(buffer);
  frames_acked++;
  pthread_mutex_lock(&mtx);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mtx);
  return 0;
}

void int_handler(int dummy) {
  (void)(dummy);
  keep_running = false;
  pthread_mutex_lock(&mtx);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mtx);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    printf("Usage: %s <ip> <port> <yuv_file>\n", argv[0]);
    return -1;
  }
  signal(SIGINT, int_handler);

  int ret = 0;
  void *buffers[3] = {};
  mtl_rdma_handle mrh = NULL;
  mtl_rdma_tx_handle tx = NULL;
  struct mtl_rdma_init_params p = {
      .log_level = MTL_RDMA_LOG_LEVEL_INFO,
      //.flags = MTL_RDMA_FLAG_LOW_LATENCY,
  };
  mrh = mtl_rdma_init(&p);
  if (!mrh) {
    printf("Failed to initialize RDMA\n");
    ret = -1;
    goto out;
  }

  size_t frame_size = 1920 * 1080 * 2; /* UYVY */
  for (int i = 0; i < 3; i++) {
    buffers[i] = mmap(NULL, frame_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (buffers[i] == MAP_FAILED) {
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
      .buffer_capacity = frame_size,
      .notify_buffer_done = tx_notify_buffer_done,
      .notify_buffer_sent = tx_notify_buffer_sent,
  };

  tx = mtl_rdma_tx_create(mrh, &tx_ops);
  if (!tx) {
    printf("Failed to create RDMA TX\n");
    ret = -1;
    goto out;
  }

  FILE *yuv_file = fopen(argv[3], "rb");
  if (!yuv_file) {
    printf("Failed to open YUV file\n");
    ret = -1;
    goto out;
  }

  printf("Starting to send frames\n");

  struct timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  while (keep_running) {
    struct mtl_rdma_buffer *buffer = mtl_rdma_tx_get_buffer(tx);
    if (!buffer) {
      /* wait for buffer done */
      pthread_mutex_lock(&mtx);
      pthread_cond_wait(&cond, &mtx);
      pthread_mutex_unlock(&mtx);
      continue;
    }

    while (fread(buffer->addr, 1, frame_size, yuv_file) != frame_size) {
      if (feof(yuv_file)) {
        /* restart from the beginning if the end of file is reached */
        fseek(yuv_file, 0, SEEK_SET);
        continue;
      } else {
        printf("Failed to read frame from file\n");
        ret = -1;
        goto out;
      }
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t send_time_ns =
        ((uint64_t)now.tv_sec * NANOSECONDS_IN_SECOND) + now.tv_nsec;

    buffer->size = frame_size;
    buffer->user_meta = &send_time_ns;
    buffer->user_meta_size = sizeof(uint64_t);

    ret = mtl_rdma_tx_put_buffer(tx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      ret = -1;
      goto out;
    }

    control_fps(&start_time);
  }

  printf("Sent %d frames\n", frames_acked);

out:

  if (tx)
    mtl_rdma_tx_free(tx);

  for (int i = 0; i < 3; i++) {
    if (buffers[i] && buffers[i] != MAP_FAILED)
      munmap(buffers[i], frame_size);
  }

  if (mrh)
    mtl_rdma_uinit(mrh);

  return ret;
}