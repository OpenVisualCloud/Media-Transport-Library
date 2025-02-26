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

#ifdef APP_HAS_SDL2
#include <SDL2/SDL.h>
#endif

#define NANOSECONDS_IN_SECOND 1000000000
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 2)  // UYVY format

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static atomic_bool keep_running = true;

#ifdef APP_HAS_SDL2
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
#endif

static int rx_notify_buffer_ready(void *priv, struct mtl_rdma_buffer *buffer) {
  (void)(priv);
  (void)(buffer);
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

#ifdef APP_HAS_SDL2
int sdl_init(size_t width, size_t height) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  window = SDL_CreateWindow("RDMA Frame Display", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 360, SDL_WINDOW_SHOWN);
  if (!window) {
    printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_UYVY, SDL_TEXTUREACCESS_STREAMING,
                              width, height);
  if (!texture) {
    printf("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  return 0;
}

void sdl_display_frame(void *frame, size_t width, size_t height) {
  (void)(height);
  SDL_UpdateTexture(texture, NULL, frame,
                    width * 2);  // Assuming UYVY (2 bytes per pixel)
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}

void sdl_cleanup() {
  if (texture) SDL_DestroyTexture(texture);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
}
#endif

int main(int argc, char **argv) {
#ifdef APP_HAS_SDL2
  if (sdl_init(1920, 1080) != 0) {
    fprintf(stderr, "Failed to initialize SDL.\n");
    return -1;
  }
#endif

  if (argc != 4) {
    printf("Usage: %s <local_ip> <ip> <port>\n", argv[0]);
    return -1;
  }
  signal(SIGINT, int_handler);

  int ret = 0;
  void *buffers[3] = {};
  mtl_rdma_handle mrh = NULL;
  mtl_rdma_rx_handle rx = NULL;
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

  for (int i = 0; i < 3; i++) {
    buffers[i] = mmap(NULL, FRAME_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (buffers[i] == MAP_FAILED) {
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
      .buffer_capacity = FRAME_SIZE,
      .notify_buffer_ready = rx_notify_buffer_ready,
  };

  rx = mtl_rdma_rx_create(mrh, &rx_ops);
  if (!rx) {
    printf("Failed to create RDMA RX\n");
    ret = -1;
    goto out;
  }

  struct timespec start_time, current_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  double elapsed_time;
  int frame_count = 0;
  double fps = 0.0;

  printf("Starting to receive frames\n");

  int frames_consumed = 0;
  struct mtl_rdma_buffer *buffer = NULL;
  while (keep_running) {
    buffer = mtl_rdma_rx_get_buffer(rx);
    if (!buffer) {
      /* wait for buffer ready */
      pthread_mutex_lock(&mtx);
      pthread_cond_wait(&cond, &mtx);
      pthread_mutex_unlock(&mtx);
      continue;
    }

    if (buffer->user_meta && buffer->user_meta_size) {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      uint64_t recv_time_ns =
          ((uint64_t)now.tv_sec * NANOSECONDS_IN_SECOND) + now.tv_nsec;
      uint64_t send_time_ns = *(uint64_t *)buffer->user_meta;
      printf("Latency: %.2f us\n", (recv_time_ns - send_time_ns) / 1000.0);
    }

#ifdef APP_HAS_SDL2
    /* display frame */
    sdl_display_frame(buffer->addr, 1920, 1080);
#endif

    ret = mtl_rdma_rx_put_buffer(rx, buffer);
    if (ret < 0) {
      printf("Failed to put buffer\n");
      ret = -1;
      break;
    }

    frames_consumed++;
    frame_count++;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    elapsed_time = current_time.tv_sec - start_time.tv_sec;
    elapsed_time += (current_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

    if (elapsed_time >= 5.0) {
      fps = frame_count / elapsed_time;
      printf("FPS: %.2f\n", fps);
      frame_count = 0;
      clock_gettime(CLOCK_MONOTONIC, &start_time);
    }
  }

  printf("Received %d frames\n", frames_consumed);

out:
  if (rx) mtl_rdma_rx_free(rx);

  for (int i = 0; i < 3; i++) {
    if (buffers[i] && buffers[i] != MAP_FAILED) munmap(buffers[i], FRAME_SIZE);
  }

  if (mrh) mtl_rdma_uinit(mrh);

#ifdef APP_HAS_SDL2
  sdl_cleanup();
#endif

  return 0;
}