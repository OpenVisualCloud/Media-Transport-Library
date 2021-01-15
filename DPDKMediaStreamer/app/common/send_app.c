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

static inline void
SendAppLock(rvrtp_send_app_t *app)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&app->lock, 1);
	} while (lock != 0);
}

static inline void
SendAppUnlock(rvrtp_send_app_t *app)
{
	__sync_lock_release(&app->lock, 0);
}

static inline void
SendAppWaitFrameDone(rvrtp_send_app_t *app)
{
       uint8_t done;
       do {
               done = __sync_lock_test_and_set(&app->frameDone[app->frameCursor], 1);
       } while (done != 0);
}

static inline st_status_t
SendAppReadFrameRgbaInline(rvrtp_send_app_t *app, st_vid_fmt_conv_t convert)
{
	SendAppLock(app);
	app->frameBuf = app->frames[app->frameCursor];
	SendAppUnlock(app);

	SendAppWaitFrameDone(app);

	st_rfc4175_422_10_pg2_t *dst = (st_rfc4175_422_10_pg2_t *)app->frameBuf;
	st_rfc4175_422_10_pg2_t *const end
		= (st_rfc4175_422_10_pg2_t *)(app->frameBuf + app->frameSize);

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

#ifdef SENDER_APP_RGB_AVG // if to use sample avg instead of even pixel samples only
			fcb1 = -0.101 * fr2 - 0.338 * fb2 + 0.439 * fb2 + 128;
			fcr1 = 0.439 * fr2 - 0.399 * fg2 - 0.040 * fb2 + 128;

			switch (convert) // will inline
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
			switch (convert) // will inline
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
	app->frameCursor = (app->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

static inline st_status_t
SendAppReadFrame422Inline(rvrtp_send_app_t *app,
						  void (*Pack)(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00,
									   uint16_t cr00, uint16_t y01))
{
	SendAppLock(app);
	app->frameBuf = app->frames[app->frameCursor];
	SendAppUnlock(app);

	SendAppWaitFrameDone(app);

	st_rfc4175_422_10_pg2_t *dst = (st_rfc4175_422_10_pg2_t *)app->frameBuf;
	st_rfc4175_422_10_pg2_t *const end
		= (st_rfc4175_422_10_pg2_t *)(app->frameBuf + app->frameSize);
	// read frame from movie and convert into frameBuf
	uint16_t const *Y = (uint16_t *)(app->movie + 0 * (app->movieBufSize / 4));
	uint16_t const *R = (uint16_t *)(app->movie + 2 * (app->movieBufSize / 4));
	uint16_t const *B = (uint16_t *)(app->movie + 3 * (app->movieBufSize / 4));
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
	app->frameCursor = (app->frameCursor + 1) % SEND_APP_FRAME_MAX;
	return ST_OK;
}

st_status_t
SendAppReadFrameNetLeBufLe(rvrtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422le10_PG2le);
}

st_status_t
SendAppReadFrameNetLeBufBe(rvrtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422be10_PG2le);
}

st_status_t
SendAppReadFrameNetBeBufLe(rvrtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422le10_PG2be);
}

st_status_t
SendAppReadFrameNetBeBufBe(rvrtp_send_app_t *app)
{
	return SendAppReadFrame422Inline(app, Pack_422be10_PG2be);
}

st_status_t
SendAppReadFrameNetLeBufRgba(rvrtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_LE10_BUF_RGBA);
}

st_status_t
SendAppReadFrameNetLeBufBgra(rvrtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_LE10_BUF_BGRA);
}

st_status_t
SendAppReadFrameNetBeBufRgba(rvrtp_send_app_t *app)
{
	return SendAppReadFrameRgbaInline(app, ST_422_FMT_CONV_NET_BE10_BUF_RGBA);
}

st_status_t
SendAppReadFrameNetBeBufBgra(rvrtp_send_app_t *app)
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

