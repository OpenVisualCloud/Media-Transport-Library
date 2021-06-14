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

#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SENDER_APP_DEBUG
#define ST_OPT_SIZE 24
//#define SENDER_APP_LOGS

static inline void
SendAppLock(strtp_send_app_t *app)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&app->lock, 1);
	} while (lock != 0);
}

static inline void
SendAppUnlock(strtp_send_app_t *app)
{
	__sync_lock_release(&app->lock, 0);
}

static inline uint32_t
SendAppFetchFrameCursor(strtp_send_app_t *app)
{
	st21_producer_t *prod21;
	st30_producer_t *prod30;
	st40_producer_t *prod40;
	uint32_t cursor = 0;

	if (app->mtype == ST_ESSENCE_VIDEO)
	{
		prod21 = (st21_producer_t *)app->prod;
		cursor = prod21->frameCursor;
	}
	else if (app->mtype == ST_ESSENCE_AUDIO)
	{
		prod30 = (st30_producer_t *)app->prod;
		cursor = prod30->frameCursor;
	}
	else if (app->mtype == ST_ESSENCE_ANC)
	{
		prod40 = (st40_producer_t *)app->prod;
		cursor = prod40->frameCursor;
	}

	return cursor;
}

static inline void
SendAppWaitFrameDone(strtp_send_app_t *app)
{
	uint8_t done;
	uint32_t cursor;

	cursor = SendAppFetchFrameCursor(app);

	do
	{
		done = __sync_lock_test_and_set(&app->frameDone[cursor], 1);
	} while (done != 0);
}

