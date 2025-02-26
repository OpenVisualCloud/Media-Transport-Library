/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "player.h"

#include <SDL2/SDL_thread.h>

#define FPS_CALCULATE_INTERVAL (30)
#define SCREEN_WIDTH (640)
#define SCREEN_HEIGHT (360)
#define MSG_WIDTH (60)
#define MSG_HEIGHT (15)
#define MSG_WIDTH_MARGIN (5)
#define MSG_HEIGHT_MARGIN (5)

int st_app_player_uinit(struct st_app_context* ctx) {
  MTL_MAY_UNUSED(ctx);

  SDL_Quit();
#ifdef APP_HAS_SDL2_TTF
  TTF_Quit();
#endif
  return 0;
}

int st_app_player_init(struct st_app_context* ctx) {
  info("%s, SDL_Init start\n", __func__);
  int res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  info("%s, SDL_Init result %d\n", __func__, res);
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
  d->window =
      SDL_CreateWindow(d->name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
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

  int ret = create_display_context(d);
  if (ret < 0) {
    err("%s, create display context fail: %d\n", __func__, ret);
    return NULL;
  }

  while (!d->display_thread_stop) {
    st_pthread_mutex_lock(&d->display_wake_mutex);
    if (!d->display_thread_stop)
      st_pthread_cond_wait(&d->display_wake_cond, &d->display_wake_mutex);
    st_pthread_mutex_unlock(&d->display_wake_mutex);

    /* calculate fps*/
    if (d->frame_cnt % FPS_CALCULATE_INTERVAL == 0) {
      uint32_t time = SDL_GetTicks();
      d->fps = 1000.0 * FPS_CALCULATE_INTERVAL / (time - d->last_time);
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
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      dbg("%s, SDL event: %d\n", __func__, event.type);
      if (event.type == SDL_QUIT) d->display_thread_stop = true;
    }
#endif
  }
  info("%s(%s), stop\n", __func__, d->name);
  return NULL;
}

int st_app_uinit_display(struct st_display* d) {
  if (!d) return 0;

  d->display_thread_stop = true;
  if (d->display_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&d->display_wake_mutex);
    st_pthread_cond_signal(&d->display_wake_cond);
    st_pthread_mutex_unlock(&d->display_wake_mutex);
    info("%s(%s), wait display thread stop\n", __func__, d->name);
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

int st_app_init_display(struct st_display* d, char* name, int width, int height,
                        char* font) {
  int ret;
  MTL_MAY_UNUSED(font);

  if (!d) return -ENOMEM;
  snprintf(d->name, 32, "%s", name);
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

  st_pthread_mutex_init(&d->display_wake_mutex, NULL);
  st_pthread_mutex_init(&d->display_frame_mutex, NULL);
  st_pthread_cond_init(&d->display_wake_cond, NULL);

  ret = pthread_create(&d->display_thread, NULL, display_thread_func, d);
  if (ret < 0) {
    err("%s(%s), create display thread fail: %d\n", __func__, d->name, ret);
    st_app_uinit_display(d);
    return ret;
  }

  info("%s(%s), succ, pixel width: %d, height: %d\n", __func__, name, width, height);
  return 0;
}