static int
SetAffinityCoreNb(rvrtp_send_app_t *app)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	if (app->iscldThrSet && affinityCore == -1)
	{
		if (pthread_getaffinity_np(app->cldThr, sizeof(cpu_set_t), &cpuset) < 0)
			return -1;
		int coreNb = get_nprocs_conf() - 1;
		for (; (coreNb >= 0) && (!CPU_ISSET(coreNb, &cpuset)); coreNb--)
			;
		// find which socket
		int soc = howCpuScks - 1;
		for (; soc >= 0; soc--)
		{
			if (coreNb >= cpuList[soc].lowMn && coreNb <= cpuList[soc].lowMx)
				break;
			if (coreNb >= cpuList[soc].highMn && coreNb <= cpuList[soc].highMx)
				break;
		}
		if (soc < 0)
			return -1;
		if (howCpuScks > 1)
		{
			if (soc == (howCpuScks - 1))
			{
				soc = 0;
			}
			else
			{
				++soc;
			}
			affinityCore = cpuList[soc].highMx;
		}
		else
		{
			if (cpuList[soc].highMn != -1)
			{
				if (coreNb >= cpuList[soc].highMn && coreNb <= cpuList[soc].highMx)
					affinityCore = cpuList[soc].lowMx;
				else
					affinityCore = cpuList[soc].highMx;
			}
			else
			{
				affinityCore = 1;
			}
		}
	}
	if (affinityCore >= 0 && app->affinited == 0)
	{

		affinityCore--;
		CPU_ZERO(&cpuset);
		if (affinityCore < 0)
			affinityCore = 0;
		CPU_SET(affinityCore + 1, &cpuset);
		if (pthread_setaffinity_np(app->movieThread, sizeof(cpu_set_t), &cpuset) == 0)
		{
			app->affinited = 1;
			printf("******  anifity OK %d\n", affinityCore + 1);
		}
	}
	return -1;
}

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
	rvrtp_send_app_t *app = (rvrtp_send_app_t *)arg;
	app->lastTmr = rte_get_tsc_cycles();
	double period = 1.0 / rte_get_tsc_hz();
	uint32_t droped = 0, old = app->frmsSend, act;
	uint64_t currTmr, firstFrameTime = StPtpGetTime();
	for (;;)
	{
		//		uint8_t *movie_old = app->movie;
		act = app->frmsSend;
		while (old == act)
		{
			sleep(0); // wait for next frame
			act = app->frmsSend;
		}
		droped += (act - old - 1);
		app->frmLocCnt += act - old;
		old = act;
		currTmr = rte_get_tsc_cycles();
		app->SendAppReadFrame(app);

		if (app->frmLocCnt >= HowFrames)
		{
			uint64_t cclks = currTmr - app->lastTmr;
			app->lastTmr = currTmr;
			double frameRate = (double)(app->frmLocCnt) / (period * cclks);
			RTE_LOG(INFO, USER2, "FrameRate = %4.2lf droped: %d\n", frameRate, droped);
			if (droped && app->affinited == 0)
				SetAffinityCoreNb(app);
			droped = 0;
			app->frmLocCnt = 0;
		}
		if (app->movie == app->movieBegin)
		{
			uint64_t curTime = StPtpGetTime();
			uint64_t del = curTime - firstFrameTime;
			firstFrameTime = curTime;
			del /= 1000000000;
			RTE_LOG(INFO, USER2, "Movie lenght: %ld\n", del);
		}

#if 1
		if (app->view != NULL)
		{
			ShowFrame(app->view, app->movie, 2);
		}
#endif
	}
}

#else

static void *__attribute__((noreturn)) SendAppThread(void *arg)
{
	rvrtp_send_app_t *app = (rvrtp_send_app_t *)arg;
	for (;;)
	{
		uint32_t const old = app->frmsSend;
		app->SendAppReadFrame(app);
		while (old == app->frmsSend)
		{
			sleep(0); // wait for next frame
		}
	}
}

#endif