static inline st_status_t
SendAppReadFrameRgbaInline(strtp_send_app_t *app, st_vid_fmt_conv_t convert)
{
	uint32_t cursor;
	st21_producer_t *prod;

	SendAppLock(app);
	prod = (st21_producer_t *)app->prod;

	cursor = SendAppFetchFrameCursor(app);
	prod->frameBuf = app->frames[cursor];
	SendAppUnlock(app);

	SendAppWaitFrameDone(app);

	st_rfc4175_422_10_pg2_t *dst = (st_rfc4175_422_10_pg2_t *)prod->frameBuf;
	st_rfc4175_422_10_pg2_t *const end
		= (st_rfc4175_422_10_pg2_t *)(prod->frameBuf + prod->frameSize);

	// read frame from movie and convert into frameBuf
	for (; dst < end; dst += 4)
	{
		for (uint32_t j = 0; j < 4; j++)
		{
			float fr1, fg1, fb1;
			float fy0, fcb0, fcr0;
			float fr2, fg2, fb2;
			float fy1;
			//float fcb1, fcr1;

			if ((convert == ST_422_FMT_CONV_NET_LE10_BUF_BGRA)
				|| (convert == ST_422_FMT_CONV_NET_BE10_BUF_BGRA))
			{
				fb1 = (float)(app->movie[0 + j * 8]);
				fg1 = (float)(app->movie[1 + j * 8]);
				fr1 = (float)(app->movie[2 + j * 8]);
				fb2 = (float)(app->movie[4 + j * 8]);
				fg2 = (float)(app->movie[5 + j * 8]);
				fr2 = (float)(app->movie[6 + j * 8]);
			}
			else
			{
				fr1 = (float)(app->movie[0 + j * 8]);
				fg1 = (float)(app->movie[1 + j * 8]);
				fb1 = (float)(app->movie[2 + j * 8]);
				fr2 = (float)(app->movie[4 + j * 8]);
				fg2 = (float)(app->movie[5 + j * 8]);
				fb2 = (float)(app->movie[6 + j * 8]);
			}

			// fy0 = 0.2126 * fr1 +  0.7152 * fg1 + 0.0722 * fb1;
			// fcb0 = -0.117 * fr1 - 0.394 * fb1 + 0.511 * fb1 + 128;
			// fcr0 = 0.511 * fr1 - 0.464 * fg1 - 0.047 * fb1 + 128;

			// fy1 = 0.2126 * fr2 +  0.7152 * fg2 + 0.0722 * fb2;
			// fcb1 = -0.117 * fr2 - 0.394 * fb2 + 0.511 * fb2 + 128;
			// fcr1 = 0.511 * fr2 - 0.464 * fg2 - 0.047 * fb2 + 128;

			fy0 = 0.183 * fr1 + 0.614 * fg1 + 0.0622 * fb1 + 16;
			fcb0 = -0.101 * fr1 - 0.338 * fb1 + 0.439 * fb1 + 128;
			fcr0 = 0.439 * fr1 - 0.399 * fg1 - 0.040 * fb1 + 128;

			fy1 = 0.183 * fr2 + 0.614 * fg2 + 0.0622 * fb2 + 16;

#ifdef SENDER_APP_RGB_AVG  // if to use sample avg instead of even pixel samples only
			fcb1 = -0.101 * fr2 - 0.338 * fb2 + 0.439 * fb2 + 128;
			fcr1 = 0.439 * fr2 - 0.399 * fg2 - 0.040 * fb2 + 128;

			switch (convert)  // will inline
			{
			case ST_422_FMT_CONV_NET_LE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_LE10_BUF_BGRA:
				Pack_422le10_PG2le(dst + j, ((uint16_t)fcb0 + (uint16_t)fcb1) * 2,
								   (uint16_t)fy0 * 4, ((uint16_t)fcr0 + (uint16_t)fcr1) * 2,
								   (uint16_t)fy1 * 4);
				break;
			case ST_422_FMT_CONV_NET_BE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_BE10_BUF_BGRA:
				Pack_422le10_PG2be(dst + j, ((uint16_t)fcb0 + (uint16_t)fcb1) * 2,
								   (uint16_t)fy0 * 4, ((uint16_t)fcr0 + (uint16_t)fcr1) * 2,
								   (uint16_t)fy1 * 4);
				break;
			default:
				ST_APP_ASSERT;
				return ST_NOT_SUPPORTED;
			}
#else
			switch (convert)  // will inline
			{
			case ST_422_FMT_CONV_NET_LE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_LE10_BUF_BGRA:
				Pack_422le10_PG2le(dst + j, (uint16_t)fcb0 * 4, (uint16_t)fy0 * 4,
								   (uint16_t)fcr0 * 4, (uint16_t)fy1 * 4);
				break;
			case ST_422_FMT_CONV_NET_BE10_BUF_RGBA:
			case ST_422_FMT_CONV_NET_BE10_BUF_BGRA:
				Pack_422le10_PG2be(dst + j, (uint16_t)fcb0 * 4, (uint16_t)fy0 * 4,
								   (uint16_t)fcr0 * 4, (uint16_t)fy1 * 4);
				break;
			default:
				ST_APP_ASSERT;
				return ST_NOT_SUPPORTED;
			}
#endif
		}
		app->movie += sizeof(uint8_t) * 8 * 4;
#ifdef SENDER_APP_DEBUG
		if (app->movieEnd < app->movie)
		{
			ST_APP_ASSERT;
		}
#endif
	}
	if (app->movieEnd <= app->movie)
	{
		app->movie = app->movieBegin;
	}
	prod->frameCursor = (prod->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

static inline st_status_t
SendAppReadFrame422Inline(strtp_send_app_t *app,
						  void (*Pack)(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00,
									   uint16_t cr00, uint16_t y01))
{
	st21_producer_t *prod;
	SendAppLock(app);

	prod = (st21_producer_t *)app->prod;

	prod->frameBuf = app->frames[prod->frameCursor];
	SendAppUnlock(app);

	SendAppWaitFrameDone(app);

	st_rfc4175_422_10_pg2_t *dst = (st_rfc4175_422_10_pg2_t *)prod->frameBuf;
	st_rfc4175_422_10_pg2_t *const end
		= (st_rfc4175_422_10_pg2_t *)(prod->frameBuf + prod->frameSize);
	// read frame from movie and convert into frameBuf
	uint16_t const *Y = (uint16_t *)(app->movie + 0 * (app->movieBufSize / 4));
	uint16_t const *R = (uint16_t *)(app->movie + 2 * (app->movieBufSize / 4));
	uint16_t const *B = (uint16_t *)(app->movie + 3 * (app->movieBufSize / 4));

	/* TODO
      * Below code is just a demo, which is not efficient for conversion
      */
	for (; dst < end; ++dst)
	{
		Pack(dst, *R++, Y[0], *B++, Y[1]);
		Y += 2;
		app->movie += sizeof(uint16_t) * 4;
		assert(app->movie <= app->movieEnd);
	}

	if (app->movieEnd <= app->movie)
	{
		app->movie = app->movieBegin;
	}
	prod->frameCursor = (prod->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

st_status_t
SendAppReadFrameNetLeBufLe(strtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422le10_PG2le);
}

st_status_t
SendAppReadFrameNetLeBufBe(strtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422be10_PG2le);
}

st_status_t
SendAppReadFrameNetBeBufLe(strtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422le10_PG2be);
}

st_status_t
SendAppReadFrameNetBeBufBe(strtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422be10_PG2be);
}

st_status_t
SendAppReadFrameNetLeBufRgba(strtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_LE10_BUF_RGBA);
}

st_status_t
SendAppReadFrameNetLeBufBgra(strtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_LE10_BUF_BGRA);
}

st_status_t
SendAppReadFrameNetBeBufRgba(strtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_BE10_BUF_RGBA);
}

st_status_t
SendAppReadFrameNetBeBufBgra(strtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_BE10_BUF_BGRA);
}

#define WhichThread (0)
#define HowFrames (400)

