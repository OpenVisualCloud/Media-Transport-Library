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
#define _GNU_SOURCE

#include "common_app.h"
#include "rx_view.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdatomic.h>
#include <numa.h>

// #define RX_APP_DEBUG 1
// #define DEBUG 1
#define ST_ESSENCE_NUM 3
#define ST_DEV_TYPES 2

static volatile atomic_uint_fast32_t isAppStopped;
static bool audioCmp = false;
static bool ancCmp = false;

static volatile cpu_set_t affinityCore_Application;
static cpu_set_t affinityCore_toUse[ST_DEV_TYPES][ST_ESSENCE_NUM];
static uint16_t coreIndex_toUse[ST_DEV_TYPES][ST_ESSENCE_NUM];
static pthread_mutex_t lock;

static const char *essence_type_name[ST_ESSENCE_MAX] = {
	[ST_ESSENCE_VIDEO] = "video",
	[ST_ESSENCE_AUDIO] = "audio",
	[ST_ESSENCE_ANC] = "ancilary",
};

void
SetAffinityCore(void *app, st_dev_type_t type)
{
	pthread_t thread;
	st_essence_type_t etype;
	if (type == ST_DEV_TYPE_PRODUCER)
	{
		etype = ((strtp_send_app_t *)app)->mtype;
		thread = ((strtp_send_app_t *)app)->movieThread;
	}
	else
	{
		etype = ((strtp_recv_app_t *)app)->session->type;
		thread = ((strtp_recv_app_t *)app)->writeThread;
	}

	cpu_set_t app_cpuset;
	CPU_ZERO(&app_cpuset);

	pthread_mutex_lock(&lock);

	if (CPU_COUNT(&affinityCore_toUse[type][etype]) == 0)
	{
		coreIndex_toUse[type][etype] = 0;
		CPU_OR(&affinityCore_toUse[type][etype], (cpu_set_t *)&affinityCore_Application,
			   (cpu_set_t *)&affinityCore_Application);
	}

	while (coreIndex_toUse[type][etype]++ < get_nprocs_conf())
	{
		if (CPU_ISSET(coreIndex_toUse[type][etype], &affinityCore_toUse[type][etype]))
		{
			CPU_CLR(coreIndex_toUse[type][etype], &affinityCore_toUse[type][etype]);
			CPU_CLR(coreIndex_toUse[type][etype], &affinityCore_Application);
			CPU_SET(coreIndex_toUse[type][etype], &app_cpuset);
			break;
		}
	}
	if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &app_cpuset) == 0)
	{
		printf("INFO USER1: ****** %s affinity set successfully in %d\n",
				essence_type_name[etype], coreIndex_toUse[type][etype]);
	}
	else
	{
		printf("ERR USER1: ****** %s affinity set fail %d\n", essence_type_name[etype],
				coreIndex_toUse[type][etype]);
	}
	pthread_mutex_unlock(&lock);
}

void
AppInitAffinity(int appStartCoreId)
{
	static bool getAffnity = 0;

	if (getAffnity == 0)
	{
		getAffnity = 1;
		cpu_set_t myCpu;
		CPU_ZERO(&myCpu);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_PRODUCER][0]);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_PRODUCER][1]);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_PRODUCER][2]);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_CONSUMER][0]);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_CONSUMER][1]);
		CPU_ZERO(&affinityCore_toUse[ST_DEV_TYPE_CONSUMER][2]);

		StGetAppAffinityCores(appStartCoreId, &myCpu);
		CPU_OR((cpu_set_t *)&affinityCore_Application, &myCpu, &myCpu);
		pthread_mutex_init(&lock, NULL);
		printf("INFO USER1: ****** App available cpu count %u\n", CPU_COUNT((cpu_set_t *)&myCpu));
	}
}

static inline uint8_t
RecvAppClamp(float value)
{
	if (value < 0.0)
		return 0;
	if (value > 255.0)
		return 255;
	return (uint8_t)value;
}