st_status_t
SendAppInit(rvrtp_send_app_t *app, const char *fileName)
{
	st_status_t status = ST_OK;
	st21_format_t fmt;
	St21GetFormat(app->session, &fmt);

	for (uint32_t i = 0; i < SEND_APP_FRAME_MAX; i++)
	{
		app->frames[i] = St21AllocFrame(app->session, app->session->frameSize);
		if (!app->frames[i])
		{
			return ST_NO_MEMORY;
		}
	}
	app->singleFrameMode = 0; // initially
	app->dualPixelSize = (2 * fmt.pixelGrpSize) / fmt.pixelsInGrp;
	app->sliceSize = 20 * // at least 20 lines if single pixel group usually 40 lines
					 fmt.width * fmt.pixelGrpSize;
	app->sliceCount = app->session->frameSize / app->sliceSize;
	app->pixelGrpsInSlice = app->sliceSize / fmt.pixelGrpSize;
	app->linesInSlice = 40; // for now TBD

	app->frameSize = app->session->frameSize;
	switch (app->bufFormat)
	{
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		switch (fmt.pixelFmt)
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
		app->movieBufSize = 4 * fmt.width * fmt.height;
		break;

	case ST21_BUF_FMT_RGBA_8BIT:
		switch (fmt.pixelFmt)
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
		app->movieBufSize = 4 * fmt.width * fmt.height;
		break;

	default:
		ST_APP_ASSERT;
		return ST_NOT_SUPPORTED;
	}

	// for now there is only BE input buffer format supported

	switch (fmt.clockRate)
	{
	case 90000: // 90kHz
		app->tmstampTime = 11111;
		break;
	case 48000: // 48kHz
		app->tmstampTime = 20833;
		break;
	}

#ifdef SENDER_APP_LOGS
	RTE_LOG(INFO, USER2, "SendApp: dualPixelSize %u sliceSize %u sliceCount %u\n",
			app->dualPixelSize, app->sliceSize, app->sliceCount);
	RTE_LOG(INFO, USER2, "SendApp: pixelGrpsInSlice %u linesInSlice %u frameSize %u\n",
			app->pixelGrpsInSlice, app->linesInSlice, app->frameSize);
#endif
	app->fileFd = open(fileName, O_RDONLY);
	if (app->fileFd >= 0)
	{
		struct stat i;
		fstat(app->fileFd, &i);

		uint8_t *m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, app->fileFd, 0);

		if (MAP_FAILED != m)
		{
			app->movieBegin = m;
			app->movie = m;
			app->movieEnd = m + i.st_size;
			app->singleFrameMode = (i.st_size == app->movieBufSize) ? 1 : 0;
		}
		else
		{
			RTE_LOG(ERR, USER1, "mmap fail '%s'\n", fileName);
			return ST_GENERAL_ERR;
		}
	} // ffmpeg -i ${file} -c:v rawvideo -pix_ftm yuv440p10be -o ${file}.yuv
	else
	{
		RTE_LOG(ERR, USER1, "open fail '%s'\n", fileName);
		return ST_GENERAL_ERR;
	}
	// initially read 1st frame
	app->frameCursor = 0;
	app->frmsSend = 0;
	app->frameBuf = app->frames[app->frameCursor];
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

st_status_t
SendAppCreateProducer(st21_session_t *sn, st21_buf_fmt_t bufFormat, uint32_t fmtIndex,
					  rvrtp_send_app_t **appOut)
{
	st_status_t status = ST_OK;

	if (!sn)
		return ST_INVALID_PARAM;

	rvrtp_send_app_t *app = malloc(sizeof(rvrtp_send_app_t));
	if (!app)
	{
		return ST_NO_MEMORY;
	}
	memset(app, 0x0, sizeof(rvrtp_send_app_t));
	app->session = sn;
	app->bufFormat = bufFormat;
	if (app->bufFormat == ST21_BUF_FMT_RGBA_8BIT)
	{
		if (fmtIndex == 0 || fmtIndex == 3)
		{
			status = SendAppInit(app, "720.signal_8b.rgba");
		}
		else if (fmtIndex == 1 || fmtIndex == 4)
		{
			status = SendAppInit(app, "signal_8b.rgba");
		}
		else if (fmtIndex == 2 || fmtIndex == 5)
		{
			status = SendAppInit(app, "2160.signal_8b.rgba");
		}
	}
	else
	{
		if (fmtIndex == 0 || fmtIndex == 3)
		{
			status = SendAppInit(app, "720.signal_be.yuv");
		}
		else if (fmtIndex == 1 || fmtIndex == 4)
		{
			status = SendAppInit(app, "signal_be.yuv");
		}
		else if (fmtIndex == 2 || fmtIndex == 5)
		{
			status = SendAppInit(app, "2160.signal_be.yuv");
		}
	}

	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "SendAppInit error of %d\n", status);
		free(app);
		return status;
	}

	app->prod.St21GetNextFrameBuf = SendAppGetNextFrameBuf;
	app->prod.appHandle = app;
	app->prod.frameSize = sn->frameSize;
	app->prod.sliceCount = app->sliceCount;
	app->prod.sliceSize = app->sliceSize;
	app->prod.prodType = ST21_PROD_P_FRAME;

	app->prod.St21GetNextSliceOffset = SendAppGetNextSliceOffset;
	app->prod.St21NotifyFrameDone = SendAppNotifyFrameDone;
	app->prod.St21NotifyStopDone = SendAppNotifyStopDone;
	app->prod.St21GetFrameTmstamp = SendAppGetFrameTmstamp;

	status = St21RegisterProducer(sn, &app->prod);
	if (status != ST_OK)
	{
		RTE_LOG(INFO, USER2, "St21RegisterProducer FAILED. ErrNo: %d\n", status);
		free(app);
		return status;
	}

	*appOut = app;

	return status;
}