#if WhichThread == 0

typedef struct
{
	int32_t lowMn;
	int32_t lowMx;
	int32_t highMn;
	int32_t highMx;
} app_cpulist_t;
static app_cpulist_t cpuList[4];
static volatile int howCpuScks = -1;
static volatile int affinityCore = -1;

static st_status_t
GetCPUs(int32_t soc, app_cpulist_t *cl)
{
	FILE *fp;
	char clb[64];
	cl->lowMn = -1;
	cl->lowMx = -1;
	cl->highMn = -1;
	cl->highMx = -1;
	snprintf(clb, sizeof(clb), "/sys/devices/system/node/node%d/cpulist", soc);
	fp = fopen(clb, "r");
	if (fp == NULL)
		return ST_DEV_CANNOT_READ_CPUS;
	if (fgets(clb, sizeof(clb), fp) == NULL)
	{
		return ST_GENERAL_ERR;
	}
	fclose(fp);
	if (sscanf(clb, "%d-%d,%d-%d", &cl->lowMn, &cl->lowMx, &cl->highMn, &cl->highMx) < 2)
		return ST_DEV_CANNOT_READ_CPUS;
	return ST_OK;
}

static void *__attribute__((noreturn)) SendAppThread(void *arg)
{
	strtp_send_app_t *app = (strtp_send_app_t *)arg;
	uint64_t currTmr;
	st21_producer_t *prod;
	double period = 1.0 / rte_get_tsc_hz();
	uint32_t elapsed = 0, old = app->frmsSend, act;

	SetAffinityCore(app, ST_DEV_TYPE_PRODUCER);
	prod = (st21_producer_t *)app->prod;
	prod->frmLocCnt = 0;
	prod->lastTmr = 0;

	for (;;)
	{
		act = app->frmsSend;
		while (old == act)
		{
			sleep(0);
			/* enqueue thread will update frmsSend */
			act = app->frmsSend;
		}
		elapsed += (act - old - 1);
		prod->frmLocCnt += act - old;
		old = act;
		app->SendAppReadFrame(app);

		if (prod->frmLocCnt >= HowFrames)
		{
			currTmr = rte_get_tsc_cycles();
			uint64_t cclks = currTmr - prod->lastTmr;
			double frameRate = (double)(prod->frmLocCnt) / (period * cclks);
			if (prod->lastTmr)
				RTE_LOG(INFO, USER2, "App[%02d], Frame Rate = %4.2lf Over elapsed: %d\n",
						app->index, frameRate, elapsed);
			prod->lastTmr = currTmr;
			elapsed = 0;
			prod->frmLocCnt = 0;
		}

		if (app->videoStream != NULL)
		{
			ShowFrame(app->videoStream, app->movie, 2);
		}
	}
}

#else

static void *__attribute__((noreturn)) SendAppThread(void *arg)
{
	strtp_send_app_t *app = (strtp_send_app_t *)arg;
	SetAffinityCore(app, ST_DEV_TYPE_PRODUCER);

	for (;;)
	{
		uint32_t const old = app->frmsSend;
		app->SendAppReadFrame(app);
		while (old == app->frmsSend)
		{
			sleep(0);  // wait for next frame
		}
	}
}

#endif

static void *__attribute__((noreturn)) SendAudioThread(void *arg)
{
	strtp_send_app_t *app = (strtp_send_app_t *)arg;
	SetAffinityCore(app, ST_DEV_TYPE_PRODUCER);

	for (;;)
	{
		uint32_t const old = app->frmsSend;
		app->SendAppReadFrame(app);
		while (old == app->frmsSend)
		{
			sleep(0);  // wait for next frame
		}
		//if (app->affinited == 0)
		//	SetAffinityCoreNb(app);
	}
}
st_status_t
SendAppOpenFile(strtp_send_app_t *app, const char *fileName)
{
	struct stat i;

	app->fileFd = open(fileName, O_RDONLY);
	if (app->fileFd >= 0)
	{
		fstat(app->fileFd, &i);

		uint8_t *m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, app->fileFd, 0);

		if (MAP_FAILED != m)
		{
			app->movieBegin = m;
			app->movie = m;
			app->movieEnd = m + i.st_size;
			if (app->mtype == ST_ESSENCE_ANC || app->mtype == ST_ESSENCE_AUDIO)
			{
				app->movieBufSize = i.st_size;	// ANC
			}
			else
			{
				app->singleFrameMode = (i.st_size == app->movieBufSize) ? 1 : 0;
			}
		}
		else
		{
			RTE_LOG(ERR, USER1, "mmap fail '%s'\n", fileName);
			return ST_GENERAL_ERR;
		}
	}  // ffmpeg -i ${file} -c:v rawvideo -pix_ftm yuv440p10be -o ${file}.yuv
	else
	{
		char options[ST_OPT_SIZE];
		switch (app->mtype)
		{
		case ST_ESSENCE_VIDEO:
			snprintf(options, ST_OPT_SIZE, "%s", "videoFile");
			break;
		case ST_ESSENCE_AUDIO:
			snprintf(options, ST_OPT_SIZE, "%s", "audioFile");
			break;
		case ST_ESSENCE_ANC:
			snprintf(options, ST_OPT_SIZE, "%s", "ancFile");
			break;
		default:
			break;
		}
		RTE_LOG(ERR, USER1, "Fail to find %s, please use option '--%s' to provide\n", fileName,
				options);
		return ST_GENERAL_ERR;
	}

	return ST_OK;
}

