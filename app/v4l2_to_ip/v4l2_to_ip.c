/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#define __STDC_FORMAT_MACROS

#define _GNU_SOURCE /* See feature_test_macros(7) */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*st20 s*/
#include <mtl/st20_api.h>

/*play*/
#include <SDL2/SDL.h>
#ifdef APP_HAS_SDL2_TTF
#include <SDL2/SDL_ttf.h>
#endif
#include <SDL2/SDL_thread.h>

#define TX_EXT_FRAME

#define TX_VIDEO_PMD MTL_PMD_DPDK_USER
#define TX_VIDEO_PORT_BDF "0000:01:00.0"

#define TX_VIDEO_DST_MAC_ADDR "00:a0:c9:00:00:02"

#define V4L_BUFFERS_DEFAULT 8
#define V4L_BUFFERS_MAX 32

#define TX_VIDEO_LCORE "2,3"
#define TX_VIDEO_UDP_PORT (50000)
#define TX_VIDEO_PAYLOAD_TYPE (112)

#define V4L2_TX_THREAD_CORE 1

#ifndef V4L2_BUF_FLAG_ERROR
#define V4L2_BUF_FLAG_ERROR 0x0040
#endif

#ifndef VIDIOC_IPU_GET_DRIVER_VERSION
#define BASE_VIDIOC_PRIVATE 192 /* 192-255 are private */
#define VIDIOC_IPU_GET_DRIVER_VERSION _IOWR('v', BASE_VIDIOC_PRIVATE + 3, uint32_t)
#endif

#define V4L2_FMT_WIDTH (1920)
#define V4L2_FMT_HEIGHT (1080)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define FPS_CALCULATE_INTERVEL (30)
#define SCREEN_WIDTH (640)
#define SCREEN_HEIGHT (360)
#define MSG_WIDTH (60)
#define MSG_HEIGHT (15)
#define MSG_WIDTH_MARGIN (5)
#define MSG_HEIGHT_MARGIN (5)

#define DISPLAY_THREAD_CORE 0
#define APP_URL_MAX_LEN (256)

/*struct*/

enum tx_frame_status {
  TX_FRAME_FREE = 0,
  TX_FRAME_READY,
  TX_FRAME_RECEIVING,
  TX_FRAME_TRANSMITTING,
  TX_FRAME_STATUS_MAX,
};

struct tx_frame_buff {
  enum tx_frame_status status;
  unsigned int size;
  struct timespec v4l2_ts;
  struct timespec app_ts;
  struct timespec st20_ts;
};

struct tx_frame_buff_ct {
  struct tx_frame_buff *buffs;
  unsigned int cnt;
  unsigned int receive_idx;
  unsigned int ready_idx;
  unsigned int transmit_idx;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

struct st_display {
  int idx;
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  SDL_PixelFormatEnum fmt;
#ifdef APP_HAS_SDL2_TTF
  TTF_Font *font;
#endif
  SDL_Rect msg_rect;
  int window_w;
  int window_h;
  int pixel_w;
  int pixel_h;
  void *front_frame;
  int front_frame_size;
  uint32_t last_time;
  uint32_t frame_cnt;
  double fps;

  pthread_t display_thread;
  bool display_thread_stop;
  pthread_cond_t display_wake_cond;
  pthread_mutex_t display_wake_mutex;
  pthread_mutex_t display_frame_mutex;
};

struct st_v4l2_tx_video_session {
  int idx;
  struct st_v4l2_tx_context *ctx;

  st20_tx_handle handle;
  struct st20_tx_ops ops_tx;

  int framebuff_size;

  struct st20_ext_frame *ext_frames;

  int width;
  int height;

  uint32_t st20_frame_done_cnt;

  pthread_t st20_app_thread;

  struct tx_frame_buff_ct framebuff_ctl;

  struct st_display display;
};

enum buffer_fill_mode {
  BUFFER_FILL_NONE = 0,
  BUFFER_FILL_FRAME = 1 << 0,
  BUFFER_FILL_PADDING = 1 << 1,
};

struct buffer {
  unsigned int idx;
  unsigned int padding[VIDEO_MAX_PLANES];
  unsigned int size[VIDEO_MAX_PLANES];
  void *mem[VIDEO_MAX_PLANES];
};

struct device {
  int fd;
  int opened;

  enum v4l2_buf_type type;
  enum v4l2_memory memtype;
  unsigned int nbufs;
  struct buffer *buffers;

  unsigned int width;
  unsigned int height;
  uint32_t buffer_output_flags;
  uint32_t buffer_qbuf_flags;
  uint32_t buffer_dqbuf_flags;
  uint32_t timestamp_type;

  unsigned char num_planes;
  struct v4l2_plane_pix_format plane_fmt[VIDEO_MAX_PLANES];

  void *pattern[VIDEO_MAX_PLANES];
  unsigned int patternsize[VIDEO_MAX_PLANES];

  bool write_data_prefix;
};

struct st_v4l2_tx_context {
  struct mtl_init_params param;
  mtl_handle st;

  bool stop;

  struct st_v4l2_tx_video_session *tx_video_sessions;
  int tx_video_session_cnt;

  struct device dev;

  unsigned int nframes;

  bool skip;

  enum buffer_fill_mode fill_mode;

  unsigned int dqbuf_cnt;

  bool has_sdl; /* has SDL device or not*/

  char ttf_file[APP_URL_MAX_LEN];
};

/*globle var*/

/* local ip address for current bdf port */
static uint8_t g_tx_video_local_ip[MTL_IP_ADDR_LEN] = {192, 168, 22, 85};
/* dst ip address for tx video session */
static uint8_t g_tx_video_dst_ip[MTL_IP_ADDR_LEN] = {239, 168, 22, 85};

static struct st_v4l2_tx_context *g_st_v4l2_tx = NULL;

/*function dec*/

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *cpuset);

/*code*/

static int video_set_realtime(pthread_t thread, int priority, int cpu) {
  cpu_set_t cpuset;
  struct sched_param sp;
  int err, policy;

  if (priority < 0) return -1;

  err = pthread_getschedparam(thread, &policy, &sp);
  if (err) return -1;

  sp.sched_priority = priority;

  err = pthread_setschedparam(thread, SCHED_FIFO, &sp);
  if (err) return -1;

  if (cpu < 0) return -1;

  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (err) return -1;

  return 0;
}

static int app_player_uinit(void) {
  SDL_Quit();
#ifdef APP_HAS_SDL2_TTF
  TTF_Quit();
#endif
  return 0;
}

static int app_player_init(void) {
  int res;

  res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
  if (res) {
    printf("%s, SDL_Init fail: %s\n", __func__, SDL_GetError());
    app_player_uinit();
    return -EIO;
  }

#ifdef APP_HAS_SDL2_TTF
  res = TTF_Init();
  if (res) {
    printf("%s, TTF_Init fail: %s\n", __func__, TTF_GetError());
    app_player_uinit();
    return -EIO;
  }
#endif

  return 0;
}

