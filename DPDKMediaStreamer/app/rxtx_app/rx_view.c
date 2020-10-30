/*
* Copyright 2020 Intel Corporation.
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

/*
 *
 *    Transmitting and receiving example using Media streamer based on DPDK
 *
 */

#include "rx_view.h"

#include <rte_atomic.h>
#include <rte_cycles.h>

#include <byteswap.h>

struct view_info
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	uint32_t format;
	uint32_t bufFormat;
	int cnt;
	long int allFrames;
	uint8_t *frame;
	int width;
	int height;
};

static pthread_t EventLoopThreadId;
static void *EventLoopThread(void *arg);
static rte_atomic32_t howView;
static rte_atomic32_t isStop;

static int default_width = 320;
static int default_height = 240;

st_status_t
InitSDL(void)
{
	int res;
	res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
	if (res != 0)
	{
		printf("SLD_Init error: %s\n", SDL_GetError());
		return res;
	}
	res = pthread_create(&EventLoopThreadId, NULL, EventLoopThread, (void *)NULL);
	if (res != 0)
	{
		printf("SDL threat create error\n");
		SDL_Quit();
	}
	rte_atomic32_init(&howView);
	rte_atomic32_set(&howView, 0);
	rte_atomic32_init(&isStop);
	rte_atomic32_set(&isStop, 0);
	return res;
}

st_status_t
CreateView(view_info_t **view, const char *label, st21_buf_fmt_t bufFormat, int width, int height)
{
	view_info_t *locView;
	uint8_t *frame = NULL;

	if ((width != 1920 || height != 1080) && (width != 1280 || height != 720))
		return ST_NOT_SUPPORTED;
	switch (bufFormat)
	{
	case ST21_BUF_FMT_RGBA_8BIT:
		break;
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		frame = malloc(width * height * 4);
		if (frame == NULL)
			return ST_NO_MEMORY;
		memset(frame, 0xff, width * height * 4);
		break;
	default:
		return ST_NOT_SUPPORTED;
		break;
	}
	locView = malloc(sizeof(view_info_t));
	if (locView == NULL)
	{
		if (frame != NULL)
			free(frame);
		return ST_NO_MEMORY;
	}
	memset(locView, 0, sizeof(view_info_t));

	locView->frame = frame;
	locView->width = width;
	locView->height = height;

	locView->window
		= SDL_CreateWindow(label, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
						   default_height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	locView->renderer = SDL_CreateRenderer(locView->window, -1, 0);
	locView->format = SDL_PIXELFORMAT_ARGB8888;
	locView->bufFormat = bufFormat;
	locView->texture = SDL_CreateTexture(locView->renderer, locView->format,
										 SDL_TEXTUREACCESS_STREAMING, width, height);
	SDL_SetTextureBlendMode(locView->texture, SDL_BLENDMODE_NONE);
	*view = locView;
	rte_atomic32_add(&howView, 1);

	return ST_OK;
}

typedef struct viewRGB
{
	uint8_t b;
	uint8_t g;
	uint8_t r;
	uint8_t n;
} viewRGB;

#define ITU_KONF (601)
#define USE_JPEG (1)

#if ITU_KONF == 601

#define Kr (0.299)
#define Kg (0.587)
#define Kb (0.114)

#elif ITU_KONF == 709

#define Kr (0.0722)
#define Kg (0.2126)
#define Kb (0.7152)

#elif ITU_KONF == 2020

#define Kr (0.0722)
#define Kg (0.2126)
#define Kb (0.7152)

#endif

#if USE_JPEG == 0

// ITU
#define Ry (1.0)
#define Rb (0.0)
#define Rr (2.0 - 2.0 * Kr)

#define Gy (1.0)
#define Gb ((-Kb / Kg) * (2.0 - 2.0 * Kb))
#define Gr ((-Kr / Kg) * (2.0 - 2.0 * Kr))

#define By (1.0)
#define Bb (2.0 - 2.0 * Kb)
#define Br (0.0)

#else

//(JPEG for 601)
#define Ry (1.0)
#define Rb (0.0)
#define Rr (1.402)

#define Gy (1.0)
#define Gb (-0.344136)
#define Gr (-0.714136)

#define By (1.0)
#define Bb (1.772)
#define Br (0.0)

#endif

static inline double
GetCollor(uint16_t y, uint16_t b, uint16_t r, double cy, double cb, double cr)
{
	return 256.0 / 1024.0 * (cy * (0 + y) + cb * (-512 + b) + cr * (-512 + r));
}

static void
ConvYuv422beToRgb(uint8_t *rgb, uint8_t const *yuv, unsigned width, unsigned height, unsigned gaps)
{
	uint8_t const *bY = yuv;
	uint8_t const *bV = yuv + (width * height * 4) / 2;
	uint8_t const *bU = yuv + (width * height * 4) / 2 + (width * height * 4) / 4;
	uint16_t const *y1 = (uint16_t *)bY;
	uint16_t const *v = (uint16_t *)bV;
	uint16_t const *u = (uint16_t *)bU;
	viewRGB *i1 = (viewRGB *)rgb;

	for (int lc = height; lc > 0; lc -= 1)
	{
		for (int pc = width/2; pc > 0; pc -= 1)
		{
			uint16_t yle, vle, ule;

			vle = __bswap_16(*v);
			ule = __bswap_16(*u);

			yle = __bswap_16(*(y1 + 0));
			i1[0].r = GetCollor(yle, vle, ule, Ry, Rb, Rr);
			i1[0].g = GetCollor(yle, vle, ule, Gy, Gb, Gr);
			i1[0].b = GetCollor(yle, vle, ule, By, Bb, Br);

			yle = __bswap_16(*(y1 + 1));
			i1[1].r = GetCollor(yle, vle, ule, Ry, Rb, Rr);
			i1[1].g = GetCollor(yle, vle, ule, Gy, Gb, Gr);
			i1[1].b = GetCollor(yle, vle, ule, By, Bb, Br);

			y1 += 2;
			v += 1;
			u += 1;

			i1 += 2;
		}
		i1 += gaps;
	}
}

st_status_t
ShowFrame(view_info_t *view, uint8_t const*frame, int interlaced)
{
	view->cnt++;
	//
	SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 255);
	SDL_RenderClear(view->renderer);

	switch (view->bufFormat)
	{
	case ST21_BUF_FMT_RGBA_8BIT:
		SDL_UpdateTexture(view->texture, NULL, frame, view->width * sizeof(Uint32));
		break;
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		switch(interlaced) {
		case 0:
			ConvYuv422beToRgb(view->frame,               frame, view->width, view->height/2, view->width);
			break;
		case 1:
			ConvYuv422beToRgb(view->frame+view->width*4, frame, view->width, view->height/2, view->width);
			break;
		default:
			ConvYuv422beToRgb(view->frame,               frame, view->width, view->height, 0);
		}
		SDL_UpdateTexture(view->texture, NULL, view->frame, view->width * sizeof(Uint32));
		break;
	}

	SDL_RenderCopy(view->renderer, view->texture, NULL, NULL);
	SDL_RenderPresent(view->renderer);
	return ST_OK;
}