static inline void
RecvAppLock(strtp_recv_app_t *app)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&app->lock, 1);
	} while (lock != 0);
}

static inline void
RecvAppUnlock(strtp_recv_app_t *app)
{
	__sync_lock_release(&app->lock, 0);
}

static inline void
RecvAppWriteFrameRgbaInline(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr,
							st_vid_fmt_conv_t convert)
{
	st_rgba_8b_t *dualPix = (st_rgba_8b_t *)(app->movie + app->movieCursor);
	uint32_t movieEndOffset = app->movieBufSize / sizeof(st_rgba_8b_t);
	st_rgba_8b_t *const end = dualPix + movieEndOffset;

	for (; dualPix < end; dualPix += 8)
	{
		uint16_t Y0, Y1, Cr, Cb;

		for (uint32_t j = 0; j < 4; j++)
		{
			switch (convert)  // will inline
			{
			case ST_422_FMT_CONV_NET_LE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_LE10_BUF_BGRA:
				Unpack_PG2le_422le10(ptr, &Cr, &Y0, &Cb, &Y1);
				break;
			case ST_422_FMT_CONV_NET_BE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_BE10_BUF_BGRA:
				Unpack_PG2be_422le10(ptr, &Cr, &Y0, &Cb, &Y1);
				break;
			default:
				ST_APP_ASSERT;
			}
			ptr++;
			float R1 = (1.164 * (Y0 - 64) + 1.793 * (Cr - 512));
			float G1 = (1.164 * (Y0 - 64) - 0.534 * (Cr - 512) - 0.213 * (Cb - 512));
			float B1 = (1.164 * (Y0 - 64) + 2.115 * (Cb - 512));
			float R2 = (1.164 * (Y1 - 64) + 1.793 * (Cr - 512));
			float G2 = (1.164 * (Y1 - 64) - 0.534 * (Cr - 512) - 0.213 * (Cb - 512));
			float B2 = (1.164 * (Y1 - 64) + 2.115 * (Cb - 512));
			dualPix[0 + 2 * j].r = RecvAppClamp(R1 / 4.0);
			dualPix[0 + 2 * j].g = RecvAppClamp(G1 / 4.0);
			dualPix[0 + 2 * j].b = RecvAppClamp(B1 / 4.0);
			dualPix[0 + 2 * j].a = 255;
			dualPix[1 + 2 * j].r = RecvAppClamp(R2 / 4.0);
			dualPix[1 + 2 * j].g = RecvAppClamp(G2 / 4.0);
			dualPix[1 + 2 * j].b = RecvAppClamp(B2 / 4.0);
			dualPix[1 + 2 * j].a = 255;
		}
	}
	app->movieCursor = (app->movieCursor + app->movieBufSize) % app->movieSize;
}

static inline void
RecvAppWriteFrame422Inline(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr,
						   void (*Unpack)(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00,
										  uint16_t *y00, uint16_t *cr00, uint16_t *y01))
{
	st_rfc4175_422_10_pg2_t const *const end = ptr + (app->frameSize / sizeof(*ptr));
	uint16_t *Yy = (uint16_t *)(app->movie + app->movieCursor + 0 * (app->movieBufSize / 4));
	uint16_t *Cr = (uint16_t *)(app->movie + app->movieCursor + 2 * (app->movieBufSize / 4));
	uint16_t *Br = (uint16_t *)(app->movie + app->movieCursor + 3 * (app->movieBufSize / 4));
	for (; ptr < end; ++ptr)
	{
		Unpack(ptr, Cr++, Yy + 0, Br++, Yy + 1);
		Yy += 2;
	}
	app->movieCursor = (app->movieCursor + app->movieBufSize) % app->movieSize;
}