static void destroy_display_context(struct st_display *d) {
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

static int create_display_context(struct st_display *d) {
  char title[32];
  sprintf(title, "v4l2-display-%d", d->idx);

  d->window =
      SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                       d->window_w, d->window_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (d->window == NULL) {
    printf("%s, create window fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }

  d->renderer = SDL_CreateRenderer(d->window, -1, 0);
  if (d->renderer == NULL) {
    printf("%s, create render fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }

  d->texture = SDL_CreateTexture(d->renderer, d->fmt, SDL_TEXTUREACCESS_STREAMING,
                                 d->pixel_w, d->pixel_h);
  if (d->texture == NULL) {
    printf("%s, create texture fail: %s\n", __func__, SDL_GetError());
    destroy_display_context(d);
    return -EIO;
  }
  SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_NONE);
  return 0;
}

static void *display_thread_func(void *arg) {
  struct st_display *d = arg;
  int idx = d->idx;

#ifdef WINDOWSENV
  SDL_Event event;
  int ret = create_display_context(d);
  if (ret < 0) {
    printf("%s, create display context fail: %d\n", __func__, ret);
    return NULL;
  }
#endif

  while (!d->display_thread_stop) {
    pthread_mutex_lock(&d->display_wake_mutex);
    if (!d->display_thread_stop)
      pthread_cond_wait(&d->display_wake_cond, &d->display_wake_mutex);
    pthread_mutex_unlock(&d->display_wake_mutex);

    /* calculate fps*/
    if (d->frame_cnt % FPS_CALCULATE_INTERVEL == 0) {
      uint32_t time = SDL_GetTicks();
      d->fps = 1000.0 * FPS_CALCULATE_INTERVEL / (time - d->last_time);
      d->last_time = time;
    }
    d->frame_cnt++;

    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(d->renderer);

    pthread_mutex_lock(&d->display_frame_mutex);
    SDL_UpdateTexture(d->texture, NULL, d->front_frame, d->pixel_w * 2);
    pthread_mutex_unlock(&d->display_frame_mutex);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);

#ifdef APP_HAS_SDL2_TTF
    /* display info */
    if (d->font) {
      char text[32];
      sprintf(text, "FPS:\t%.2f", d->fps);
      SDL_Color Red = {255, 0, 0};
      SDL_Surface *surfaceMessage = TTF_RenderText_Solid(d->font, text, Red);
      SDL_Texture *Message = SDL_CreateTextureFromSurface(d->renderer, surfaceMessage);

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
  printf("%s(%d), stop\n", __func__, idx);
  return NULL;
}

static int display_thread_create(struct st_v4l2_tx_video_session *tx_video_session,
                                 unsigned int priority, unsigned int cpu) {
  int ret = 0;

  if (pthread_create(&(tx_video_session->display.display_thread), NULL,
                     display_thread_func, &(tx_video_session->display))) {
    printf("%s pthread_create Failed: %s (%d)\n", __func__, strerror(errno), errno);
    ret = -EIO;
    return ret;
  }

  if (video_set_realtime(tx_video_session->display.display_thread, priority, cpu) < 0) {
    printf("%s video_set_realtime Failed\n", __func__);
    ret = -EIO;
    return ret;
  }

  return ret;
}

static void display_consume_frame(struct st_v4l2_tx_video_session *tx_video_session,
                                  void *frame) {
  struct st_display *display = &(tx_video_session->display);

  if (display->front_frame) {
    if (pthread_mutex_trylock(&display->display_frame_mutex) == 0) {
      mtl_memcpy(display->front_frame, frame, display->front_frame_size);
      pthread_mutex_unlock(&display->display_frame_mutex);

      pthread_mutex_lock(&display->display_wake_mutex);
      pthread_cond_signal(&display->display_wake_cond);
      pthread_mutex_unlock(&display->display_wake_mutex);
    }
  }
}

static int app_uinit_display(struct st_display *d) {
  if (!d) return 0;
  int idx = d->idx;

  d->display_thread_stop = true;
  if (d->display_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&d->display_wake_mutex);
    pthread_cond_signal(&d->display_wake_cond);
    pthread_mutex_unlock(&d->display_wake_mutex);
    printf("%s(%d), wait display thread stop\n", __func__, idx);
    pthread_join(d->display_thread, NULL);
  }
  pthread_mutex_destroy(&d->display_wake_mutex);
  pthread_mutex_destroy(&d->display_frame_mutex);
  pthread_cond_destroy(&d->display_wake_cond);

  destroy_display_context(d);

#ifdef APP_HAS_SDL2_TTF
  if (d->font) {
    TTF_CloseFont(d->font);
    d->font = NULL;
  }
#endif

  if (d->front_frame) {
    free(d->front_frame);
    d->front_frame = NULL;
  }

  return 0;
}

static int app_init_display(struct st_display *d, int idx, int width, int height,
                            char *font) {
  int ret;
  if (!d) return -ENOMEM;
  MTL_MAY_UNUSED(font);

  d->idx = idx;
  d->window_w = SCREEN_WIDTH;
  d->window_h = SCREEN_HEIGHT;
  d->pixel_w = width;
  d->pixel_h = height;
  d->fmt = SDL_PIXELFORMAT_UYVY;
#ifdef APP_HAS_SDL2_TTF
  d->font = TTF_OpenFont(font, 40);
  if (!d->font)
    printf("%s, open font fail, won't show info: %s\n", __func__, TTF_GetError());
#endif
  if (d->fmt == SDL_PIXELFORMAT_UYVY) {
    d->front_frame_size = width * height * 2;
  } else {
    printf("%s, unsupported pixel format %d\n", __func__, d->fmt);
    return -EIO;
  }

  d->front_frame = malloc(d->front_frame_size);
  if (!d->front_frame) {
    printf("%s, alloc front frame fail\n", __func__);
    return -ENOMEM;
  }
  memset(d->front_frame, 0, d->front_frame_size);

  d->msg_rect.w = MSG_WIDTH;
  d->msg_rect.h = MSG_HEIGHT;
  d->msg_rect.x = MSG_WIDTH_MARGIN;
  d->msg_rect.y = SCREEN_HEIGHT - MSG_HEIGHT - MSG_HEIGHT_MARGIN;

#ifndef WINDOWSENV
  ret = create_display_context(d);
  if (ret < 0) {
    printf("%s, create display context fail: %d\n", __func__, ret);
    app_uinit_display(d);
    return ret;
  }
#endif

  pthread_mutex_init(&d->display_wake_mutex, NULL);
  pthread_mutex_init(&d->display_frame_mutex, NULL);
  pthread_cond_init(&d->display_wake_cond, NULL);

  return 0;
}

/**/

static bool video_is_mplane(struct device *dev) {
  return dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
         dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static bool video_is_capture(struct device *dev) {
  return dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
         dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static bool video_is_output(struct device *dev) {
  return dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
         dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

static struct v4l2_format_info {
  const char *name;
  unsigned int fourcc;
  unsigned char n_planes;
} pixel_formats[] = {
    {"RGB332", V4L2_PIX_FMT_RGB332, 1},
    {"RGB444", V4L2_PIX_FMT_RGB444, 1},
    {"ARGB444", V4L2_PIX_FMT_ARGB444, 1},
    {"XRGB444", V4L2_PIX_FMT_XRGB444, 1},
    {"RGB555", V4L2_PIX_FMT_RGB555, 1},
    {"ARGB555", V4L2_PIX_FMT_ARGB555, 1},
    {"XRGB555", V4L2_PIX_FMT_XRGB555, 1},
    {"RGB565", V4L2_PIX_FMT_RGB565, 1},
    {"RGB555X", V4L2_PIX_FMT_RGB555X, 1},
    {"RGB565X", V4L2_PIX_FMT_RGB565X, 1},
    {"BGR666", V4L2_PIX_FMT_BGR666, 1},
    {"BGR24", V4L2_PIX_FMT_BGR24, 1},
    {"RGB24", V4L2_PIX_FMT_RGB24, 1},
    {"BGR32", V4L2_PIX_FMT_BGR32, 1},
    {"ABGR32", V4L2_PIX_FMT_ABGR32, 1},
    {"XBGR32", V4L2_PIX_FMT_XBGR32, 1},
    {"RGB32", V4L2_PIX_FMT_RGB32, 1},
    {"ARGB32", V4L2_PIX_FMT_ARGB32, 1},
    {"XRGB32", V4L2_PIX_FMT_XRGB32, 1},
    {"Y8", V4L2_PIX_FMT_GREY, 1},
    {"Y10", V4L2_PIX_FMT_Y10, 1},
    {"Y12", V4L2_PIX_FMT_Y12, 1},
    {"Y16", V4L2_PIX_FMT_Y16, 1},
    {"UYVY", V4L2_PIX_FMT_UYVY, 1},
    {"VYUY", V4L2_PIX_FMT_VYUY, 1},
    {"YUYV", V4L2_PIX_FMT_YUYV, 1},
    {"YVYU", V4L2_PIX_FMT_YVYU, 1},
    {"NV12", V4L2_PIX_FMT_NV12, 1},
    {"NV12M", V4L2_PIX_FMT_NV12M, 2},
    {"NV21", V4L2_PIX_FMT_NV21, 1},
    {"NV21M", V4L2_PIX_FMT_NV21M, 2},
    {"NV16", V4L2_PIX_FMT_NV16, 1},
    {"NV16M", V4L2_PIX_FMT_NV16M, 2},
    {"NV61", V4L2_PIX_FMT_NV61, 1},
    {"NV61M", V4L2_PIX_FMT_NV61M, 2},
    {"NV24", V4L2_PIX_FMT_NV24, 1},
    {"NV42", V4L2_PIX_FMT_NV42, 1},
    {"YUV420M", V4L2_PIX_FMT_YUV420M, 3},
    {"YUV420", V4L2_PIX_FMT_YUV420, 3},
    //	{ "YUYV420_V32", V4L2_PIX_FMT_YUYV420_V32, 3 },
    {"SBGGR8", V4L2_PIX_FMT_SBGGR8, 1},
    {"SGBRG8", V4L2_PIX_FMT_SGBRG8, 1},
    {"SGRBG8", V4L2_PIX_FMT_SGRBG8, 1},
    {"SRGGB8", V4L2_PIX_FMT_SRGGB8, 1},
    //	{ "SBGGR8_16V32", V4L2_PIX_FMT_SBGGR8_16V32, 1 },
    //	{ "SGBRG8_16V32", V4L2_PIX_FMT_SGBRG8_16V32, 1 },
    //	{ "SGRBG8_16V32", V4L2_PIX_FMT_SGRBG8_16V32, 1 },
    //	{ "SRGGB8_16V32", V4L2_PIX_FMT_SRGGB8_16V32, 1 },
    {"SBGGR10_DPCM8", V4L2_PIX_FMT_SBGGR10DPCM8, 1},
    {"SGBRG10_DPCM8", V4L2_PIX_FMT_SGBRG10DPCM8, 1},
    {"SGRBG10_DPCM8", V4L2_PIX_FMT_SGRBG10DPCM8, 1},
    {"SRGGB10_DPCM8", V4L2_PIX_FMT_SRGGB10DPCM8, 1},
    {"SBGGR10", V4L2_PIX_FMT_SBGGR10, 1},
    {"SGBRG10", V4L2_PIX_FMT_SGBRG10, 1},
    {"SGRBG10", V4L2_PIX_FMT_SGRBG10, 1},
    {"SRGGB10", V4L2_PIX_FMT_SRGGB10, 1},
    {"SBGGR10P", V4L2_PIX_FMT_SBGGR10P, 1},
    {"SGBRG10P", V4L2_PIX_FMT_SGBRG10P, 1},
    {"SGRBG10P", V4L2_PIX_FMT_SGRBG10P, 1},
    {"SRGGB10P", V4L2_PIX_FMT_SRGGB10P, 1},
    //	{ "SBGGR10V32", V4L2_PIX_FMT_SBGGR10V32, 1 },
    //	{ "SGBRG10V32", V4L2_PIX_FMT_SGBRG10V32, 1 },
    //	{ "SGRBG10V32", V4L2_PIX_FMT_SGRBG10V32, 1 },
    //	{ "SRGGB10V32", V4L2_PIX_FMT_SRGGB10V32, 1 },
    {"SBGGR12", V4L2_PIX_FMT_SBGGR12, 1},
    {"SGBRG12", V4L2_PIX_FMT_SGBRG12, 1},
    {"SGRBG12", V4L2_PIX_FMT_SGRBG12, 1},
    {"SRGGB12", V4L2_PIX_FMT_SRGGB12, 1},
    //	{ "SBGGR12V32", V4L2_PIX_FMT_SBGGR12V32, 1 },
    //	{ "SGBRG12V32", V4L2_PIX_FMT_SGBRG12V32, 1 },
    //	{ "SGRBG12V32", V4L2_PIX_FMT_SGRBG12V32, 1 },
    //	{ "SRGGB12V32", V4L2_PIX_FMT_SRGGB12V32, 1 },
    {"DV", V4L2_PIX_FMT_DV, 1},
    {"MJPEG", V4L2_PIX_FMT_MJPEG, 1},
    {"MPEG", V4L2_PIX_FMT_MPEG, 1},
    //	{ "META", V4L2_FMT_INTEL_IPU4_ISYS_META, 1 },
    //	{ "Y210", V4L2_PIX_FMT_Y210, 1 },
};

static const struct v4l2_format_info *v4l2_format_by_fourcc(unsigned int fourcc) {
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
    if (pixel_formats[i].fourcc == fourcc) return &pixel_formats[i];
  }

  return NULL;
}

static const char *v4l2_format_name(unsigned int fourcc) {
  const struct v4l2_format_info *info;
  static char name[5];
  unsigned int i;

  info = v4l2_format_by_fourcc(fourcc);
  if (info) return info->name;

  for (i = 0; i < 4; ++i) {
    name[i] = fourcc & 0xff;
    fourcc >>= 8;
  }

  name[4] = '\0';
  return name;
}

static const struct {
  const char *name;
  enum v4l2_field field;
} fields[] = {
    {"any", V4L2_FIELD_ANY},
    {"none", V4L2_FIELD_NONE},
    {"top", V4L2_FIELD_TOP},
    {"bottom", V4L2_FIELD_BOTTOM},
    {"interlaced", V4L2_FIELD_INTERLACED},
    {"seq-tb", V4L2_FIELD_SEQ_TB},
    {"seq-bt", V4L2_FIELD_SEQ_BT},
    {"alternate", V4L2_FIELD_ALTERNATE},
    {"interlaced-tb", V4L2_FIELD_INTERLACED_TB},
    {"interlaced-bt", V4L2_FIELD_INTERLACED_BT},
};

static const char *v4l2_field_name(enum v4l2_field field) {
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE(fields); ++i) {
    if (fields[i].field == field) return fields[i].name;
  }

  return "unknown";
}

static void video_set_buf_type(struct device *dev, enum v4l2_buf_type type) {
  dev->type = type;
}

static void video_init(struct device *dev) {
  memset(dev, 0, sizeof *dev);
  dev->fd = -1;
  dev->memtype = V4L2_MEMORY_MMAP;
  dev->buffers = NULL;
  dev->type = (enum v4l2_buf_type) - 1;
}

static bool video_has_fd(struct device *dev) {
  return dev->fd != -1;
}

static int video_open(struct device *dev, const char *devname) {
  if (video_has_fd(dev)) {
    printf("Can't open device (already open).\n");
    return -1;
  }

  dev->fd = open(devname, O_RDWR);
  if (dev->fd < 0) {
    printf("Error opening device %s: %s (%d).\n", devname, strerror(errno), errno);
    return dev->fd;
  }

  printf("Device %s opened.\n", devname);

  dev->opened = 1;

  return 0;
}

static int do_print_ipu_version(struct device *dev) {
  unsigned int version;
  int ret;

  ret = ioctl(dev->fd, VIDIOC_IPU_GET_DRIVER_VERSION, &version);
  if (ret < 0) return 0;

  printf("IPU driver version: %d.%d\n", version >> 16, version & 0xFFFF);

  return 0;
}

static int video_querycap(struct device *dev, unsigned int *capabilities) {
  struct v4l2_capability cap;
  unsigned int caps;
  int ret;

  memset(&cap, 0, sizeof cap);
  ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &cap);
  if (ret < 0) return 0;

  caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;

  printf(
      "Device `%s' on `%s' is a video %s (%s mplanes) device.\n", cap.card, cap.bus_info,
      caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_CAPTURE) ? "capture"
                                                                      : "output",
      caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE) ? "with"
                                                                            : "without");

  *capabilities = caps;

  return 0;
}

static int cap_get_buf_type(unsigned int capabilities) {
  if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
    return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  } else if (capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) {
    return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  } else if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {
    return V4L2_BUF_TYPE_VIDEO_CAPTURE;
  } else if (capabilities & V4L2_CAP_VIDEO_OUTPUT) {
    return V4L2_BUF_TYPE_VIDEO_OUTPUT;
  } else {
    printf("Device supports neither capture nor output.\n");
    return -EINVAL;
  }

  return 0;
}

static void video_close(struct device *dev) {
  unsigned int i;

  for (i = 0; i < dev->num_planes; i++) free(dev->pattern[i]);

  free(dev->buffers);
  if (dev->opened) close(dev->fd);
}

static void video_log_status(struct device *dev) {
  int ret;
  ret = ioctl(dev->fd, VIDIOC_LOG_STATUS);
  if (ret < 0) {
    printf("Failed to log status: %s (%d).\n", strerror(errno), errno);
  }
}

static int video_get_format(struct device *dev) {
  struct v4l2_format v_fmt;
  unsigned int i;
  int ret;

  memset(&v_fmt, 0, sizeof v_fmt);
  v_fmt.type = dev->type;

  ret = ioctl(dev->fd, VIDIOC_G_FMT, &v_fmt);
  if (ret < 0) {
    return ret;
  }

  if (video_is_mplane(dev)) {
    dev->width = v_fmt.fmt.pix_mp.width;
    dev->height = v_fmt.fmt.pix_mp.height;
    dev->num_planes = v_fmt.fmt.pix_mp.num_planes;

    for (i = 0; i < v_fmt.fmt.pix_mp.num_planes; i++) {
      dev->plane_fmt[i].bytesperline = v_fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
      dev->plane_fmt[i].sizeimage = v_fmt.fmt.pix_mp.plane_fmt[i].bytesperline
                                        ? v_fmt.fmt.pix_mp.plane_fmt[i].sizeimage
                                        : 0;
    }
  } else {
    dev->width = v_fmt.fmt.pix.width;
    dev->height = v_fmt.fmt.pix.height;
    dev->num_planes = 1;

    dev->plane_fmt[0].bytesperline = v_fmt.fmt.pix.bytesperline;
    dev->plane_fmt[0].sizeimage =
        v_fmt.fmt.pix.bytesperline ? v_fmt.fmt.pix.sizeimage : 0;
  }

  return 0;
}

static int video_set_format(struct device *dev, unsigned int w, unsigned int h,
                            unsigned int format, unsigned int stride,
                            unsigned int buffer_size, enum v4l2_field field,
                            unsigned int flags) {
  struct v4l2_format fmt;
  unsigned int i;
  int ret;

  memset(&fmt, 0, sizeof fmt);
  fmt.type = dev->type;

  if (video_is_mplane(dev)) {
    const struct v4l2_format_info *info = v4l2_format_by_fourcc(format);

    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = format;
    fmt.fmt.pix_mp.field = field;
    fmt.fmt.pix_mp.num_planes = info->n_planes;
    fmt.fmt.pix_mp.flags = flags;

    for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
      fmt.fmt.pix_mp.plane_fmt[i].bytesperline = stride;
      fmt.fmt.pix_mp.plane_fmt[i].sizeimage = buffer_size;
    }
  } else {
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = format;
    fmt.fmt.pix.field = field;
    fmt.fmt.pix.bytesperline = stride;
    fmt.fmt.pix.sizeimage = buffer_size;
    fmt.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
    fmt.fmt.pix.flags = flags;
  }

  ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
  if (ret < 0) {
    printf("Failed to configure video format: %s (%d).\n", strerror(errno), errno);
    return ret;
  }

  if (video_is_mplane(dev)) {
    printf(
        "Video attributes, pixel format: %s (%08x), resolution: %ux%u field: %s, number "
        "of planes: %u\n",
        v4l2_format_name(fmt.fmt.pix_mp.pixelformat), fmt.fmt.pix_mp.pixelformat,
        fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
        v4l2_field_name(fmt.fmt.pix_mp.field), fmt.fmt.pix_mp.num_planes);

    for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
      printf("Plane %d attributes, stride: %u, buffer size: %u\n", i,
             fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
             fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
    }
  } else {
    printf(
        "Video attributes, pixel format: %s (%08x), resolution: %ux%u, stride: %u, "
        "field: %s buffer size %u\n",
        v4l2_format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.pixelformat,
        fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
        v4l2_field_name(fmt.fmt.pix.field), fmt.fmt.pix.sizeimage);
  }

  return 0;
}