st_status_t
SendAppInitProd(strtp_send_app_t *app, void *producer)
{
	st_essence_type_t type;
	st_status_t status = ST_OK;

	type = app->mtype;
	app->prod = producer;

	switch (type)
	{
	case ST_ESSENCE_VIDEO:
		for (uint32_t i = 0; i < SEND_APP_FRAME_MAX; i++)
		{
			app->frames[i] = StAllocFrame(app->session, app->session->frameSize);
			if (!app->frames[i])
			{
				return ST_NO_MEMORY;
			}
		}
		status = SendSt21AppInit(app, producer);
		break;
	case ST_ESSENCE_AUDIO:
		for (uint32_t i = 0; i < SEND_APP_FRAME_MAX; i++)
		{
			app->frames[i] = StAllocFrame(app->session, app->session->frameSize);
			if (!app->frames[i])
			{
				return ST_NO_MEMORY;
			}
		}
		status = SendSt30AppInit(app, producer);
		break;
	case ST_ESSENCE_ANC:
		for (uint32_t i = 0; i < SEND_APP_FRAME_MAX; i++)
		{
			app->ancFrames[i].data = StAllocFrame(app->session, app->session->frameSize);
			if (!app->ancFrames[i].data)
			{
				return ST_NO_MEMORY;
			}
		}
		status = SendSt40AppInit(app, producer);
		break;
	default:
		status = ST_INVALID_PARAM;
		break;
	}
	return status;
}

st_status_t
SendSt21AppInit(strtp_send_app_t *app, void *producer)
{
	st_status_t status = ST_OK;
	st_session_t *session = app->session;
	st21_producer_t *prod = (st21_producer_t *)producer;

	st_format_t vfmt;
	StGetFormat(app->session, &vfmt);

	st21_format_t *fmt = &vfmt.v;

	app->singleFrameMode = 0;  // initially
	prod->dualPixelSize = (2 * fmt->pixelGrpSize) / fmt->pixelsInGrp;
	prod->sliceSize = 20 *	// at least 20 lines if single pixel group usually 40 lines
					  fmt->width * fmt->pixelGrpSize;
	prod->sliceCount = session->frameSize / prod->sliceSize;
	prod->pixelGrpsInSlice = prod->sliceSize / fmt->pixelGrpSize;
	prod->linesInSlice = 40;  // for now TBD

	prod->frameSize = session->frameSize;
	prod->appHandle = (void *)app;

	switch (app->bufFormat)
	{
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		switch (fmt->pixelFmt)
		{
		case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
			app->SendAppReadFrame = SendAppReadFrameNetBeBufBe;
			break;
		case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
			app->SendAppReadFrame = SendAppReadFrameNetLeBufBe;
			break;
		default:
			ST_APP_ASSERT;
			return ST_NOT_SUPPORTED;
		}
		app->movieBufSize = 4 * fmt->width * fmt->height;
		break;

	case ST21_BUF_FMT_RGBA_8BIT:
		switch (fmt->pixelFmt)
		{
		case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
			app->SendAppReadFrame = SendAppReadFrameNetBeBufRgba;
			break;
		case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
			app->SendAppReadFrame = SendAppReadFrameNetLeBufRgba;
			break;
		default:
			ST_APP_ASSERT;
			return ST_NOT_SUPPORTED;
		}
		app->movieBufSize = 4 * fmt->width * fmt->height;
		break;

	default:
		ST_APP_ASSERT;
		return ST_NOT_SUPPORTED;
	}

	// for now there is only BE input buffer format supported

	switch (fmt->clockRate)
	{
	case 90000:	 // 90kHz
		app->tmstampTime = 11111;
		break;
	case 48000:	 // 48kHz
		app->tmstampTime = 20833;
		break;
	}

#ifdef SENDER_APP_LOGS
	RTE_LOG(INFO, USER2, "SendApp: dualPixelSize %u sliceSize %u sliceCount %u\n",
			app->dualPixelSize, app->sliceSize, app->sliceCount);
	RTE_LOG(INFO, USER2, "SendApp: pixelGrpsInSlice %u linesInSlice %u frameSize %u\n",
			app->pixelGrpsInSlice, app->linesInSlice, prod->frameSize);
#endif
	// initially read 1st frame
	prod->frameCursor = 0;
	prod->frameBuf = app->frames[prod->frameCursor];
	prod->frmLocCnt = 0;
	app->frmsSend = 0;

	status = app->SendAppReadFrame(app);
	if (status != ST_OK)
		return status;

	if (!app->singleFrameMode)
	{
		pthread_create(&app->movieThread, NULL, SendAppThread, (void *)app);
		if (howCpuScks == -1)
			for (howCpuScks = 0;
				 (howCpuScks < 3) && (GetCPUs(howCpuScks, &cpuList[howCpuScks]) >= 0); howCpuScks++)
				;
	}
	return ST_OK;
}