static inline void
RecvAppWriteAudioBuffer(strtp_recv_app_t *app, const unsigned char *buffer)
{
	if ((app->movieCursor + app->session->frameSize) > app->movieSize)
	{
		app->movieCursor = 0;
	}
#ifdef RX_APP_DEBUG
	printf("Write: movie=%x movieCursor=%x size=%u movie size%u from buffer:%x data:%x%x%x%x\n",
		   (unsigned int)app->movie, (unsigned int)app->movieCursor, app->session->frameSize,
		   app->movieSize, buffer, buffer[0], buffer[1], buffer[2], buffer[3]);
#endif /* RX_APP_DEBUG */
	memcpy((uint8_t *)app->movie + app->movieCursor, buffer, app->session->frameSize);
	app->movieCursor = (app->movieCursor + app->session->frameSize) % app->movieSize;
}

static void
RecvAppWriteFrameNetLeBufBe(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2le_422be10);
}

static void
RecvAppWriteFrameNetBeBufBe(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2be_422be10);
}

void
RecvAppWriteFrameNetLeBufRgba(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_LE10_BUF_RGBA);
}

void
RecvAppWriteFrameNetBeBufRgba(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_BE10_BUF_RGBA);
}

void
RecvAppWriteFrameNetLeBufBgra(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_LE10_BUF_BGRA);
}

void
RecvAppWriteFrameNetBeBufBgra(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_BE10_BUF_BGRA);
}

static void *
RecvAppThread(strtp_recv_app_t *app)
{
	uint8_t *frameBuf;
	st_status_t status;
	uint8_t *sharedMv;

	SetAffinityCore(app, ST_DEV_TYPE_CONSUMER);

	app->fpsFrameCnt = 0;
	app->fpsLastTimeNs = 0;

	while (atomic_load(&isAppStopped) == 0)
	{
		uint32_t const old = app->frmsRecv;
		while ((old == app->frmsRecv) && (atomic_load(&isAppStopped) == 0))
		{
			sleep(0);  // wait for next frame
		}

		if (app->session->type == ST_ESSENCE_VIDEO)
		{
			RecvAppLock(app);
			frameBuf = app->frames[app->writeCursor];
			sharedMv = app->movie + app->movieCursor;
			RecvAppUnlock(app);
			app->RecvAppWriteFrame(app, (st_rfc4175_422_10_pg2_t const *)frameBuf);
			if (app->videoStream != NULL)
				ShowFrame(app->videoStream, sharedMv, app->fieldId);
#if ST_APP_FPS_SHOW
			app->fpsFrameCnt += app->frmsRecv - old;
			if (app->fpsFrameCnt >= ST_APP_FPS_FRAME_INTERVAL)
			{
				uint64_t curTimeNs = StGetMonotonicTimeNano();

				if (app->fpsLastTimeNs)
				{
					double timeSec = (double)(curTimeNs - app->fpsLastTimeNs) / NS_PER_S;
					double frameRate = app->fpsFrameCnt / timeSec;
					printf("INFO USER2: RecvApp[%02d], Frame Rate = %4.2lf\n", app->index, frameRate);
				}

				app->fpsFrameCnt = 0;
				app->fpsLastTimeNs = curTimeNs;
			}
#endif
		}
		else if (app->session->type == ST_ESSENCE_AUDIO)
		{
			RecvAppLock(app);
			while (app->framesToRead > 0)
			{
				frameBuf = app->samples[app->readCursor];
				app->readCursor = (app->readCursor + 1) % RECV_APP_SAMPLE_MAX;
				sharedMv = app->movie + app->movieCursor;
				app->RecvAppWriteAudioFrame(app, frameBuf);
				app->framesToRead--;
				if (audioCmp)
				{
					status = PlayAudioFrame(app->ref, sharedMv, app->session->frameSize);
					if (status != ST_OK)
						printf("INFO USER1"
								"Cursor - (write:%u,read:%u), frames - (ToRead:%u,Recv:%u) "
								"session:%d\n",
								app->writeCursor, app->readCursor, app->framesToRead, app->frmsRecv,
								app->session->ssid);
				}
			}
			RecvAppUnlock(app);
		}
		else if ((app->session->type == ST_ESSENCE_ANC) && ancCmp)
		{
			RecvAppLock(app);
			while (app->framesToRead > 0)
			{
				int offset = app->ancFrames[app->readCursor].meta[0].udwOffset;
				int count = app->ancFrames[app->readCursor].meta[0].udwSize;
				status = PlayAncFrame(app->ancref,
									  app->ancFrames[app->readCursor].data + offset, count);
				if (status != ST_OK)
					printf("ERR USER1:"
							"Anc Frame check failure: Cursor - (write:%u,read:%u), frames - "
							"(ToRead:%u,Recv:%u) session:%d\n",
							app->writeCursor, app->readCursor, app->framesToRead, app->frmsRecv,
							app->session->ssid);
				app->readCursor = (app->readCursor + 1) % RECV_APP_FRAME_MAX;
				app->framesToRead--;
			}
			RecvAppUnlock(app);
		}
	}
	return app;
}