static int video_buffer_mmap(struct device *dev, struct buffer *buffer,
                             struct v4l2_buffer *v4l2buf) {
  unsigned int length;
  unsigned int offset;
  unsigned int i;

  for (i = 0; i < dev->num_planes; i++) {
    if (video_is_mplane(dev)) {
      length = v4l2buf->m.planes[i].length;
      offset = v4l2buf->m.planes[i].m.mem_offset;
    } else {
      length = v4l2buf->length;
      offset = v4l2buf->m.offset;
    }

    buffer->mem[i] = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, offset);
    if (buffer->mem[i] == MAP_FAILED) {
      printf("Unable to map buffer %u/%u: %s (%d)\n", buffer->idx, i, strerror(errno),
             errno);
      return -1;
    }

    buffer->size[i] = length;
    buffer->padding[i] = 0;

    printf("Buffer %u/%u mapped at address %p.\n", buffer->idx, i, buffer->mem[i]);
  }

  return 0;
}

static int video_buffer_munmap(struct device *dev, struct buffer *buffer) {
  unsigned int i;
  int ret;

  for (i = 0; i < dev->num_planes; i++) {
    ret = munmap(buffer->mem[i], buffer->size[i]);
    if (ret < 0) {
      printf("Unable to unmap buffer %u/%u: %s (%d)\n", buffer->idx, i, strerror(errno),
             errno);
    }

    buffer->mem[i] = NULL;
  }

  return 0;
}