st_status_t
SendAppStart(st21_session_t *sn, rvrtp_send_app_t *app)
{
	st_status_t status = ST_OK;

	if (!sn || !app)
		return ST_INVALID_PARAM;

	status = St21ProducerStartFrame(sn, app->frameBuf, app->prod.frameSize, 0, 0);

	return status;
}

uint32_t
SendAppReadNextSlice(rvrtp_send_app_t *app, uint8_t *frameBuf, uint32_t prevOffset,
					 uint32_t sliceSize, uint32_t fieldId)
{
	app->sliceOffset += sliceSize;
	return app->sliceOffset;
}

/**
 * Callback to producer or consumer application to get next frame buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming
 */
uint8_t *
SendAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize, uint32_t fieldId)
{
	rvrtp_send_app_t *app = (rvrtp_send_app_t *)appHandle;
	uint8_t *nextBuf;

	SendAppLock(app);

	nextBuf = app->frameBuf;
	app->frmsSend++;

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
			app->sliceOffset = 0;
			SendAppReadNextSlice(app, nextBuf, 0, app->sliceSize, fieldId);
			return nextBuf;
		}
		else if (nextBuf != prevFrameBuf)
		{
			app->sliceOffset = 0;
			SendAppReadNextSlice(app, nextBuf, 0, app->sliceSize, fieldId);
			return nextBuf;
		}
	}
	else
	{
		return NULL;
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
	rvrtp_send_app_t *app = (rvrtp_send_app_t *)appHandle;

	if (frameBuf == NULL)
		return prevOffset; // invalid input

	if (app->sliceOffset > prevOffset)
		return app->sliceOffset;

	if ((prevOffset + app->sliceSize) > app->frameSize)
	{
		return prevOffset; // above end of frame
	}
	return SendAppReadNextSlice(app, frameBuf, prevOffset, app->sliceSize, fieldId);
}

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
void
SendAppNotifyFrameDone(void *appHandle, uint8_t *frameBuf, uint32_t fieldId)
{
	rvrtp_send_app_t *app = (rvrtp_send_app_t *)appHandle;
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
	//rvrtp_send_app_t *app = (rvrtp_send_app_t *)appHandle;
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

	rvrtp_send_app_t *app = (rvrtp_send_app_t *)appHandle;
	st21_session_t *s = app->session;
	st21_format_t fmt;

	St21GetFormat(s, &fmt);

	uint32_t repeats = 0;
	U64 ntime = StPtpGetTime();
	struct timespec spec, specLast;
	spec.tv_nsec = ntime % GIGA;
	spec.tv_sec = ntime / GIGA;
	U64 toEpoch;
	U64 epochs = ntime / fmt.frameTime;
	toEpoch = (epochs + 1) * fmt.frameTime - ntime;
	U64 toElapse = toEpoch + s->trOffset - NETWORK_TIME;
	U64 elapsed;

	// U64 st21Tmstamp90k = ((epochs + 1) * (tmIncrement1 + tmIncrement2)) / 2 + tfOffsetInTicks;

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