st_status_t
RecvAppInit(strtp_recv_app_t *app)
{
	st_format_t vfmt;
	StGetFormat(app->session, &vfmt);

	st21_format_t *fmt = &vfmt.v;

	if (app->session->type == ST_ESSENCE_VIDEO)
	{
		switch (app->bufFormat)
		{
		case ST21_BUF_FMT_YUV_422_10BIT_BE:
			app->frames[0] = malloc(RECV_APP_FRAME_MAX * app->session->frameSize);
			if (!app->frames[0])
			{
				return ST_NO_MEMORY;
			}
			for (uint32_t i = 1; i < RECV_APP_FRAME_MAX; i++)
			{
				app->frames[i] = app->frames[0] + i * app->session->frameSize;
			}
			// for now there is only BE output buffer format
			app->movieBufSize = 2 * sizeof(uint16_t) * fmt->width * fmt->height;
			switch (fmt->vscan)
			{
			case ST21_720I:
			case ST21_1080I:
			case ST21_2160I:
				app->movieBufSize /= 2;
				snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.yuv422p10be.yuv",
						 app, fmt->width, fmt->height / 2);
				break;
			default:
				snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.yuv422p10be.yuv",
						 app, fmt->width, fmt->height);
			}
			switch (fmt->pixelFmt)
			{
			case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
				app->RecvAppWriteFrame = RecvAppWriteFrameNetBeBufBe;
				break;
			case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
				app->RecvAppWriteFrame = RecvAppWriteFrameNetLeBufBe;
				break;
			default:
				ST_APP_ASSERT;
				return ST_NOT_SUPPORTED;
			}
			break;

		case ST21_BUF_FMT_RGBA_8BIT:
			snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.rgba", app,
					 fmt->width, fmt->height);
			app->movieBufSize = sizeof(st_rgba_8b_t) * fmt->width * fmt->height;
			switch (fmt->pixelFmt)
			{
			case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
				app->RecvAppWriteFrame = RecvAppWriteFrameNetBeBufRgba;
				break;
			case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
				app->RecvAppWriteFrame = RecvAppWriteFrameNetLeBufRgba;
				break;
			default:
				ST_APP_ASSERT;
				return ST_NOT_SUPPORTED;
			}
			break;

		default:
			ST_APP_ASSERT;
			return ST_NOT_SUPPORTED;
		}
	}
	else if (app->session->type == ST_ESSENCE_AUDIO)
	{
		switch ((st30_buf_fmt_t)app->bufFormat)
		{
		case ST30_BUF_FMT_WAV:
			app->samples[0] = malloc(RECV_APP_SAMPLE_MAX * app->session->frameSize);
			if (!app->samples[0])
			{
				return ST_NO_MEMORY;
			}
			for (uint32_t i = 1; i < RECV_APP_SAMPLE_MAX; i++)
			{
				app->samples[i] = app->samples[0] + i * app->session->frameSize;
			}
			// TBD what Audio Buffer size do we want
			//	Temporary size - TBD it must be aligned to audio sample packet size
			app->movieBufSize = 192 * 102400;
			snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.wav", app);
			app->RecvAppWriteAudioFrame = RecvAppWriteAudioBuffer;
			break;

		default:
			ST_APP_ASSERT;
			return ST_NOT_SUPPORTED;
		}
	}
	else
	{
		switch ((st40_buf_fmt_t)app->bufFormat)
		{
		case ST40_BUF_FMT_CLOSED_CAPTIONS:
			app->ancFrames[0].data = malloc(RECV_APP_FRAME_MAX * app->session->frameSize);
			if (!app->ancFrames[0].data)
			{
				return ST_NO_MEMORY;
			}
			for (uint32_t i = 1; i < RECV_APP_FRAME_MAX; i++)
			{
				app->ancFrames[i].data = app->ancFrames[0].data + i * app->session->frameSize;
			}
			break;
		default:
			ST_APP_ASSERT;
			return ST_NOT_SUPPORTED;
		}
	}

	if (app->session->type == ST_ESSENCE_VIDEO)
	{
		app->dualPixelSize = (2 * fmt->pixelGrpSize) / fmt->pixelsInGrp;
		app->sliceSize = 20 *  // at least 20 lines if single pixel group usually 40 lines
						 fmt->width * fmt->pixelGrpSize;
		app->sliceCount = app->session->frameSize / app->sliceSize;
		app->pixelGrpsInSlice = app->sliceSize / fmt->pixelGrpSize;
		app->linesInSlice = 40;	 // for now TBD
	}

	app->frameSize = app->session->frameSize;