static inline st_status_t
SendAppReadFrameAnc(strtp_send_app_t *app)
{
	st40_producer_t *prod;
	SendAppLock(app);
	prod = (st40_producer_t *)app->prod;
	prod->frameBuf = &(app->ancFrames[prod->frameCursor]);
	app->isEndOfAncDataBuf = false;
	SendAppUnlock(app);
	SendAppWaitFrameDone(app);
	uint16_t udwSize
		= app->movieBufSize > ST_ANC_UDW_MAX_SIZE ? ST_ANC_UDW_MAX_SIZE : app->movieBufSize;
	prod->frameBuf->meta[0].c = 0;
	prod->frameBuf->meta[0].lineNumber = 10;
	prod->frameBuf->meta[0].horiOffset = 0;
	prod->frameBuf->meta[0].s = 0;
	prod->frameBuf->meta[0].streamNum = 0;
	prod->frameBuf->meta[0].did = 0x43;
	prod->frameBuf->meta[0].sdid = 0x02;
	prod->frameBuf->meta[0].udwSize = udwSize;
	prod->frameBuf->meta[0].udwOffset = 0;
	rte_memcpy(prod->frameBuf->data, app->movie, udwSize);
	prod->frameBuf->dataSize = udwSize;
	prod->frameBuf->metaSize = 1;
	prod->frameCursor = (prod->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

static void *__attribute__((noreturn)) SendAncThread(void *arg)
{
	strtp_send_app_t *app = (strtp_send_app_t *)arg;
	SetAffinityCore(app, ST_DEV_TYPE_PRODUCER);

	for (;;)
	{
		uint32_t const old = app->frmsSend;
		app->SendAppReadFrame(app);
		while (old == app->frmsSend)
		{
			sleep(0);  // wait for next frame
		}
	}
}

st_status_t
SendSt40AppInit(strtp_send_app_t *app, void *producer)
{
	st_status_t status = ST_OK;
	st_session_t *session = app->session;
	st40_producer_t *prod = (st40_producer_t *)producer;

	st_format_t ancfmt;
	StGetFormat(app->session, &ancfmt);

	st40_format_t *fmt = &ancfmt.anc;

	app->singleFrameMode = 1;  // initially

	prod->bufSize = session->frameSize;
	prod->appHandle = (void *)app;

	app->SendAppReadFrame = SendAppReadFrameAnc;

	// for now there is only BE input buffer format supported

	switch (fmt->clockRate)
	{
	case 90000:	 // 90kHz
		app->tmstampTime = 11111;
		break;
	case 48000:	 // 48kHz
		app->tmstampTime = 20833;
		break;
	}

	prod->frameCursor = 0;
	prod->bufOffset = 0;
	app->frmsSend = 0;

	status = app->SendAppReadFrame(app);
	if (status != ST_OK)
		return status;

	if (!app->singleFrameMode)
	{
		pthread_create(&app->movieThread, NULL, SendAncThread, (void *)app);
		if (howCpuScks == -1)
			for (howCpuScks = 0;
				 (howCpuScks < 3) && (GetCPUs(howCpuScks, &cpuList[howCpuScks]) >= 0); howCpuScks++)
				;
	}
	return ST_OK;
}

void *
SendAppNewSt21Producer(st_session_t *sn)
{
	st21_producer_t *prod;

	prod = malloc(sizeof(st21_producer_t));
	if (!prod)
	{
		return NULL;
	}

	prod->St21GetNextFrameBuf = SendAppGetNextFrameBuf;
	prod->prodType = ST21_PROD_P_FRAME;

	prod->St21GetNextSliceOffset = SendAppGetNextSliceOffset;
	prod->St21NotifyFrameDone = SendAppNotifyFrameDone;
	prod->St21NotifyStopDone = SendAppNotifyStopDone;
	prod->St21GetFrameTmstamp = SendAppGetFrameTmstamp;

	return prod;
}
uint32_t
SendAppGetNextAudioOffset(void *appHandle, uint8_t *frameBuf, uint32_t prevOffset,
						  uint32_t *tmstamp)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st30_producer_t *prod = (st30_producer_t *)app->prod;

	if (frameBuf == NULL)
		return prevOffset;	// invalid input

	if (prod->bufOffset > prevOffset)
		return prod->bufOffset;

	return prevOffset;
}

void *
SendAppNewSt30Producer(st_session_t *sn)
{
	st30_producer_t *prod;
	prod = malloc(sizeof(st30_producer_t));
	if (!prod)
	{
		return NULL;
	}

	prod->St30GetNextAudioBuf = SendAppGetNextAudioBuf;
	prod->St30GetNextSampleOffset = SendAppGetNextAudioOffset;
	prod->bufOffset = 0;
	prod->prodType = ST30_PROD_RAW_RTP;

	prod->St30NotifyBufferDone = SendAppNotifyBufDone;
	prod->St30NotifyStopDone = SendAppNotifyStopDone;

	return prod;
}

/**
 * Callback to producer application to get next ancillary buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and TBD
 */
void *
SendAppGetNextAncFrame(void *appHandle)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st40_producer_t *prod;
	uint8_t *nextBuf;

	SendAppLock(app);

	prod = (st40_producer_t *)app->prod;
	nextBuf = (void *)prod->frameBuf;

	if (app->iscldThrSet == 0)
	{
		app->cldThr = pthread_self();
		app->iscldThrSet = 1;
	}
	app->frmsSend++;
	SendAppUnlock(app);
	return nextBuf;
}

