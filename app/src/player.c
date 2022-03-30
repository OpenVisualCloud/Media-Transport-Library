/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "player.h"

int st_app_player_uinit(struct st_app_context* ctx) {
  SDL_Quit();
  return 0;
}

int st_app_player_init(struct st_app_context* ctx) {
  int res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
  if (res) {
    warn("%s, SDL_Init fail: %s\n", __func__, SDL_GetError());
    st_app_player_uinit(ctx);
    return -EIO;
  }

  return 0;
}

static void destroy_display_context(struct st_display* d) {
  if (d->texture) {
    SDL_DestroyTexture(d->texture);
    d->texture = NULL;
  }
  if (d->renderer) {
    SDL_DestroyRenderer(d->renderer);
    d->renderer = NULL;
  }
  if (d->window) {
    SDL_DestroyWindow(d->window);
    d->window = NULL;
  }
}

static int create_display_context(struct st_display* d) {
  char title[32];
  sprintf(title, "st2110-20-display-%d", d->idx);

  d->window =
      SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                       d->window_w, d->window_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (d->window == NULL) {
    err("%s, create window fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }

  d->renderer = SDL_CreateRenderer(d->window, -1, 0);
  if (d->renderer == NULL) {
    err("%s, create render fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }

  d->texture = SDL_CreateTexture(d->renderer, d->fmt, SDL_TEXTUREACCESS_STREAMING,
                                 d->pixel_w, d->pixel_h);
  if (d->texture == NULL) {
    err("%s, create texture fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }
  SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_NONE);
  return 0;
}

int st_app_display_frame(struct st_display* d, uint8_t const* frame) {
  if (d == NULL) {
    err("%s, display not initialized\n", __func__);
    return -EIO;
  }

  SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(d->renderer);

  SDL_UpdateTexture(d->texture, NULL, frame, d->pixel_w * 2);
  SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
  SDL_RenderPresent(d->renderer);
  return 0;
}

static void* display_thread_func(void* arg) {
  struct st_display* d = arg;
  int idx = d->idx;

#ifdef WINDOWSENV
  SDL_Event event;
  int ret = create_display_context(d);
  if (ret < 0) {
    err("%s, create display context fail: %d\n", __func__, ret);
    return NULL;
  }
#endif
  while (!d->st_display_thread_stop) {
    pthread_mutex_lock(&d->st_display_wake_mutex);
    if (!d->st_display_thread_stop)
      pthread_cond_wait(&d->st_display_wake_cond, &d->st_display_wake_mutex);
    pthread_mutex_unlock(&d->st_display_wake_mutex);
    st_app_display_frame(d, d->source_frame);

#ifdef WINDOWSENV
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) d->st_display_thread_stop = true;
    }
#endif
  }
  info("%s(%d), stop\n", __func__, idx);
  return NULL;
}

static int init_display_thread(struct st_display* d) {
  int ret, idx = d->idx;

  ret = pthread_create(&d->st_display_thread, NULL, display_thread_func, d);
  if (ret < 0) {
    err("%s(%d), st_display_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

int st_app_dettach_display(struct st_app_rx_video_session* video) {
  struct st_display* d = video->display;
  if (!d) return 0;
  int idx = d->idx;

  d->st_display_thread_stop = true;
  if (d->st_display_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&d->st_display_wake_mutex);
    pthread_cond_signal(&d->st_display_wake_cond);
    pthread_mutex_unlock(&d->st_display_wake_mutex);
    info("%s(%d), wait display thread stop\n", __func__, idx);
    pthread_join(d->st_display_thread, NULL);
  }

  pthread_mutex_destroy(&d->st_display_wake_mutex);
  pthread_cond_destroy(&d->st_display_wake_cond);

  destroy_display_context(d);
  if (d != NULL) {
    st_app_free(d);
    video->display = NULL;
  }
  return 0;
}

int st_app_attach_display(struct st_app_rx_video_session* video) {
  if (video->user_pg.fmt != USER_FMT_YUV_422_8BIT) {
    err("%s, format not supported\n", __func__);
    return -EINVAL;
  }

  struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
  if (!d) return -ENOMEM;
  d->idx = video->idx;
  d->window_w = 320;
  d->window_h = 180;
  d->pixel_w = video->width;
  d->pixel_h = video->height;
  d->fmt = SDL_PIXELFORMAT_UYVY;

#ifndef WINDOWSENV
  int ret = create_display_context(d);
  if (ret < 0) {
    err("%s, create display context fail: %d\n", __func__, ret);
    st_app_dettach_display(video);
    return ret;
  }
#else
  int ret;
#endif

  pthread_mutex_init(&d->st_display_wake_mutex, NULL);
  pthread_cond_init(&d->st_display_wake_cond, NULL);

  ret = init_display_thread(d);
  if (ret < 0) {
    err("%s, init_display_thread fail: %d\n", __func__, ret);
    st_app_dettach_display(video);
    return ret;
  }

  video->display = d;
  return 0;
}