#ifdef DEBUG
	if (app->session->type == ST_ESSENCE_VIDEO)
	{
		printf("DEBUG USER2: RecvApp dualPixelSize %u sliceSize %u sliceCount %u\n",
				app->dualPixelSize, app->sliceSize, app->sliceCount);
		printf("DEBUG USER2: RecvApp pixelGrpsInSlice %u linesInSlice %u frameSize %u\n",
				app->pixelGrpsInSlice, app->linesInSlice, app->frameSize);
	}
	else
	{
		printf("DEBUG USER2: RecvApp frameSize %u\n", app->frameSize);
	}
#endif

	if (app->session->type != ST_ESSENCE_ANC)
	{
		app->movieCursor = 0;
		// TODO - compe up with reasonable size
		app->movieSize = 4 * app->movieBufSize;
		uint32_t fileSizeBytes = app->movieSize;

#ifdef DEBUG
		RTE_LOG(DEBUG, USER2, "Opening Rx file: %s size %d\n", app->fileName, fileSizeBytes);
#endif

		app->fileFd = open(app->fileName, O_CREAT | O_RDWR, 0640);
		int ret = ftruncate(app->fileFd, fileSizeBytes);
		if (ret < 0)
		{
			printf("ERR USER2: Opening Rx file: %s failed\n", app->fileName);
			return ST_GENERAL_ERR;
		}
		app->movie = mmap(NULL, fileSizeBytes, PROT_READ | PROT_WRITE, MAP_SHARED, app->fileFd, 0);
	}

	app->writeCursor = 0;
	app->readCursor = 0;
	app->framesToRead = 0;
	app->inputCursor = 0;
	app->frmsRecv = 0;

	atomic_init(&isAppStopped, 0);

	pthread_create(&app->writeThread, NULL, (void *(*)(void *))RecvAppThread, (void *)app);

	return ST_OK;
}

