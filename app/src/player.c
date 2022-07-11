/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "player.h"

#define FPS_CALCULATE_INTERVEL (30)
#define SCREEN_WIDTH (640)
#define SCREEN_HEIGHT (360)
#define MSG_WIDTH (60)
#define MSG_HEIGHT (15)
#define MSG_WIDTH_MARGIN (5)
#define MSG_HEIGHT_MARGIN (5)

int st_app_player_uinit(struct st_app_context* ctx) {
  SDL_Quit();
#ifdef APP_HAS_SDL2_TTF
  TTF_Quit();
#endif
  return 0;
}

int st_app_player_init(struct st_app_context* ctx) {
  int res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
  if (res) {
    warn("%s, SDL_Init fail: %s\n", __func__, SDL_GetError());
    st_app_player_uinit(ctx);
    return -EIO;
  }

#ifdef APP_HAS_SDL2_TTF
  res = TTF_Init();
  if (res) {
    warn("%s, TTF_Init fail: %s\n", __func__, TTF_GetError());
    st_app_player_uinit(ctx);
    return -EIO;
  }
#endif

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

  while (!d->display_thread_stop) {
    st_pthread_mutex_lock(&d->display_wake_mutex);
    if (!d->display_thread_stop)
      st_pthread_cond_wait(&d->display_wake_cond, &d->display_wake_mutex);
    st_pthread_mutex_unlock(&d->display_wake_mutex);

    /* calculate fps*/
    if (d->frame_cnt % FPS_CALCULATE_INTERVEL == 0) {
      uint32_t time = SDL_GetTicks();
      d->fps = 1000.0 * FPS_CALCULATE_INTERVEL / (time - d->last_time);
      d->last_time = time;
    }
    d->frame_cnt++;

    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(d->renderer);

    st_pthread_mutex_lock(&d->display_frame_mutex);
    SDL_UpdateTexture(d->texture, NULL, d->front_frame, d->pixel_w * 2);
    st_pthread_mutex_unlock(&d->display_frame_mutex);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);

#ifdef APP_HAS_SDL2_TTF
    /* display info */
    if (d->font) {
      char text[32];
      sprintf(text, "FPS:\t%.2f", d->fps);
      SDL_Color Red = {255, 0, 0};
      SDL_Surface* surfaceMessage = TTF_RenderText_Solid(d->font, text, Red);
      SDL_Texture* Message = SDL_CreateTextureFromSurface(d->renderer, surfaceMessage);

      SDL_RenderCopy(d->renderer, Message, NULL, &d->msg_rect);
      SDL_FreeSurface(surfaceMessage);
      SDL_DestroyTexture(Message);
    }
#endif

    SDL_RenderPresent(d->renderer);
#ifdef WINDOWSENV
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) d->display_thread_stop = true;
    }
#endif
  }
  info("%s(%d), stop\n", __func__, idx);
  return NULL;
}

int st_app_uinit_display(struct st_display* d) {
  if (!d) return 0;
  int idx = d->idx;

  d->display_thread_stop = true;
  if (d->display_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&d->display_wake_mutex);
    st_pthread_cond_signal(&d->display_wake_cond);
    st_pthread_mutex_unlock(&d->display_wake_mutex);
    info("%s(%d), wait display thread stop\n", __func__, idx);
    pthread_join(d->display_thread, NULL);
  }
  st_pthread_mutex_destroy(&d->display_wake_mutex);
  st_pthread_mutex_destroy(&d->display_frame_mutex);
  st_pthread_cond_destroy(&d->display_wake_cond);

  destroy_display_context(d);

#ifdef APP_HAS_SDL2_TTF
  if (d->font) {
    TTF_CloseFont(d->font);
    d->font = NULL;
  }
#endif

  if (d->front_frame) {
    st_app_free(d->front_frame);
    d->front_frame = NULL;
  }

  return 0;
}

int st_app_init_display(struct st_display* d, int idx, int width, int height,
                        char* font) {
  int ret;
  if (!d) return -ENOMEM;
  d->idx = idx;
  d->window_w = SCREEN_WIDTH;
  d->window_h = SCREEN_HEIGHT;
  d->pixel_w = width;
  d->pixel_h = height;
  d->fmt = SDL_PIXELFORMAT_UYVY;
#ifdef APP_HAS_SDL2_TTF
  d->font = TTF_OpenFont(font, 40);
  if (!d->font)
    warn("%s, open font fail, won't show info: %s\n", __func__, TTF_GetError());
#endif
  if (d->fmt == SDL_PIXELFORMAT_UYVY) {
    d->front_frame_size = width * height * 2;
  } else {
    err("%s, unsupported pixel format %d\n", __func__, d->fmt);
    return -EIO;
  }

  d->front_frame = st_app_zmalloc(d->front_frame_size);
  if (!d->front_frame) {
    err("%s, alloc front frame fail\n", __func__);
    return -ENOMEM;
  }

  d->msg_rect.w = MSG_WIDTH;
  d->msg_rect.h = MSG_HEIGHT;
  d->msg_rect.x = MSG_WIDTH_MARGIN;
  d->msg_rect.y = SCREEN_HEIGHT - MSG_HEIGHT - MSG_HEIGHT_MARGIN;

#ifndef WINDOWSENV
  ret = create_display_context(d);
  if (ret < 0) {
    err("%s, create display context fail: %d\n", __func__, ret);
    st_app_uinit_display(d);
    return ret;
  }
#endif

  st_pthread_mutex_init(&d->display_wake_mutex, NULL);
  st_pthread_mutex_init(&d->display_frame_mutex, NULL);
  st_pthread_cond_init(&d->display_wake_cond, NULL);

  ret = pthread_create(&d->display_thread, NULL, display_thread_func, d);
  if (ret < 0) {
    err("%s(%d), create display thread fail: %d\n", __func__, idx, ret);
    st_app_uinit_display(d);
    return ret;
  }

  return 0;
}