static int video_buffer_alloc_userptr(struct device *dev, struct buffer *buffer,
                                      struct v4l2_buffer *v4l2buf, unsigned int offset,
                                      unsigned int padding) {
  int page_size = getpagesize();
  unsigned int length;
  unsigned int i;
  int ret;

  for (i = 0; i < dev->num_planes; i++) {
    if (video_is_mplane(dev))
      length = v4l2buf->m.planes[i].length;
    else
      length = v4l2buf->length;

    ret =
        posix_memalign(&buffer->mem[i], page_size, length + offset + padding + page_size);
    if (ret < 0) {
      printf("Unable to allocate buffer %u/%u (%d)\n", buffer->idx, i, ret);
      return -ENOMEM;
    }

    buffer->mem[i] += offset;
    buffer->size[i] = length;
    buffer->padding[i] = padding;

    printf("Buffer %u/%u allocated at address %p length %u page %u.\n", buffer->idx, i,
           buffer->mem[i], length, page_size);
  }

  return 0;
}

static void video_buffer_free_userptr(struct device *dev, struct buffer *buffer) {
  unsigned int i;

  for (i = 0; i < dev->num_planes; i++) {
    free(buffer->mem[i]);
    buffer->mem[i] = NULL;
  }
}

static void get_ts_flags(uint32_t flags, const char **ts_type, const char **ts_source) {
  switch (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) {
    case V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN:
      *ts_type = "unk";
      break;
    case V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC:
      *ts_type = "mono";
      break;
    case V4L2_BUF_FLAG_TIMESTAMP_COPY:
      *ts_type = "copy";
      break;
    default:
      *ts_type = "inv";
  }
  switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
    case V4L2_BUF_FLAG_TSTAMP_SRC_EOF:
      *ts_source = "EoF";
      break;
    case V4L2_BUF_FLAG_TSTAMP_SRC_SOE:
      *ts_source = "SoE";
      break;
    default:
      *ts_source = "inv";
  }
}

static int video_alloc_buffers(struct device *dev, int nbufs, unsigned int offset,
                               unsigned int padding) {
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  struct v4l2_requestbuffers rb;
  struct v4l2_buffer buf;
  struct buffer *buffers;
  unsigned int i;
  int ret;

  memset(&rb, 0, sizeof rb);
  rb.count = nbufs;
  rb.type = dev->type;
  rb.memory = dev->memtype;

  ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
  if (ret < 0) {
    printf("Unable to request buffers: %s (%d).\n", strerror(errno), errno);
    return ret;
  }

  printf("%u buffers requested.\n", rb.count);

  buffers = malloc(rb.count * sizeof buffers[0]);
  if (buffers == NULL) return -ENOMEM;

  /* Map the buffers. */
  for (i = 0; i < rb.count; ++i) {
    const char *ts_type, *ts_source;

    memset(&buf, 0, sizeof buf);
    memset(planes, 0, sizeof planes);

    buf.index = i;
    buf.type = dev->type;
    buf.memory = dev->memtype;
    buf.length = VIDEO_MAX_PLANES;
    buf.m.planes = planes;

    ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &buf);
    if (ret < 0) {
      free(buffers);
      printf("Unable to query buffer %u: %s (%d).\n", i, strerror(errno), errno);
      return ret;
    }
    get_ts_flags(buf.flags, &ts_type, &ts_source);
    printf("length: %u offset: %u timestamp type/source: %s/%s\n", buf.length,
           buf.m.offset, ts_type, ts_source);

    buffers[i].idx = i;

    switch (dev->memtype) {
      case V4L2_MEMORY_MMAP:
        ret = video_buffer_mmap(dev, &buffers[i], &buf);
        break;

      case V4L2_MEMORY_USERPTR:
        ret = video_buffer_alloc_userptr(dev, &buffers[i], &buf, offset, padding);
        break;

      default:
        break;
    }

    if (ret < 0) {
      free(buffers);
      return ret;
    }
  }

  dev->timestamp_type = buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
  dev->buffers = buffers;
  dev->nbufs = rb.count;
  return 0;
}

static int video_free_buffers(struct device *dev) {
  struct v4l2_requestbuffers rb;
  unsigned int i;
  int ret;

  if (dev->nbufs == 0) return 0;

  for (i = 0; i < dev->nbufs; ++i) {
    switch (dev->memtype) {
      case V4L2_MEMORY_MMAP:
        ret = video_buffer_munmap(dev, &dev->buffers[i]);
        if (ret < 0) return ret;
        break;
      case V4L2_MEMORY_USERPTR:
        video_buffer_free_userptr(dev, &dev->buffers[i]);
        break;
      default:
        break;
    }
  }

  memset(&rb, 0, sizeof rb);
  rb.count = 0;
  rb.type = dev->type;
  rb.memory = dev->memtype;

  ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
  if (ret < 0) {
    printf("Unable to release buffers: %s (%d).\n", strerror(errno), errno);
    return ret;
  }

  printf("%u buffers released.\n", dev->nbufs);

  free(dev->buffers);
  dev->nbufs = 0;
  dev->buffers = NULL;

  return 0;
}