static void
RecvInitSt21Cons(st_session_t *sn, st21_consumer_t *cons, void *app)
{

	memset((void *)cons, 0, sizeof(st21_consumer_t));
	cons->appHandle = app;
	cons->frameSize = sn->frameSize;
	/* TODO
      * consumer type is provided through user command
      */
	cons->consType = ST21_CONS_P_FRAME;

	cons->St21NotifyFrameRecv = RecvAppNotifyFrameRecv;
	cons->St21NotifyFrameDone = RecvAppNotifyFrameDone;
	cons->St21GetNextFrameBuf = RecvAppGetNextFrameBuf;
	cons->St21PutFrameTmstamp = RecvAppPutFrameTmstamp;
}
static void
RecvInitSt30Cons(st_session_t *sn, st30_consumer_t *cons, void *app)
{

	memset((void *)cons, 0, sizeof(st30_consumer_t));
	cons->appHandle = app;
	cons->bufSize = sn->frameSize;
	cons->consType = ST30_CONS_REGULAR;
	/* TODO
      * consumer type is provided through user command
      */

	/* TODO
     * add consumer callback
     */
	cons->St30GetNextAudioBuf = RecvAppGetNextAudioBuf;
	cons->St30NotifySampleRecv = RecvAppNotifySampleRecv;
	cons->St30NotifyBufferDone = RecvAppNotifyBufferDone;
	cons->St30NotifyStopDone = NULL;
	cons->St30RecvRtpPkt = NULL;
}

/**
 * Callback to consumer application with notification about the buffer completion
 * Audio buffer can be released or reused after it but not sooner
 */
void
RecvAppNotifyAncFrameDone(void *appHandle, void *frameBuf)
{
#ifdef DEBUG
	printf("DEBUG USER3: RecvAppNotifyAncFrameDone frameBuf:%" PRIxPTR "\n", (uintptr_t)frameBuf);
#endif
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;

	RecvAppLock(app);

	for (uint32_t i = 0; i < RECV_APP_FRAME_MAX; i++)
	{
		if (&(app->ancFrames[i]) == frameBuf)
		{
			app->writeCursor = i;
			app->framesToRead++;
			break;
		}
	}
	app->frmsRecv++;
	RecvAppUnlock(app);
#ifdef DEBUG
	printf("DEBUG USER3: AncBuf %p\n", frameBuf);
#endif
	return;
}

/**
 * Callback to consumer application to get next audio buffer necessary to continue streaming
 */
void *
RecvAppGetNextAncFrame(void *appHandle)
{
	strtp_ancFrame_t *nextFrame;
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;
	RecvAppLock(app);
	nextFrame = &(app->ancFrames[app->inputCursor]);
	app->inputCursor = (app->inputCursor + 1) % RECV_APP_FRAME_MAX;
	nextFrame->dataSize = 0;
	nextFrame->metaSize = 0;
	RecvAppUnlock(app);
#ifdef DEBUG
	printf("DEBUG USER3: RecvAppGetNextAncFrame %" PRIxPTR "\n", (uintptr_t)nextFrame);
#endif
	return nextFrame;
}

static void
RecvInitSt40Cons(st_session_t *sn, st40_consumer_t *cons, void *app)
{

	memset((void *)cons, 0, sizeof(st40_consumer_t));
	cons->appHandle = app;
	cons->bufSize = sn->frameSize;
	cons->consType = ST40_CONS_REGULAR;
	cons->St40GetNextAncFrame = RecvAppGetNextAncFrame;
	cons->St40NotifyFrameDone = RecvAppNotifyAncFrameDone;
}

/**
 * Callback to producer or consumer application with notification about the buffer completion
 * Audio buffer can be released or reused after it but not sooner
 */
void
RecvAppNotifyBufferDone(void *appHandle, uint8_t *frameBuf)
{
#ifdef DEBUG
	printf("DEBUG USER3: RecvAppNotifyBufferDone frameBuf:%" PRIxPTR "\n", (uintptr_t)frameBuf);
#endif
	return;
}



/**
 * Callback to producer application to get next audio buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and TBD then has to call St30ProducerUpdate
 * to restart streaming
 */