/**
 * Callback to producer application with notification about the buffer completion
 * Ancillary frame can be released or reused after it but not sooner
 */
void
SendAppNotifyAncFrameDone(void *appHandle, void *frameBuf)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	int i;
	SendAppLock(app);
	for (i = 0; i < SEND_APP_FRAME_MAX; ++i)
	{
		if (&(app->ancFrames[i]) == frameBuf)
			break;
	}

	assert(i < SEND_APP_FRAME_MAX);

	__sync_lock_release(&app->frameDone[i], 0);
	SendAppUnlock(app);
	return;
}

void *
SendAppNewSt40Producer(st_session_t *sn)
{
	st40_producer_t *prod;

	prod = malloc(sizeof(st40_producer_t));
	if (!prod)
	{
		return NULL;
	}
	prod->prodType = ST40_PROD_REGULAR;

	prod->St40GetNextAncFrame = SendAppGetNextAncFrame;
	prod->St40NotifyFrameDone = SendAppNotifyAncFrameDone;

	return prod;
}

static inline st_status_t
SendAppReadFrameAudio(strtp_send_app_t *app)
{
	st30_producer_t *prod;
	SendAppLock(app);

	prod = (st30_producer_t *)app->prod;

	prod->frameBuf = app->frames[prod->frameCursor];
	SendAppUnlock(app);

	SendAppWaitFrameDone(app);

	uint8_t *dst = (uint8_t *)prod->frameBuf;
	uint8_t *const end = (uint8_t *)(prod->frameBuf + prod->bufSize);

	// read frame from movie and convert into frameBuf
	for (; dst < end; ++dst)
	{
		*dst = *(uint8_t *)app->movie;
		app->movie += sizeof(uint8_t);
		prod->bufOffset += sizeof(uint8_t);
		if (app->movie >= app->movieEnd)
		{
			app->movie = app->movieBegin;
		}
	}
	prod->frameCursor = (prod->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

st_status_t
SendSt30AppInit(strtp_send_app_t *app, void *producer)
{
	st_status_t status = ST_OK;
	st_session_t *session = app->session;
	st30_producer_t *prod = (st30_producer_t *)producer;

	app->singleFrameMode = 1;  // initially

	prod->bufSize = session->frameSize;
	prod->appHandle = (void *)app;

	app->SendAppReadFrame = SendAppReadFrameAudio;
	// initially read 1st frame
	prod->frameCursor = 0;
	prod->frameBuf = app->frames[prod->frameCursor];
	app->frmsSend = 0;

	status = app->SendAppReadFrame(app);
	if (status != ST_OK)
		return status;

	if (!app->singleFrameMode)
	{
		pthread_create(&app->movieThread, NULL, SendAudioThread, (void *)app);
		if (howCpuScks == -1)
			for (howCpuScks = 0;
				 (howCpuScks < 3) && (GetCPUs(howCpuScks, &cpuList[howCpuScks]) >= 0); howCpuScks++)
				;
	}

	return ST_OK;
}

st_status_t
SendAppNewProducer(st_session_t *sn, void **prodOut)
{
	st_essence_type_t mtype;
	mtype = sn->type;
	void *prod = NULL;

	switch (mtype)
	{
	case ST_ESSENCE_VIDEO:
		prod = SendAppNewSt21Producer(sn);
		break;

	case ST_ESSENCE_AUDIO:
		prod = SendAppNewSt30Producer(sn);
		break;

	case ST_ESSENCE_ANC:
		prod = SendAppNewSt40Producer(sn);
		break;
	default:
		break;
	}
	if (!prod)
		return ST_NO_MEMORY;

	*prodOut = prod;

	return ST_OK;
}

st_status_t
SendAppCreateProducer(st_session_t *sn, uint8_t bufFormat, const char *fileName,
					  strtp_send_app_t **appOut)
{
	st_status_t status = ST_OK;
	void *producer;

	if (!sn)
		return ST_INVALID_PARAM;

	strtp_send_app_t *app = malloc(sizeof(strtp_send_app_t));
	if (!app)
	{
		return ST_NO_MEMORY;
	}
	memset(app, 0x0, sizeof(strtp_send_app_t));
	app->mtype = sn->type;
	app->session = sn;

	app->bufFormat = bufFormat;

	status = SendAppNewProducer(sn, &producer);
	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "SendAppNewProducer error of %d\n", status);
		free(app);
		return status;
	}

	status = SendAppOpenFile(app, fileName);
	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "SendAppOpenFile error of %d\n", status);
		free(app);
		return status;
	}

	if (app->mtype == ST_ESSENCE_ANC || app->mtype == ST_ESSENCE_AUDIO)
	{
		sn->frameSize = app->movieBufSize;
	}

	status = SendAppInitProd(app, producer);
	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "SendAppInitProd error of %d\n", status);
		free(app);
		return status;
	}

	status = StRegisterProducer(sn, producer);	//&app->prod);
	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "StRegisterProducer FAILED. ErrNo: %d\n", status);
		free(app);
		return status;
	}

	*appOut = app;

	return status;
}

