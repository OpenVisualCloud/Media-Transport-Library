/*
* Copyright (C) 2020-2021 Intel Corporation.
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
#include <rte_hexdump.h>

#include <byteswap.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>

struct video_stream_info
{
	char label[256];
	uint32_t format;
	uint32_t bufFormat;
	uint8_t *frame;
	int cnt;
	int width;
	int height;
	int id;
	LIST_ENTRY(video_stream_info) videoStreamInfo;
};
typedef struct video_stream_info video_stream_info_t;
LIST_HEAD(listVideoStreamHead, video_stream_info);

static struct listVideoStreamHead headOfListVideoSteams;
static struct video_stream_info *currentVideoStream = NULL;

struct gui_window
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	long int allFrames;
};

static struct gui_window guiWindow;

struct audio_ref
{
	const uint8_t *refBegin;  // begining of reference audio stream
	const uint8_t *refEnd;	  // end of reference audio stream
	const uint8_t *refFrame;  // current reference audio frame
	int fileFd;				  // Handle for reference audio file
};

struct anc_ref
{
	const uint8_t *refBegin;  // begining of reference audio stream
	const uint8_t *refEnd;	  // end of reference audio stream
	const uint8_t *refFrame;  // current reference audio frame
	int fileFd;				  // Handle for reference audio file
};

static pthread_t EventLoopThreadId;
static void *EventLoopThread(void *arg);
static rte_atomic32_t howView;
static rte_atomic32_t isStop;

static int default_width = 320;
static int default_height = 240;

static void
destroySDL()
{
	if (guiWindow.texture)
	{
		SDL_DestroyTexture(guiWindow.texture);
		guiWindow.texture = NULL;
	}
	if (guiWindow.renderer)
	{
		SDL_DestroyRenderer(guiWindow.renderer);
		guiWindow.renderer = NULL;
	}
	if (guiWindow.window)
	{
		SDL_DestroyWindow(guiWindow.window);
		guiWindow.window = NULL;
	}
	SDL_Quit();
}
st_status_t
CreateGuiWindow(void)
{
	int res;
	res = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
	if (res != 0)
	{
		printf("SDL_Init error: %s\n", SDL_GetError());
		return ST_GUI_ERR_NO_SDL;
	}
	memset(&guiWindow, 0, sizeof(guiWindow));
	guiWindow.window
		= SDL_CreateWindow("=== HELP ===", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
						   default_width, default_height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (guiWindow.window == NULL)
	{
		printf("could not create window: %s\n", SDL_GetError());
		destroySDL();
		return ST_GUI_ERR_NO_WINDOW;
	}

	guiWindow.renderer = SDL_CreateRenderer(guiWindow.window, -1, 0);
	if (guiWindow.renderer == NULL)
	{
		printf("could not create render: %s\n", SDL_GetError());
		destroySDL();
		return ST_GUI_ERR_NO_RENDER;
	}

	guiWindow.texture = SDL_CreateTexture(guiWindow.renderer, SDL_PIXELFORMAT_ARGB8888,
										  SDL_TEXTUREACCESS_STREAMING, 1920, 1080);
	if (guiWindow.texture == NULL)
	{
		printf("could not create texture: %s\n", SDL_GetError());
		destroySDL();
		return ST_GUI_ERR_NO_TEXTURE;
	}
	SDL_SetTextureBlendMode(guiWindow.texture, SDL_BLENDMODE_NONE);
	LIST_INIT(&headOfListVideoSteams);
	rte_atomic32_init(&howView);
	rte_atomic32_set(&howView, 0);
	rte_atomic32_init(&isStop);
	rte_atomic32_set(&isStop, 0);
	//Add help stream?
	res = pthread_create(&EventLoopThreadId, NULL, EventLoopThread, (void *)NULL);
	if (res != 0)
	{
		printf("SDL threat create error\n");
		destroySDL();
	}
	return res;
}

st_status_t
CreateAudioRef(audio_ref_t **ref)
{
	audio_ref_t *locRef = malloc(sizeof(audio_ref_t));
	if (locRef == NULL)
	{
		return ST_NO_MEMORY;
	}
	memset(locRef, 0, sizeof(audio_ref_t));
	*ref = locRef;

	return ST_OK;
}

st_status_t
CreateAncRef(anc_ref_t **ref)
{
	anc_ref_t *locRef = malloc(sizeof(anc_ref_t));
	if (locRef == NULL)
	{
		return ST_NO_MEMORY;
	}
	memset(locRef, 0, sizeof(anc_ref_t));
	*ref = locRef;

	return ST_OK;
}

st_status_t
AddStream(video_stream_info_t **videoStream, const char *label, st21_buf_fmt_t bufFormat, int width,
		  int height)
{
	video_stream_info_t *locVideoStream = NULL;
	uint8_t *frame = NULL;
	*videoStream = NULL;
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
	}
	locVideoStream = malloc(sizeof(video_stream_info_t));
	if (locVideoStream == NULL)
	{
		if (frame != NULL)
			free(frame);
		return ST_NO_MEMORY;
	}
	memset(locVideoStream, 0, sizeof(video_stream_info_t));
	locVideoStream->frame = frame;
	locVideoStream->width = width;
	locVideoStream->height = height;
	locVideoStream->bufFormat = bufFormat;
	snprintf(locVideoStream->label, sizeof(locVideoStream->label), "%s", label);
	LIST_INSERT_HEAD(&headOfListVideoSteams, locVideoStream, videoStreamInfo);
	*videoStream = locVideoStream;
	if (currentVideoStream == NULL)
	{
		currentVideoStream = locVideoStream;
		SDL_SetWindowTitle(guiWindow.window, label);
	}
	printf("\nSTREAM NAME: %s\n", label);
	return ST_OK;
}

void
PrepNext(void)
{
	video_stream_info_t *videoStream, *videoStremNext;
	videoStremNext = LIST_FIRST(&headOfListVideoSteams);
	do
	{
		videoStream = videoStremNext;
		videoStremNext = LIST_NEXT(videoStream, videoStreamInfo);
		if (videoStremNext == NULL)
			videoStremNext = LIST_FIRST(&headOfListVideoSteams);
	} while (videoStremNext != currentVideoStream);
	currentVideoStream = videoStream;
	SDL_SetWindowTitle(guiWindow.window, currentVideoStream->label);
}

static void
PrepPrev(void)
{
	video_stream_info_t *videoStream;
	videoStream = LIST_NEXT(currentVideoStream, videoStreamInfo);
	if (videoStream != NULL)
		currentVideoStream = videoStream;
	else
		currentVideoStream = LIST_FIRST(&headOfListVideoSteams);
	SDL_SetWindowTitle(guiWindow.window, currentVideoStream->label);
}

static void *
EventLoopThread(void *arg)
{
	SDL_Event event;
	while (rte_atomic32_read(&isStop) == 0)
	{
		do
		{
			if (rte_atomic32_read(&isStop) == 1)
				break;
			SDL_PumpEvents();
		} while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT));

		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym)
			{
			case SDLK_LEFT:
				break;
			case SDLK_RIGHT:
				break;
			case SDLK_UP:
				PrepNext();
				break;
			case SDLK_DOWN:
				PrepPrev();
				break;
			case SDLK_h:
				printf("\nSDL GUIL HELP\n"
					   "h  - display this help\n"
					   "Up - switch to next video strem\n"
					   "Dw - switch to previus video strem\n");
				break;
			default:
				break;
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEMOTION:
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

static inline uint8_t
GetCollor(uint16_t y, uint16_t b, uint16_t r, double cy, double cb, double cr)
{
	return (uint16_t)(cy * (0 + y) + cb * (-512 + b) + cr * (-512 + r)) >> 2;
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
		for (int pc = width / 2; pc > 0; pc -= 1)
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
ShowFrame(video_stream_info_t *stream, uint8_t const *frame, int interlaced)
{
	stream->cnt++;
	if (stream != currentVideoStream)
		return ST_OK;

	SDL_SetRenderDrawColor(guiWindow.renderer, 0, 0, 0, 255);
	SDL_RenderClear(guiWindow.renderer);

	switch (stream->bufFormat)
	{
	case ST21_BUF_FMT_RGBA_8BIT:
		SDL_UpdateTexture(guiWindow.texture, NULL, frame, stream->width * sizeof(Uint32));
		break;
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		switch (interlaced)
		{
		case 0:
			ConvYuv422beToRgb(stream->frame, frame, stream->width, stream->height / 2,
							  stream->width);
			break;
		case 1:
			ConvYuv422beToRgb(stream->frame + stream->width * 4, frame, stream->width,
							  stream->height / 2, stream->width);
			break;
		default:
			ConvYuv422beToRgb(stream->frame, frame, stream->width, stream->height, 0);
		}
		SDL_UpdateTexture(guiWindow.texture, NULL, stream->frame, stream->width * sizeof(Uint32));
		break;
	}

	SDL_RenderCopy(guiWindow.renderer, guiWindow.texture, NULL, NULL);
	SDL_RenderPresent(guiWindow.renderer);
	return ST_OK;
}

char *
AudioRefSelectFile(uint8_t bufFormat)
{
	char *filename = NULL;

	if (bufFormat == ST30_BUF_FMT_WAV)
	{
		filename = ST_DEFAULT_AUDIO;
	}
	return filename;
}

char *
AncRefSelectFile(uint8_t bufFormat)
{
	char *filename = NULL;

	if (bufFormat == ST40_BUF_FMT_CLOSED_CAPTIONS)
	{
		filename = ST_DEFAULT_ANCILIARY;
	}
	return filename;
}

st_status_t
AudioRefOpenFile(audio_ref_t *ref, const char *fileName)
{
	struct stat i;

	ref->fileFd = open(fileName, O_RDONLY);
	if (ref->fileFd >= 0)
	{
		fstat(ref->fileFd, &i);

		uint8_t *m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, ref->fileFd, 0);

		if (MAP_FAILED != m)
		{
			ref->refBegin = m;
			ref->refFrame = m;
			ref->refEnd = m + i.st_size;
		}
		else
		{
			RTE_LOG(ERR, USER1, "mmap fail '%s'\n", fileName);
			return ST_GENERAL_ERR;
		}
	}
	else
	{
		ref = NULL;
		RTE_LOG(INFO, USER1, "There are no audio file to compare!\n");
		return ST_GENERAL_ERR;
	}

	return ST_OK;
}

st_status_t
AncRefOpenFile(anc_ref_t *ref, const char *fileName)
{
	struct stat i;

	ref->fileFd = open(fileName, O_RDONLY);
	if (ref->fileFd >= 0)
	{
		fstat(ref->fileFd, &i);

		uint8_t *m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, ref->fileFd, 0);

		if (MAP_FAILED != m)
		{
			ref->refBegin = m;
			ref->refFrame = m;
			ref->refEnd = m + i.st_size;
		}
		else
		{
			RTE_LOG(ERR, USER1, "mmap fail '%s'\n", fileName);
			return ST_GENERAL_ERR;
		}
	}
	else
	{
		ref = NULL;
		RTE_LOG(INFO, USER1, "There are no anciliary file to compare\n");
		return ST_GENERAL_ERR;
	}

	return ST_OK;
}

st_status_t
PlayAudioFrame(audio_ref_t *ref, uint8_t const *frame, uint32_t frameSize)
{
	// Compare incoming audio frame with reference
	int status = -1;
	bool rewind = false;
	int count = 0;
	const uint8_t *old_ref = ref->refFrame;

	if (ref == NULL)
	{
		//There are no audio file
		return ST_OK;
	}

	while (status)
	{
		status = memcmp(frame, ref->refFrame, frameSize);
		// Calculate new reference frame
		ref->refFrame += frameSize;
		if ((ref->refFrame >= ref->refEnd) || ((ref->refEnd - ref->refFrame) < frameSize))
		{
			ref->refFrame = ref->refBegin;
		}

		if (status)
		{
			if (!rewind)
			{
				RTE_LOG(INFO, USER2, "Bad audio...rewinding...\n");
				rewind = true;
			}
			count++;
		}
		if (ref->refFrame == old_ref)
			break;
	}
	if (rewind)
	{
		RTE_LOG(INFO, USER2, "Audio rewind %d\n", count);
	}

	return ST_OK;
}

st_status_t
PlayAncFrame(anc_ref_t *ref, uint8_t const *frame, uint32_t frameSize)
{
	// Compare incoming anc frame with reference
	int status = 1;
	bool rewind = false;
	int count = 0;
	const uint8_t *old_ref = ref->refFrame;

	if (ref == NULL)
	{
		//There are no anciliary file
		return ST_OK;
	}

	while (status)
	{
		status = memcmp(frame, ref->refFrame, frameSize);
		// Calculate new reference frame
		ref->refFrame += frameSize;
		if ((ref->refFrame >= ref->refEnd) || ((ref->refEnd - ref->refFrame) < frameSize))
		{
			ref->refFrame = ref->refBegin;
		}
		if (status)
		{
			if (!rewind)
			{
				RTE_LOG(INFO, USER2, "Bad anc...rewinding");
				rewind = true;
			}
			count++;
		}
		if (ref->refFrame == old_ref)
			break;
	}
	if (rewind)
	{
		RTE_LOG(INFO, USER2, "ANC rewind %d\n", count);
	}

	return ST_OK;
}

void
DestroyGui(void)
{
	if (!DoesGuiExist())
		return;
	rte_atomic32_set(&isStop, 1);
	pthread_join(EventLoopThreadId, NULL);
	destroySDL();
}

bool
DoesGuiExist(void)
{
	return guiWindow.window != NULL && guiWindow.renderer != NULL && guiWindow.texture != NULL;
}