uint8_t *
RecvAppGetNextAudioBuf(void *appHandle, uint8_t *prevAudioBuf, uint32_t bufSize, uint32_t *tmstamp)
{
	uint8_t *nextBuf;
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;
	if (bufSize != app->frameSize)
		return NULL;

	RecvAppLock(app);
	nextBuf = app->samples[app->inputCursor];
	app->inputCursor = (app->inputCursor + 1) % RECV_APP_SAMPLE_MAX;
	RecvAppUnlock(app);
#ifdef DEBUG
	printf("DEBUG USER3: RecvAppGetNextAudioBuf %" PRIxPTR "\n", (uintptr_t)nextBuf);
#endif
	return nextBuf;
}

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void
RecvAppNotifySampleRecv(void *appHandle, uint8_t *audioBuf, uint32_t bufOffset, uint32_t tmstamp)
{
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;

	RecvAppLock(app);

	for (uint32_t i = 0; i < RECV_APP_SAMPLE_MAX; i++)
	{
		if (app->samples[i] == audioBuf)
		{
			app->writeCursor = i;
			app->framesToRead++;
			break;
		}
	}
	app->frmsRecv++;
	RecvAppUnlock(app);
#ifdef DEBUG
	printf("DEBUG USER3: RecvAppNotifySampleRecv %" PRIxPTR " tmstamp %u\n", (uintptr_t)audioBuf, tmstamp);
#endif
}

static int
RecvInitConsumer(st_session_t *sn, strtp_recv_app_t *app, void *cons)
{
	st_status_t status = ST_NO_MEMORY;
	char *filename = NULL;

	switch (sn->type)
	{
	case ST_ESSENCE_AUDIO:
		// Open Audio Reference file
		status = CreateAudioRef(&app->ref);
		if (status != ST_OK)
		{
			printf("ERR USER1: CreateRef FAILED. ErrNo: %d\n", status);
			return status;
		}
		filename = AudioRefSelectFile(app->bufFormat);
		status = AudioRefOpenFile(app->ref, filename);
		if (status != ST_OK)
		{
			printf("INFO USER2: AudioRefOpenFile error of %d, no audio compare\n", status);
			free(app->ref);
		}
		else
			audioCmp = true;

		RecvInitSt30Cons(sn, cons, app);
		status = ST_OK;
		break;

	case ST_ESSENCE_VIDEO:
		RecvInitSt21Cons(sn, cons, app);
		status = ST_OK;
		break;

	case ST_ESSENCE_ANC:
		// Open Ancillary Reference file
		status = CreateAncRef(&app->ancref);
		if (status != ST_OK)
		{
			printf("ERR USER1: CreateRef FAILED. ErrNo: %d\n", status);
			return status;
		}
		filename = AncRefSelectFile(app->bufFormat);
		status = AncRefOpenFile(app->ancref, filename);
		if (status != ST_OK)
		{
			printf("INFO USER2: AncRefOpenFile error of %d, no Anc Compare\n", status);
			free(app->ancref);
		}
		else
			ancCmp = true;
		RecvInitSt40Cons(sn, cons, app);
		status = ST_OK;
		break;

	default:
		break;
	}
	return status;
}

st_status_t
RecvAppCreateConsumer(st_session_t *sn, st21_buf_fmt_t bufFormat, strtp_recv_app_t **appOut)
{
	st21_consumer_t cons;

	if (!sn)
		return ST_INVALID_PARAM;
	strtp_recv_app_t *app = numa_alloc_local( sizeof(strtp_recv_app_t));
	if (app)
	{
		memset(app, 0x0, sizeof(strtp_recv_app_t));
		app->session = sn;
		app->bufFormat = bufFormat;
		st_status_t status = RecvAppInit(app);
		if (status == ST_OK)
		{
			status = RecvInitConsumer(sn, app, (void *)&cons);
			if (status != ST_OK)
			{
				printf("INFO USER2: RecvInitConsumer FAILED. ErrNo: %d\n", status);
				numa_free(app, sizeof(strtp_recv_app_t));
				return status;
			}
			status = StRegisterConsumer(sn, (void *)&cons);
			if (status != ST_OK)
			{
				printf("INFO USER2: RecvInitConsumer FAILED. ErrNo: %d\n", status);
				numa_free(app, sizeof(strtp_recv_app_t));
				return status;
			}
			*appOut = app;
		}
		else
		{
			printf("INFO USER3: RecvAppInit error of %d\n", status);
		}
		// ToDo ~RecvAppDeInit - app contain open file!
		return status;
	}

	return ST_NO_MEMORY;
}