static int video_queue_buffer(struct device *dev, int index, enum buffer_fill_mode fill) {
  struct v4l2_buffer buf;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  int ret;
  unsigned int i;

  memset(&buf, 0, sizeof buf);
  memset(&planes, 0, sizeof planes);

  buf.index = index;
  buf.type = dev->type;
  buf.memory = dev->memtype;

  if (video_is_output(dev)) {
    buf.flags = dev->buffer_output_flags;
    if (dev->timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_COPY) {
      struct timespec ts;

      clock_gettime(CLOCK_MONOTONIC, &ts);
      buf.timestamp.tv_sec = ts.tv_sec;
      buf.timestamp.tv_usec = ts.tv_nsec / 1000;
    }
  }

  buf.flags |= dev->buffer_qbuf_flags;

  if (video_is_mplane(dev)) {
    buf.m.planes = planes;
    buf.length = dev->num_planes;
  }

  if (dev->memtype == V4L2_MEMORY_USERPTR) {
    if (video_is_mplane(dev)) {
      for (i = 0; i < dev->num_planes; i++) {
        buf.m.planes[i].m.userptr = (unsigned long)dev->buffers[index].mem[i];
        buf.m.planes[i].length = dev->buffers[index].size[i];
      }
    } else {
      buf.m.userptr = (unsigned long)dev->buffers[index].mem[0];
      buf.length = dev->buffers[index].size[0];
    }
  }

  for (i = 0; i < dev->num_planes; i++) {
    if (video_is_output(dev)) {
      if (video_is_mplane(dev))
        buf.m.planes[i].bytesused = dev->patternsize[i];
      else
        buf.bytesused = dev->patternsize[i];

      memcpy(dev->buffers[buf.index].mem[i], dev->pattern[i], dev->patternsize[i]);
    } else {
      if (fill & BUFFER_FILL_FRAME)
        memset(dev->buffers[buf.index].mem[i], 0x55, dev->buffers[index].size[i]);
      if (fill & BUFFER_FILL_PADDING)
        memset(dev->buffers[buf.index].mem[i] + dev->buffers[index].size[i], 0x55,
               dev->buffers[index].padding[i]);
    }
  }

  ret = ioctl(dev->fd, VIDIOC_QBUF, &buf);
  if (ret < 0) printf("Unable to queue buffer: %s (%d).\n", strerror(errno), errno);

  return ret;
}

static int video_enable(struct device *dev, int enable) {
  int type = dev->type;
  int ret;

  ret = ioctl(dev->fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
  if (ret < 0) {
    printf("Unable to %s streaming: %s (%d).\n", enable ? "start" : "stop",
           strerror(errno), errno);
    return ret;
  }

  return 0;
}

static int video_load_test_pattern(struct device *dev, const char *filename) {
  unsigned int plane;
  unsigned int size;
  int fd = -1;
  int ret;

  if (filename != NULL) {
    fd = open(filename, O_RDONLY);
    if (fd == -1) {
      printf("Unable to open test pattern file '%s': %s (%d).\n", filename,
             strerror(errno), errno);
      return -errno;
    }
  }

  /* Load or generate the test pattern */
  for (plane = 0; plane < dev->num_planes; plane++) {
    size = dev->buffers[0].size[plane];
    dev->pattern[plane] = malloc(size);
    if (dev->pattern[plane] == NULL) {
      ret = -ENOMEM;
      goto done;
    }

    if (filename != NULL) {
      ret = read(fd, dev->pattern[plane], size);
      if (ret != (int)size && dev->plane_fmt[plane].bytesperline != 0) {
        printf("Test pattern file size %u doesn't match image size %u\n", ret, size);
        ret = -EINVAL;
        goto done;
      }
    } else {
      uint8_t *data = dev->pattern[plane];
      unsigned int i;

      if (dev->plane_fmt[plane].bytesperline == 0) {
        printf(
            "Compressed format detected for plane %u and no test pattern filename "
            "given.\n"
            "The test pattern can't be generated automatically.\n",
            plane);
        ret = -EINVAL;
        goto done;
      }

      for (i = 0; i < dev->plane_fmt[plane].sizeimage; ++i) *data++ = i;
    }

    dev->patternsize[plane] = size;
  }

  ret = 0;

done:
  if (fd != -1) close(fd);

  return ret;
}

static int video_prepare_capture(struct device *dev, int nbufs, unsigned int offset,
                                 const char *filename, enum buffer_fill_mode fill) {
  unsigned int padding;
  int ret;

  /* Allocate and map buffers. */
  padding = (fill & BUFFER_FILL_PADDING) ? 4096 : 0;
  if ((ret = video_alloc_buffers(dev, nbufs, offset, padding)) < 0) return ret;

  if (video_is_output(dev)) {
    ret = video_load_test_pattern(dev, filename);
    if (ret < 0) return ret;
  }

  return 0;
}

static int video_queue_all_buffers(struct device *dev, enum buffer_fill_mode fill) {
  unsigned int i;
  int ret;

  /* Queue the buffers. */
  for (i = 0; i < dev->nbufs; ++i) {
    ret = video_queue_buffer(dev, i, fill);
    if (ret < 0) return ret;
  }

  return 0;
}

/*st20 s*/

static int tx_video_next_frame(void *priv, uint16_t *next_frame_idx,
                               struct st20_tx_frame_meta *meta) {
  struct st_v4l2_tx_video_session *tx_video_session = priv;
  struct tx_frame_buff_ct *framebuff_ctl = &(tx_video_session->framebuff_ctl);
  int ret;
  MTL_MAY_UNUSED(meta);

  pthread_mutex_lock(&(framebuff_ctl->wake_mutex));

  if (TX_FRAME_READY == framebuff_ctl->buffs[framebuff_ctl->transmit_idx].status) {
    // printf("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff_ctl->buffs[framebuff_ctl->transmit_idx].status = TX_FRAME_TRANSMITTING;
    *next_frame_idx = framebuff_ctl->transmit_idx;

    clock_gettime(CLOCK_MONOTONIC,
                  &(framebuff_ctl->buffs[framebuff_ctl->transmit_idx].st20_ts));

    /* point to next */
    framebuff_ctl->transmit_idx++;
    if (framebuff_ctl->transmit_idx >= framebuff_ctl->cnt) {
      framebuff_ctl->transmit_idx = 0;
    }
  } else {
    /* not ready */
    ret = -EIO;
  }

  pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

  return ret;
}

static int tx_video_frame_done(void *priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta *meta) {
  struct st_v4l2_tx_video_session *tx_video_session = priv;
  struct st_v4l2_tx_context *st_v4l2_tx = tx_video_session->ctx;
  struct tx_frame_buff_ct *framebuff_ctl = &(tx_video_session->framebuff_ctl);
  int ret;
  MTL_MAY_UNUSED(meta);

  pthread_mutex_lock(&(framebuff_ctl->wake_mutex));

  if (frame_idx != framebuff_ctl->receive_idx) {
    ret = -EIO;
    printf("%s, receive_idx %d != frame_done %d\n", __func__, framebuff_ctl->receive_idx,
           frame_idx);
    pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));
    return ret;
  }

  if (TX_FRAME_TRANSMITTING != framebuff_ctl->buffs[framebuff_ctl->receive_idx].status) {
    ret = -EIO;
    printf("%s, receive status %d != TRASNSMIT\n", __func__,
           framebuff_ctl->buffs[framebuff_ctl->receive_idx].status);
    pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

    return ret;
  }

  framebuff_ctl->buffs[framebuff_ctl->receive_idx].status = TX_FRAME_RECEIVING;

  /* point to next */
  framebuff_ctl->receive_idx++;
  if (framebuff_ctl->receive_idx >= framebuff_ctl->cnt) {
    framebuff_ctl->receive_idx = 0;
  }

  pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

  tx_video_session->st20_frame_done_cnt++;

  ret = video_queue_buffer(&(st_v4l2_tx->dev), frame_idx, st_v4l2_tx->fill_mode);
  if (ret < 0) {
    printf("%s Unable to requeue buffer: %d\n", __func__, ret);
  }

  return ret;
}

static void tx_video_debug_output(void) {
  struct st_v4l2_tx_video_session *tx_video_session = g_st_v4l2_tx->tx_video_sessions;

  for (int i = 0; i < tx_video_session->framebuff_ctl.cnt; i++) {
    printf("time %ld.%06ld %ld.%06ld %ld.%06ld\n",
           tx_video_session->framebuff_ctl.buffs[i].v4l2_ts.tv_sec,
           tx_video_session->framebuff_ctl.buffs[i].v4l2_ts.tv_nsec / 1000,
           tx_video_session->framebuff_ctl.buffs[i].app_ts.tv_sec,
           tx_video_session->framebuff_ctl.buffs[i].app_ts.tv_nsec / 1000,
           tx_video_session->framebuff_ctl.buffs[i].st20_ts.tv_sec,
           tx_video_session->framebuff_ctl.buffs[i].st20_ts.tv_nsec / 1000);
  }

  printf("index %d %d %d\n", tx_video_session->framebuff_ctl.receive_idx,
         tx_video_session->framebuff_ctl.ready_idx,
         tx_video_session->framebuff_ctl.transmit_idx);

  printf("capture/transmit %d/%d frames\n", g_st_v4l2_tx->dqbuf_cnt,
         tx_video_session->st20_frame_done_cnt);
}

static void tx_video_sig_handler(int signo) {
  printf("%s, signal %d\n", __func__, signo);

  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      if (g_st_v4l2_tx->st) {
        mtl_abort(g_st_v4l2_tx->st);
      }
      g_st_v4l2_tx->stop = true;

      tx_video_debug_output();

      break;
  }
}