uint8_t *
SendAppGetFrameBuf(strtp_send_app_t *app)
{
	if (app->mtype == ST_ESSENCE_VIDEO)
	{
		st21_producer_t *prod = app->prod;
		return prod->frameBuf;
	}
	else if (app->mtype == ST_ESSENCE_AUDIO)
	{
		st30_producer_t *prod = app->prod;
		return prod->frameBuf;
	}
	else
	{
		st40_producer_t *prod = app->prod;
		return (uint8_t *)prod->frameBuf;
	}
}

st_status_t
SendAppStart(st_session_t *sn, strtp_send_app_t *app)
{
	st_status_t status = ST_OK;

	if (!sn || !app)
		return ST_INVALID_PARAM;

	switch (sn->type)
	{
	case ST_ESSENCE_VIDEO:
		status = St21ProducerStartFrame(sn, SendAppGetFrameBuf(app), sn->frameSize, 0, 0);
		break;

	case ST_ESSENCE_AUDIO:
		status = St30ProducerStartFrame(sn, SendAppGetFrameBuf(app), sn->frameSize, 0, 0);
		break;

	case ST_ESSENCE_ANC:
		status = St40ProducerStartFrame(sn, SendAppGetFrameBuf(app), sn->frameSize, 0, 0);
		break;
	default:
		status = ST_INVALID_PARAM;
		break;
	}
	return status;
}

uint32_t
SendAppReadNextSlice(strtp_send_app_t *app, uint8_t *frameBuf, uint32_t prevOffset,
					 uint32_t sliceSize, uint32_t fieldId)
{
	st21_producer_t *prod = (st21_producer_t *)app->prod;

	prod->sliceOffset += sliceSize;
	return prod->sliceOffset;
}

/**
 * Callback to producer or consumer application to get next frame buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming
 */
uint8_t *
SendAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize, uint32_t fieldId)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st21_producer_t *prod;
	uint8_t *nextBuf;

	SendAppLock(app);

	prod = (st21_producer_t *)app->prod;
	nextBuf = prod->frameBuf;

	if (app->iscldThrSet == 0)
	{
		app->cldThr = pthread_self();
		app->iscldThrSet = 1;
	}

	SendAppUnlock(app);

	if (nextBuf)
	{
		if (app->singleFrameMode)
		{
			app->frmsSend++;
			prod->sliceOffset = 0;
			SendAppReadNextSlice(app, nextBuf, 0, prod->sliceSize, fieldId);
			return nextBuf;
		}
		else if (nextBuf != prevFrameBuf)
		{
			app->frmsSend++;
			prod->sliceOffset = 0;
			SendAppReadNextSlice(app, nextBuf, 0, prod->sliceSize, fieldId);
			return nextBuf;
		}
	}
	else
	{
		return NULL;
	}
	return NULL;
}

uint8_t *
SendAppGetNextAudioBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st30_producer_t *prod;
	uint8_t *nextBuf;

	SendAppLock(app);

	prod = (st30_producer_t *)app->prod;
	nextBuf = prod->frameBuf;

	if (app->iscldThrSet == 0)
	{
		app->cldThr = pthread_self();
		app->iscldThrSet = 1;
	}

	SendAppUnlock(app);

	if (nextBuf)
	{
		if (app->singleFrameMode)
		{
			app->frmsSend++;
			return nextBuf;
		}
		else if (nextBuf != prevFrameBuf)
		{
			app->frmsSend++;
			return nextBuf;
		}
	}

	return NULL;
}