static void
RefreshLoopWaitEvent(SDL_Event *event)
{
	SDL_PumpEvents();
	while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
	{
		SDL_PumpEvents();
	}
}

// bellow is loop event for GUI - template
static void *
EventLoopThread(void *arg)
{
	SDL_Event event;

	while ((rte_atomic32_read(&howView) == 0) && (rte_atomic32_read(&isStop) == 0))
		rte_delay_us_sleep(500 * 1000);

	rte_delay_us_sleep(1000 * 1000);

	while (rte_atomic32_read(&isStop) == 0)
	{
		RefreshLoopWaitEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			continue;
			switch (event.key.keysym.sym)
			{
			case SDLK_f:
				break;
			case SDLK_p:
			case SDLK_SPACE:
				break;
			case SDLK_m:
				break;
			case SDLK_KP_MULTIPLY:
			case SDLK_0:
				break;
			case SDLK_KP_DIVIDE:
			case SDLK_9:
				break;
			case SDLK_s: // S: Step to next frame
				break;
			case SDLK_a:
				break;
			case SDLK_v:
				break;
			case SDLK_c:
				break;
			case SDLK_t:
				break;
			case SDLK_w:
				break;
			case SDLK_PAGEUP:
				break;
			case SDLK_PAGEDOWN:
				break;
			case SDLK_LEFT:
			case SDLK_RIGHT:
			case SDLK_UP:
			case SDLK_DOWN:
				break;
			default:
				break;
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEMOTION:
			break;
		case SDL_WINDOWEVENT:
			break;
		case SDL_QUIT:
			break;
		default:
			break;
		}
	}
	return NULL;
}

void
CloseViews(void)
{
	rte_atomic32_set(&isStop, 1);
	pthread_join(EventLoopThreadId, NULL);
	SDL_Quit();
}