static int tx_video_verify_buffer(struct st_v4l2_tx_video_session *tx_video_session,
                                  struct v4l2_buffer *buf) {
  struct st_v4l2_tx_context *st_v4l2_tx = tx_video_session->ctx;
  struct device *dev = &(st_v4l2_tx->dev);
  unsigned int length;

  if (0 != buf->m.planes[0].data_offset) {
    printf("%s data_offset %d != 0\n", __func__, buf->m.planes[0].data_offset);
    return -1;
  }

  length = buf->m.planes[0].bytesused;

  if (length != tx_video_session->framebuff_size) {
    printf("%s bytesused %d != framebuff_size %d\n", __func__, length,
           tx_video_session->framebuff_size);
    return -1;
  }

  if (dev->plane_fmt[0].sizeimage != (length + dev->plane_fmt[0].bytesperline)) {
    printf("%s bytes used %u != image size %u\n", __func__, length,
           dev->plane_fmt[0].sizeimage);
    return -1;
  }

  st_v4l2_tx->dqbuf_cnt++;

  return 0;
}

static int tx_video_copy_frame(struct st_v4l2_tx_video_session *tx_video_session,
                               struct v4l2_buffer *buf) {
  struct st_v4l2_tx_context *st_v4l2_tx = tx_video_session->ctx;
  struct device *dev = &(st_v4l2_tx->dev);
  struct tx_frame_buff_ct *framebuff_ctl = &(tx_video_session->framebuff_ctl);
  void *frame_addr;
  unsigned int i;
  void *data = NULL;
  unsigned int length = 0;

  pthread_mutex_lock(&(framebuff_ctl->wake_mutex));
  if (buf->index != framebuff_ctl->ready_idx) {
    /* out of order*/
    printf("%s(%d), ready idx out of order\n", __func__, framebuff_ctl->ready_idx);
    pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));
    return -1;
  }
  if (TX_FRAME_RECEIVING != framebuff_ctl->buffs[framebuff_ctl->ready_idx].status) {
    /* buff full */
    printf("%s(%d), buff full\n", __func__, framebuff_ctl->ready_idx);
    pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));
    return -1;
  }
  pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

  if (tx_video_session->ops_tx.flags & ST20_TX_FLAG_EXT_FRAME) {
    st20_tx_set_ext_frame(tx_video_session->handle, framebuff_ctl->ready_idx,
                          &tx_video_session->ext_frames[framebuff_ctl->ready_idx]);

    display_consume_frame(
        tx_video_session,
        tx_video_session->ext_frames[framebuff_ctl->ready_idx].buf_addr);
  } else {
    frame_addr =
        st20_tx_get_framebuffer(tx_video_session->handle, framebuff_ctl->ready_idx);

    for (i = 0; i < dev->num_planes; i++) {
      data = dev->buffers[buf->index].mem[i];
      if (video_is_mplane(dev)) {
        length = buf->m.planes[i].bytesused;

        if (!dev->write_data_prefix) {
          data += buf->m.planes[i].data_offset;
          length -= buf->m.planes[i].data_offset;
        }
      } else {
        length = buf->bytesused;
      }
    }

    if (data) memcpy(frame_addr, data, length);
  }

  pthread_mutex_lock(&(framebuff_ctl->wake_mutex));
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].status = TX_FRAME_READY;
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].size = tx_video_session->framebuff_size;

  clock_gettime(CLOCK_MONOTONIC,
                &(framebuff_ctl->buffs[framebuff_ctl->ready_idx].app_ts));
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].v4l2_ts.tv_sec = buf->timestamp.tv_sec;
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].v4l2_ts.tv_nsec =
      buf->timestamp.tv_usec * 1000;
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].st20_ts.tv_sec = 0;
  framebuff_ctl->buffs[framebuff_ctl->ready_idx].st20_ts.tv_nsec = 0;

  /* point to next */
  framebuff_ctl->ready_idx++;
  if (framebuff_ctl->ready_idx >= framebuff_ctl->cnt) {
    framebuff_ctl->ready_idx = 0;
  }
  pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

  return 0;
}

/*st20 e*/

static void *tx_video_thread_capture(void *arg) {
  int ret = 0;
  struct st_v4l2_tx_video_session *tx_video_session =
      (struct st_v4l2_tx_video_session *)arg;
  struct st_v4l2_tx_context *st_v4l2_tx = tx_video_session->ctx;
  struct tx_frame_buff_ct *framebuff_ctl = &(tx_video_session->framebuff_ctl);

  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  struct v4l2_buffer buf;
  struct timespec start;
  struct timeval last;

  unsigned int i;
  double fps;

  clock_gettime(CLOCK_MONOTONIC, &start);
  last.tv_sec = start.tv_sec;
  last.tv_usec = start.tv_nsec / 1000;

  for (i = 0; i < st_v4l2_tx->nframes; ++i) {
    /* Dequeue a buffer. */
    memset(&buf, 0, sizeof buf);
    memset(planes, 0, sizeof planes);

    buf.type = st_v4l2_tx->dev.type;
    buf.memory = st_v4l2_tx->dev.memtype;
    buf.length = VIDEO_MAX_PLANES;
    buf.m.planes = planes;
    buf.flags = st_v4l2_tx->dev.buffer_dqbuf_flags;

    ret = ioctl(st_v4l2_tx->dev.fd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
      printf("%s Unable to dequeue buffer: %s (%d).\n", __func__, strerror(errno), errno);
      break;
    }

    ret = tx_video_verify_buffer(tx_video_session, &buf);
    if (ret < 0) {
      printf("%s tx_video_verify_buffer failed %d.\n", __func__, ret);
      break;
    }

    fps = (buf.timestamp.tv_sec - last.tv_sec) * 1000000 + buf.timestamp.tv_usec -
          last.tv_usec;
    fps = fps ? 1000000.0 / fps : 0.0;
    /*
            printf("%u (%u) [%c] %s %u %u %ld.%06ld %.3f fps \n", i, buf.index,
                (buf.flags & V4L2_BUF_FLAG_ERROR) ? 'E' : '-', v4l2_field_name(buf.field),
       buf.sequence, buf.m.planes[0].bytesused, buf.timestamp.tv_sec,
       buf.timestamp.tv_usec, fps);
    */
    last = buf.timestamp;

    /* Save the image. */
    if (!st_v4l2_tx->skip) {
      ret = tx_video_copy_frame(tx_video_session, &buf);
      if (ret < 0) {
        printf("%s tx_video_copy_frame failed %d.\n", __func__, ret);
        break;
      }
    } else {
      pthread_mutex_lock(&(framebuff_ctl->wake_mutex));
      if (buf.index != framebuff_ctl->ready_idx) {
        /* out of order*/
        printf("%s(%d), ready idx out of order\n", __func__, framebuff_ctl->ready_idx);
        pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));
        ret = -1;
        break;
      }

      framebuff_ctl->buffs[framebuff_ctl->ready_idx].status = TX_FRAME_RECEIVING;
      framebuff_ctl->ready_idx++;
      if (framebuff_ctl->ready_idx >= framebuff_ctl->cnt) {
        framebuff_ctl->ready_idx = 0;
      }
      framebuff_ctl->receive_idx = framebuff_ctl->ready_idx;
      framebuff_ctl->transmit_idx = framebuff_ctl->ready_idx;
      pthread_mutex_unlock(&(framebuff_ctl->wake_mutex));

      ret = video_queue_buffer(&(st_v4l2_tx->dev), buf.index, st_v4l2_tx->fill_mode);
      if (ret < 0) {
        printf("%s Unable to requeue buffer: %d\n", __func__, ret);
        break;
      }
    }

    if (st_v4l2_tx->stop) {
      break;
    }
  }

  st_v4l2_tx->stop = true;

  printf("%s capture_stop.\n", __func__);

  // pthread_exit(&ret);
  return NULL;
}

static int tx_video_thread_create(struct st_v4l2_tx_video_session *tx_video_session,
                                  unsigned int priority, unsigned int cpu) {
  int ret = 0;

  if (pthread_create(&(tx_video_session->st20_app_thread), NULL, tx_video_thread_capture,
                     tx_video_session)) {
    printf("%s pthread_create Failed: %s (%d)\n", __func__, strerror(errno), errno);
    ret = -EIO;
    return ret;
  }

  if (video_set_realtime(tx_video_session->st20_app_thread, priority, cpu) < 0) {
    printf("%s video_set_realtime Failed\n", __func__);
    ret = -EIO;
    return ret;
  }

  return ret;
}

static void usage(const char *argv0) {
  printf("Usage: %s [options] device\n", argv0);
  printf("Supported options:\n");
  printf("-h, --help    Show this help screen\n");
  printf("-c, --capture    Set capture frames\n");
  printf("-n, --nbufs    Set the number of video buffers\n");
  printf("-p, --port    Set port BDF\n");
  printf("-m, --mac    Set dst mac address\n");
  printf("-s, --show    Display capture video\n");
  printf("-e, --ptp    Enable ptp\n");
  printf("-t, --tsn    Enable TSN based packet pacing\n");
  printf("    --log-status    Log device status\n");
}

#define OPT_LOG_STATUS 256

static struct option opts[] = {{"capture", 1, 0, 'c'},
                               {"help", 0, 0, 'h'},
                               {"nbufs", 1, 0, 'n'},
                               {"port", 1, 0, 'p'},
                               {"mac", 1, 0, 'm'},
                               {"show", 0, 0, 's'},
                               {"ptp", 0, 0, 'e'},
                               {"tsn", 0, 0, 't'},
                               {"log-status", 0, 0, OPT_LOG_STATUS},
                               {0, 0, 0, 0}};