/**
 * Callback to producer or consumer application to get next slice buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate to restart streaming
 */
uint32_t
SendAppGetNextSliceOffset(void *appHandle, uint8_t *frameBuf, uint32_t prevOffset, uint32_t fieldId)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st21_producer_t *prod = (st21_producer_t *)app->prod;

	if (frameBuf == NULL)
		return prevOffset;	// invalid input

	if (prod->sliceOffset > prevOffset)
		return prod->sliceOffset;

	if ((prevOffset + prod->sliceSize) > prod->frameSize)
	{
		return prevOffset;	// above end of frame
	}
	return SendAppReadNextSlice(app, frameBuf, prevOffset, prod->sliceSize, fieldId);
}

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
void
SendAppNotifyFrameDone(void *appHandle, uint8_t *frameBuf, uint32_t fieldId)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	int i;

	for (i = 0; i < SEND_APP_FRAME_MAX; ++i)
	{
		if (app->frames[i] == frameBuf)
			break;
	}

	assert(i < SEND_APP_FRAME_MAX);

	__sync_lock_release(&app->frameDone[i], 0);

	return;
}

void
SendAppNotifyBufDone(void *appHandle, uint8_t *frameBuf)
{
	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	int i;

	for (i = 0; i < SEND_APP_FRAME_MAX; ++i)
	{
		if (app->frames[i] == frameBuf)
			break;
	}

	assert(i < SEND_APP_FRAME_MAX);

	__sync_lock_release(&app->frameDone[i], 0);

	return;
}

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void
SendAppNotifyStopDone(void *appHandle)
{
	//strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	return;
}

/**
 * Callback to producer application to get timestamp as transported in SDI frame
 */
uint32_t
SendAppGetFrameTmstamp(void *appHandle)
{
#define NETWORK_TIME 30000ul
#define NIC_TX_TIME 20000ul

	strtp_send_app_t *app = (strtp_send_app_t *)appHandle;
	st_session_t *s = app->session;
	st_format_t getFmt;

	StGetFormat(s, &getFmt);

	uint32_t repeats = 0;
	U64 ntime = StPtpGetTime();
	struct timespec spec, specLast;
	spec.tv_nsec = ntime % GIGA;
	spec.tv_sec = ntime / GIGA;
	U64 toEpoch = 0;
	U64 epochs = 0;

	if (s->type == ST_ESSENCE_VIDEO)
	{
		epochs = (U64)(ntime / getFmt.v.frameTime);
		toEpoch = (U64)((epochs + 1) * getFmt.v.frameTime - ntime);
	}
	else if (s->type == ST_ESSENCE_ANC)
	{
		epochs = (U64)(ntime / getFmt.anc.frameTime);
		toEpoch = (U64)((epochs + 1) * getFmt.anc.frameTime - ntime);
	}

	U64 toElapse = toEpoch + s->trOffset - NETWORK_TIME;
	U64 elapsed;

	struct timespec req, rem;

	req.tv_sec = 0;
	req.tv_nsec = toElapse / 256;

	for (;;)
	{
		clock_nanosleep(CLOCK_REALTIME, 0, &req, &rem);
		U64 t = StPtpGetTime();
		specLast.tv_nsec = t % GIGA;
		specLast.tv_sec = t / GIGA;
		elapsed = (specLast.tv_nsec > spec.tv_nsec) ? specLast.tv_nsec - spec.tv_nsec
													: specLast.tv_nsec + GIGA - spec.tv_nsec;
		if (elapsed + MAX(req.tv_nsec, app->tmstampTime) > toElapse)
			break;
		repeats++;
	}

	uint64_t tmstampSec = (uint64_t)specLast.tv_sec * GIGA;
	uint32_t tmstamp = ((uint64_t)specLast.tv_nsec + tmstampSec + NIC_TX_TIME) / app->tmstampTime;

#ifdef SENDER_APP_LOGS
	RTE_LOG(INFO, USER2,
			"SendAppGetFrameTmstamp: session %u, repeats %llu elapsed %llu diff %llu \n",
			s->timeslot, repeats, elapsed,
			(elapsed > toElapse) ? elapsed - toElapse : toElapse - elapsed);

	RTE_LOG(INFO, USER2, "SendAppGetFrameTmstamp: session %u, waiting %llu troffset %u delta %u\n",
			s->timeslot, toElapse, s->sn.trOffset, tmstamp - app->firstTmstamp);

	app->firstTmstamp = tmstamp;
#endif

	return tmstamp;
}
