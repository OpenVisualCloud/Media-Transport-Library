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

struct rfc4175_422_8_pg2 {
  uint8_t Cb00;
  uint8_t Y00;
  uint8_t Cr00;
  uint8_t Y01;
} __attribute__((__packed__));

typedef struct rfc4175_422_8_pg2 rfc4175_422_8_pg2_t;

struct rfc4175_422_10_pg2 {
  uint8_t Cb00; /**< Blue-difference chrominance (8-bits) */
#ifdef ST_LITTLE_ENDIAN
  uint8_t Y00 : 6;   /**< First Luminance (6-bits) */
  uint8_t Cb00_ : 2; /**< Blue-difference chrominance (2-bits)
                                             as complement to the 10-bits */

  uint8_t Cr00 : 4; /**< Red-difference chrominance */
  uint8_t Y00_ : 4; /**< First Luminance (4-bits) as complement
                                            to the 10-bits */

  uint8_t Y01 : 2;   /**< Second Luminance (2-bits) */
  uint8_t Cr00_ : 6; /**< Red-difference chrominance (6-bits)
                                             as complement to the 10-bits */
#else
  uint8_t Cb00_ : 2;
  uint8_t Y00 : 6;

  uint8_t Y00_ : 4;
  uint8_t Cr00 : 4;

  uint8_t Cr00_ : 6;
  uint8_t Y01 : 2;
#endif
  uint8_t Y01_; /**< Secoond Luminance (8-bits)
                                        as complement to the 10-bits*/
} __attribute__((__packed__));

typedef struct rfc4175_422_10_pg2 rfc4175_422_10_pg2_t;

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

  d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_UYVY,
                                 SDL_TEXTUREACCESS_STREAMING, d->pixel_w, d->pixel_h);
  if (d->texture == NULL) {
    err("%s, create texture fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }
  SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_NONE);
  return 0;
}

static void convert_uyvy10b_to_uyvy8b(uint8_t* yuv_8b, uint8_t const* yuv_10b, int width,
                                      int height) {
  rfc4175_422_8_pg2_t* p_8 = (rfc4175_422_8_pg2_t*)yuv_8b;
  rfc4175_422_10_pg2_t* p_10 = (rfc4175_422_10_pg2_t*)yuv_10b;

  for (int i = 0; i < width * height / 2; i++) {
    p_8[i].Cb00 = p_10[i].Cb00;
    p_8[i].Y00 = (p_10[i].Y00 << 2) + (p_10[i].Y00_ >> 2);
    p_8[i].Cr00 = (p_10[i].Cr00 << 4) + (p_10[i].Cr00_ >> 2);
    p_8[i].Y01 = (p_10[i].Y01 << 6) + (p_10[i].Y01_ >> 2);
  }
}

int st_app_display_frame(struct st_display* d, uint8_t const* frame) {
  if (d == NULL) {
    err("%s, display not initialized\n", __func__);
    return -EIO;
  }
  if (d->display_frame == NULL) {
    err("%s, display frame not initialized\n", __func__);
    return -EIO;
  }

  SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(d->renderer);

  convert_uyvy10b_to_uyvy8b(d->display_frame, frame, d->pixel_w, d->pixel_h);

  SDL_UpdateTexture(d->texture, NULL, d->display_frame, d->pixel_w * 2);
  SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
  SDL_RenderPresent(d->renderer);
  return 0;
}

static void* display_thread_func(void* arg) {
  struct st_display* d = arg;
  int idx = d->idx;
  while (!d->st_dispaly_thread_stop) {
    pthread_mutex_lock(&d->st_dispaly_wake_mutex);
    if (!d->st_dispaly_thread_stop)
      pthread_cond_wait(&d->st_dispaly_wake_cond, &d->st_dispaly_wake_mutex);
    pthread_mutex_unlock(&d->st_dispaly_wake_mutex);
    st_app_display_frame(d, d->source_frame);
  }
  info("%s(%d), stop\n", __func__, idx);
  return NULL;
}

static int init_display_thread(struct st_display* d) {
  int ret, idx = d->idx;

  ret = pthread_create(&d->st_dispaly_thread, NULL, display_thread_func, d);
  if (ret < 0) {
    err("%s(%d), st_dispaly_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

int st_app_dettach_display(struct st_app_rx_video_session* video) {
  struct st_display* d = video->display;
  if (!d) return 0;
  int idx = d->idx;

  d->st_dispaly_thread_stop = true;
  if (d->st_dispaly_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&d->st_dispaly_wake_mutex);
    pthread_cond_signal(&d->st_dispaly_wake_cond);
    pthread_mutex_unlock(&d->st_dispaly_wake_mutex);
    info("%s(%d), wait display thread stop\n", __func__, idx);
    pthread_join(d->st_dispaly_thread, NULL);
  }

  pthread_mutex_destroy(&d->st_dispaly_wake_mutex);
  pthread_cond_destroy(&d->st_dispaly_wake_cond);

  destroy_display_context(d);
  if (d != NULL) {
    if (d->display_frame != NULL) {
      st_app_free(d->display_frame);
      d->display_frame = NULL;
    }
    if (d->source_frame != NULL) {
      st_app_free(d->source_frame);
      d->source_frame = NULL;
    }
    st_app_free(d);
    video->display = NULL;
  }
  return 0;
}

int st_app_attach_display(struct st_app_rx_video_session* video) {
  struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
  if (!d) return -ENOMEM;

  d->idx = video->idx;
  d->window_w = 320;
  d->window_h = 180;
  d->pixel_w = video->width;
  d->pixel_h = video->height;

  int ret = create_display_context(d);
  if (ret < 0) {
    err("%s, create display context fail: %d\n", __func__, ret);
    st_app_dettach_display(video);
    return ret;
  }

  size_t frame_size = d->pixel_w * d->pixel_h * 2;
  d->display_frame = st_app_zmalloc(frame_size);
  if (!d->display_frame) return -ENOMEM;
  d->source_frame = st_app_zmalloc(frame_size * 10 / 8);
  if (!d->source_frame) {
    st_app_free(d->display_frame);
    return -ENOMEM;
  }

  pthread_mutex_init(&d->st_dispaly_wake_mutex, NULL);
  pthread_cond_init(&d->st_dispaly_wake_cond, NULL);

  ret = init_display_thread(d);
  if (ret < 0) {
    err("%s, init_display_thread fail: %d\n", __func__, ret);
    st_app_dettach_display(video);
    return ret;
  }

  video->display = d;
  return 0;
}