int main(int argc, char *argv[]) {
  struct st_v4l2_tx_context *st_v4l2_tx;
  struct st_v4l2_tx_video_session *tx_video_session;

  int ret;

  /* Use video capture by default if query isn't done. */
  unsigned int capabilities = V4L2_CAP_VIDEO_CAPTURE_MPLANE;

  int do_log_status = 0;
  int c;
  int memory_type = 1;

  bool show = false;
  bool ptp = false;
  bool tsn = false;

  /* Video buffers */
  unsigned int nbufs = V4L_BUFFERS_DEFAULT;
  unsigned int userptr_offset = 0;

  unsigned int width = V4L2_FMT_WIDTH;
  unsigned int height = V4L2_FMT_HEIGHT;

  /* Capture loop */
  enum buffer_fill_mode fill_mode = BUFFER_FILL_NONE;
  unsigned int nframes = (unsigned int)-1;
  const char *filename = NULL;

  unsigned int v4l2_thread_priority = 90;
  unsigned int v4l2_thread_cpu = V4L2_TX_THREAD_CORE;

  unsigned int display_thread_priority = 80;
  unsigned int display_thread_cpu = DISPLAY_THREAD_CORE;

  /*st20 s*/
  unsigned int session_num = 1;
  char port[] = TX_VIDEO_PORT_BDF;
  char *tx_lcore = TX_VIDEO_LCORE;
  enum st_fps tx_fps = ST_FPS_P50;
  char dst_mac[] = TX_VIDEO_DST_MAC_ADDR;

  size_t pg_size;
  size_t map_size;

  opterr = 0;
  while ((c = getopt_long(argc, argv, "c::hn::p:m:se", opts, NULL)) != -1) {
    switch (c) {
      case 'c':
        nframes = atoi(optarg);
        break;

      case 'h':
        usage(argv[0]);
        return 0;

      case 'n':
        nbufs = atoi(optarg);
        if (nbufs > V4L_BUFFERS_MAX) nbufs = V4L_BUFFERS_MAX;
        break;

      case 'p':
        strcpy(port, optarg);
        break;

      case 'm':
        strcpy(dst_mac, optarg);
        break;

      case 's':
        show = true;
        break;

      case 'e':
        ptp = true;
        break;

      case 't':
        tsn = true;
        break;

      case OPT_LOG_STATUS:
        do_log_status = 1;
        break;

      default:
        printf("%s Invalid option -%c\n", __func__, c);
        printf("%s Run %s -h for help.\n", __func__, argv[0]);
        return 1;
    }
  }

  st_v4l2_tx = (struct st_v4l2_tx_context *)malloc(sizeof(struct st_v4l2_tx_context));
  if (!st_v4l2_tx) {
    printf("%s struct application malloc fail\n", __func__);
    return -EIO;
  }
  memset(st_v4l2_tx, 0, sizeof(struct st_v4l2_tx_context));

  st_v4l2_tx->nframes = nframes;
  st_v4l2_tx->skip = true;
  st_v4l2_tx->fill_mode = fill_mode;

  // init v4l2
  video_init(&(st_v4l2_tx->dev));

  if (1 == memory_type) {
    st_v4l2_tx->dev.memtype = V4L2_MEMORY_USERPTR;
  }

  if (optind >= argc) {
    usage(argv[0]);
    free(st_v4l2_tx);
    return -EIO;
  }

  ret = video_open(&(st_v4l2_tx->dev), argv[optind]);
  if (ret < 0) {
    free(st_v4l2_tx);
    return -EIO;
  }

  do_print_ipu_version(&(st_v4l2_tx->dev));

  ret = video_querycap(&(st_v4l2_tx->dev), &capabilities);
  if (ret < 0) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  ret = cap_get_buf_type(capabilities);
  if (ret < 0) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  video_set_buf_type(&(st_v4l2_tx->dev), ret);

  if (!video_is_capture(&(st_v4l2_tx->dev))) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  if (do_log_status) {
    video_log_status(&(st_v4l2_tx->dev));
  }

  if (video_set_format(&(st_v4l2_tx->dev), width, height, V4L2_PIX_FMT_UYVY, 0, 0,
                       V4L2_FIELD_ANY, 0) < 0) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  /* Get the video format. */
  if (!video_get_format(&(st_v4l2_tx->dev))) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  if (!video_is_mplane(&(st_v4l2_tx->dev))) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  if (1 != st_v4l2_tx->dev.num_planes) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  if (video_prepare_capture(&(st_v4l2_tx->dev), nbufs, userptr_offset, filename,
                            fill_mode) < 0) {
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  if (video_queue_all_buffers(&(st_v4l2_tx->dev), fill_mode) < 0) {
    video_free_buffers(&(st_v4l2_tx->dev));
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  // init st20
  st_v4l2_tx->param.num_ports = 1;
  st_v4l2_tx->param.pmd[MTL_PORT_P] = TX_VIDEO_PMD;
  strncpy(st_v4l2_tx->param.port[MTL_PORT_P], port, MTL_PORT_MAX_LEN);
  memcpy(st_v4l2_tx->param.sip_addr[MTL_PORT_P], g_tx_video_local_ip, MTL_IP_ADDR_LEN);
  st_v4l2_tx->param.flags =
      MTL_FLAG_BIND_NUMA | MTL_FLAG_TX_VIDEO_MIGRATE;  // default bind to numa
  if (ptp == true) {
    st_v4l2_tx->param.flags |= MTL_FLAG_PTP_ENABLE;
  }
  if (tsn == true) {
    st_v4l2_tx->param.flags |= MTL_FLAG_PTP_ENABLE;
    st_v4l2_tx->param.flags |= MTL_FLAG_PHC2SYS_ENABLE;
    st_v4l2_tx->param.pacing = ST21_TX_PACING_WAY_TSN;
  } else {
    st_v4l2_tx->param.pacing = ST21_TX_PACING_WAY_AUTO;
  }
  st_v4l2_tx->param.log_level = MTL_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  st_v4l2_tx->param.priv = NULL;                     // usr ctx pointer
  // if not registed, the internal ptp source will be used
  st_v4l2_tx->param.ptp_get_time_fn = NULL;
  st_v4l2_tx->param.tx_queues_cnt[0] = session_num;
  st_v4l2_tx->param.rx_queues_cnt[0] = 0;
  // let lib decide to core or user could define it.
  st_v4l2_tx->param.lcores = tx_lcore;

  // create device
  st_v4l2_tx->st = mtl_init(&(st_v4l2_tx->param));
  if (!st_v4l2_tx->st) {
    printf("%s st_init fail\n", __func__);
    video_free_buffers(&(st_v4l2_tx->dev));
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }

  tx_video_session = (struct st_v4l2_tx_video_session *)malloc(
      sizeof(struct st_v4l2_tx_video_session) * session_num);
  if (!tx_video_session) {
    printf("%s struct st_v4l2_tx_video_session is not correctly malloc", __func__);
    mtl_uninit(st_v4l2_tx->st);
    video_free_buffers(&(st_v4l2_tx->dev));
    video_close(&(st_v4l2_tx->dev));
    free(st_v4l2_tx);
    return -EIO;
  }
  memset(tx_video_session, 0, sizeof(struct st_v4l2_tx_video_session) * session_num);

  if (show) {
    ret = app_player_init();
    if (ret < 0) {
      st_v4l2_tx->has_sdl = false;
    } else {
      st_v4l2_tx->has_sdl = true;
    }
  }

  st_v4l2_tx->tx_video_sessions = tx_video_session;
  st_v4l2_tx->tx_video_session_cnt = session_num;

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    tx_video_session = &(st_v4l2_tx->tx_video_sessions[i]);
    tx_video_session->ctx = st_v4l2_tx;
    tx_video_session->idx = i;

    pthread_mutex_init(&(tx_video_session->framebuff_ctl.wake_mutex), NULL);
    pthread_cond_init(&(tx_video_session->framebuff_ctl.wake_cond), NULL);

    tx_video_session->framebuff_ctl.cnt = nbufs;
    tx_video_session->framebuff_ctl.ready_idx = 0;
    tx_video_session->framebuff_ctl.receive_idx = 0;
    tx_video_session->framebuff_ctl.transmit_idx = 0;
    tx_video_session->framebuff_ctl.buffs =
        (struct tx_frame_buff *)malloc(sizeof(struct tx_frame_buff) * nbufs);
    if (!tx_video_session->framebuff_ctl.buffs) {
      printf("%s[%d], tx_frame_buffs malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(tx_video_session->framebuff_ctl.buffs, 0,
           sizeof(struct tx_frame_buff) * nbufs);

    for (int j = 0; j < nbufs; j++) {
      tx_video_session->framebuff_ctl.buffs[j].status = TX_FRAME_FREE;
      tx_video_session->framebuff_ctl.buffs[j].size = 0;
    }

    // init ops
    tx_video_session->ops_tx.name = "v4l2_st20_tx";
    tx_video_session->ops_tx.priv = tx_video_session;  // app handle register to lib
    tx_video_session->ops_tx.num_port = 1;

    // tx src ip like 239.0.0.1
    memcpy(tx_video_session->ops_tx.dip_addr[MTL_PORT_P], g_tx_video_dst_ip,
           MTL_IP_ADDR_LEN);
    // send port interface like 0000:af:00.0
    strncpy(tx_video_session->ops_tx.port[MTL_PORT_P], port, MTL_PORT_MAX_LEN);

    if (1 == memory_type) {
      tx_video_session->ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    }

    tx_video_session->ops_tx.udp_port[MTL_PORT_P] = TX_VIDEO_UDP_PORT + i;  // udp port
    tx_video_session->ops_tx.pacing = ST21_PACING_NARROW;
    tx_video_session->ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    tx_video_session->ops_tx.width = st_v4l2_tx->dev.width;
    tx_video_session->ops_tx.height = st_v4l2_tx->dev.height;
    tx_video_session->ops_tx.fps = tx_fps;
    tx_video_session->ops_tx.fmt = ST20_FMT_YUV_422_8BIT;
    tx_video_session->ops_tx.payload_type = TX_VIDEO_PAYLOAD_TYPE;
    tx_video_session->ops_tx.framebuff_cnt = nbufs;

    // app regist non-block func, app could get a frame to send to lib
    tx_video_session->ops_tx.get_next_frame = tx_video_next_frame;
    // app regist non-block func, app could get the frame tx done
    tx_video_session->ops_tx.notify_frame_done = tx_video_frame_done;

    tx_video_session->handle =
        st20_tx_create(st_v4l2_tx->st, &(tx_video_session->ops_tx));
    if (!tx_video_session->handle) {
      printf("%s[%d] tx_session is not correctly created\n", __func__, i);
      ret = -EIO;
      goto error;
    }

    tx_video_session->framebuff_size =
        st20_tx_get_framebuffer_size(tx_video_session->handle);

    if (tx_video_session->ops_tx.flags & ST20_TX_FLAG_EXT_FRAME) {
      if (st_v4l2_tx->dev.buffers->size[0] < tx_video_session->framebuff_size) {
        printf("%s[%d] buffers->size %d < framebuff_size %d\n", __func__, i,
               st_v4l2_tx->dev.buffers->size[0], tx_video_session->framebuff_size);
        ret = -EIO;
        goto error;
      }

      if (getpagesize() < mtl_page_size(st_v4l2_tx->st)) {
        printf("%s[%d] pagesize %d < pg_sz %ld\n", __func__, i, getpagesize(),
               mtl_page_size(st_v4l2_tx->st));
        ret = -EIO;
        goto error;
      }

      tx_video_session->ext_frames =
          (struct st20_ext_frame *)malloc(sizeof(struct st20_ext_frame) * nbufs);
      if (!tx_video_session->ext_frames) {
        printf("%s[%d], ext_frames malloc fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      memset(tx_video_session->ext_frames, 0, sizeof(struct st20_ext_frame) * nbufs);

      for (int j = 0; j < nbufs; ++j) {
        tx_video_session->ext_frames[j].buf_addr = st_v4l2_tx->dev.buffers[j].mem[0];
        pg_size = mtl_page_size(st_v4l2_tx->st);
        map_size = tx_video_session->framebuff_size;
        map_size += (pg_size - tx_video_session->framebuff_size % pg_size);
        tx_video_session->ext_frames[j].buf_iova = mtl_dma_map(
            st_v4l2_tx->st, tx_video_session->ext_frames[j].buf_addr, map_size);
        if (tx_video_session->ext_frames[j].buf_iova == MTL_BAD_IOVA) {
          printf("%s(%d), %d ext fb mmap fail\n", __func__, i, j);
          ret = -EIO;
          goto error;
        }
        tx_video_session->ext_frames[j].buf_len = map_size;
      }
    }

    if (st_v4l2_tx->has_sdl) {
      ret = app_init_display(&(tx_video_session->display), i, st_v4l2_tx->dev.width,
                             st_v4l2_tx->dev.height, st_v4l2_tx->ttf_file);
      if (ret < 0) {
        printf("%s(%d), app_init_display fail %d\n", __func__, i, ret);
        goto error;
      }
    }
  }

  g_st_v4l2_tx = st_v4l2_tx;

  signal(SIGINT, tx_video_sig_handler);

  printf("start capture...\n");

  // start tx
  ret = mtl_start(st_v4l2_tx->st);
  if (0 != ret) {
    printf("%s st_start fail\n", __func__);
    ret = -EIO;
    goto error;
  }

  /* Start streaming. */
  ret = video_enable(&(st_v4l2_tx->dev), 1);
  if (ret < 0) {
    printf("%s video_enable 1 fail %d.\n", __func__, ret);
    goto error;
  }

  /*task create*/
  for (int i = 0; i < session_num; i++) {
    tx_video_session = &(st_v4l2_tx->tx_video_sessions[i]);
    ret = tx_video_thread_create(tx_video_session, v4l2_thread_priority, v4l2_thread_cpu);
    if (ret < 0) {
      printf("%s video thread create fail %d.\n", __func__, ret);
      goto error;
    }

    if (true == st_v4l2_tx->has_sdl) {
      ret = display_thread_create(tx_video_session, display_thread_priority,
                                  display_thread_cpu);
      if (ret < 0) {
        printf("%s video thread create fail %d.\n", __func__, ret);
        goto error;
      }
    }
  }

  sleep(4);
  st_v4l2_tx->skip = false;

  // waiting
  while (true != st_v4l2_tx->stop) {
    sleep(1);
  }

error:

  sleep(1);

  // printf("capture/transmit %d/%d frames\n", st_v4l2_tx->dqbuf_cnt,
  // tx_video_session->st20_frame_done_cnt);
  for (int i = 0; i < session_num; i++) {
    tx_video_session = &(st_v4l2_tx->tx_video_sessions[i]);

    if (tx_video_session->st20_app_thread) {
      if (pthread_join(tx_video_session->st20_app_thread, NULL)) {
        printf("pthread_join Failed: %s (%d)\n", strerror(errno), errno);
      }
    }
  }
  printf("%s thread joined\n", __func__);

  ret = mtl_stop(st_v4l2_tx->st);
  if (0 != ret) {
    printf("%s st_stop fail\n", __func__);
  }
  printf("%s st_stop.\n", __func__);

  for (int i = 0; i < session_num; i++) {
    tx_video_session = &(st_v4l2_tx->tx_video_sessions[i]);

    if (tx_video_session->ext_frames) {
      for (int j = 0; j < nbufs; ++j) {
        if ((tx_video_session->ext_frames[j].buf_iova != MTL_BAD_IOVA) &&
            (tx_video_session->ext_frames[j].buf_iova != 0)) {
          ret = mtl_dma_unmap(st_v4l2_tx->st, tx_video_session->ext_frames[j].buf_addr,
                              tx_video_session->ext_frames[j].buf_iova,
                              tx_video_session->ext_frames[j].buf_len);
          if (0 != ret) {
            printf("%s st_dma_unmap fail\n", __func__);
          }
        }
      }
      free(tx_video_session->ext_frames);
    }

    if (tx_video_session->handle) {
      ret = st20_tx_free(tx_video_session->handle);
      if (0 != ret) {
        printf("%s st20_tx_free fail\n", __func__);
      }
    }

    if (tx_video_session->framebuff_ctl.buffs) {
      free(tx_video_session->framebuff_ctl.buffs);
    }

    pthread_cond_destroy(&(tx_video_session->framebuff_ctl.wake_cond));
    pthread_mutex_destroy(&(tx_video_session->framebuff_ctl.wake_mutex));

    if (st_v4l2_tx->has_sdl) {
      ret = app_uinit_display(&(tx_video_session->display));
      if (ret < 0) {
        printf("%s(%d), app_uinit_display fail %d\n", __func__, i, ret);
      }
    }
  }

  free(st_v4l2_tx->tx_video_sessions);
  printf("%s free tx_video session.\n", __func__);

  if (true == st_v4l2_tx->has_sdl) {
    app_player_uinit();
  }

  ret = mtl_uninit(st_v4l2_tx->st);
  if (0 != ret) {
    printf("%s st_uninit fail\n", __func__);
  }
  printf("%s st_uninit.\n", __func__);

  /* Stop streaming. */
  ret = video_enable(&(st_v4l2_tx->dev), 0);
  if (ret < 0) {
    printf("%s video_enable 0 fail %d.\n", __func__, ret);
  }
  printf("%s video_disable.\n", __func__);

  video_free_buffers(&(st_v4l2_tx->dev));
  video_close(&(st_v4l2_tx->dev));
  printf("%s video_close.\n", __func__);

  free(st_v4l2_tx);

  return 0;
}