st_status_t
RecvAppStart(st_session_t *sn, strtp_recv_app_t *app)
{
	st_status_t status = ST_OK;
	if (!sn || !app)
		return ST_INVALID_PARAM;

	if (sn->type == ST_ESSENCE_VIDEO)
		status = St21ConsumerStartFrame(sn, app->frames[0], 0);
	else if (sn->type == ST_ESSENCE_AUDIO)
		status = St30ConsumerStartFrame(sn, app->samples[0], 0);
	else if (sn->type == ST_ESSENCE_ANC)
		status = St40ConsumerStartFrame(sn);
	return status;
}

st_status_t
RecvAppStop(st_session_t *sn, strtp_recv_app_t *app)
{
	st_status_t status = ST_OK;

	if (!sn || !app)
		return ST_INVALID_PARAM;

	atomic_store(&isAppStopped, 1);
	(void)pthread_join(app->writeThread, NULL);

	status = ConsumerStop(sn);
	if (status != ST_OK)
	{
		printf("ERR USER1: ConsumerStop FAILED. ErrNo: %d\n", status);
		return status;
	}
	if (app->session->type == ST_ESSENCE_VIDEO)
	{
		free(app->frames[0]);
	}
	if (app->session->type == ST_ESSENCE_AUDIO)
	{
		free(app->samples[0]);
	}
	if (app->session->type == ST_ESSENCE_ANC)
	{
		free(app->ancFrames[0].data);
	}

	return status;
}

/**
 * Callback to producer application to get next frame buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and then has to call St21ProducerUpdate
 * to restart streaming
 */
uint8_t *
RecvAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize, uint32_t *tmstamp, uint32_t fieldId)
{
	uint8_t *nextBuf;
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;
	if (bufSize != app->frameSize)
		return NULL;

	nextBuf = app->frames[app->inputCursor];
	app->inputCursor = (app->inputCursor + 1) % RECV_APP_FRAME_MAX;

	return nextBuf;
}

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
void
RecvAppNotifyFrameDone(void *appHandle, uint8_t *frameBuf, uint32_t fieldId)
{
	// printf("INFO USER3: RecvAppNotifyFrameDone\n");
	// free(frameBuf);
	return;
}

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void
RecvAppNotifyFrameRecv(void *appHandle, uint8_t *frameBuf, uint32_t tmstamp, uint32_t fieldId)
{
	strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;

	if (!frameBuf)
	{
	#ifdef DEBUG
		printf("DEBUG USER1: %s(%d), null frame for field %u\n", __func__, app->index, fieldId);
	#endif
		return;
	}

	RecvAppLock(app);

	for (uint32_t i = 0; i < RECV_APP_FRAME_MAX; i++)
	{
		if (app->frames[i] == frameBuf)
		{
			app->writeCursor = i;
			break;
		}
	}
	RecvAppUnlock(app);
#ifdef DEBUG
	printf("DEBUG USER1: %s(%d), field %u %p tmstamp %u\n", __func__, app->index, fieldId, frameBuf, tmstamp);
#endif
	app->fieldId = fieldId;
	app->frmsRecv++;
}

void
RecvAppPutFrameTmstamp(void *appHandle, uint32_t tmstamp)
{
	//strtp_recv_app_t *app = (strtp_recv_app_t *)appHandle;

	// printf("Received timestamp of during frame of %llu of %u\n", (U64)app->frmsRecv, tmstamp);
	return;
}
