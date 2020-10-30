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

#include "common_app.h"
#include "rx_view.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static rte_atomic32_t isAppStopped;

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
RecvAppLock(rvrtp_recv_app_t *app)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&app->lock, 1);
	} while (lock != 0);
}

static inline void
RecvAppUnlock(rvrtp_recv_app_t *app)
{
	__sync_lock_release(&app->lock, 0);
}

static inline void
RecvAppWriteFrameRgbaInline(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr,
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
			switch (convert) // will inline
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
RecvAppWriteFrame422Inline(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr,
						   void (*Unpack)(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00,
										  uint16_t *y00, uint16_t *cr00, uint16_t *y01))
{
	st_rfc4175_422_10_pg2_t const *const end = ptr + (app->frameSize / sizeof(*ptr));
	uint16_t * Yy = (uint16_t *)(app->movie + app->movieCursor + 0 * (app->movieBufSize / 4));
	uint16_t * Cr = (uint16_t *)(app->movie + app->movieCursor + 2 * (app->movieBufSize / 4));
	uint16_t * Br = (uint16_t *)(app->movie + app->movieCursor + 3 * (app->movieBufSize / 4));
	for (; ptr < end; ++ptr)
	{
		Unpack(ptr, Cr++, Yy + 0, Br++, Yy + 1);
		Yy += 2;
	}
	app->movieCursor = (app->movieCursor + app->movieBufSize) % app->movieSize;
}

#if 0
static void
RecvAppWriteFrameNetLeBufLe(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2le_422le10);
}
#endif

static void
RecvAppWriteFrameNetLeBufBe(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2le_422be10);
}

#if 0
static void
RecvAppWriteFrameNetBeBufLe(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2be_422le10);
}
#endif
static void
RecvAppWriteFrameNetBeBufBe(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrame422Inline(app, ptr, Unpack_PG2be_422be10);
}

void
RecvAppWriteFrameNetLeBufRgba(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_LE10_BUF_RGBA);
}

void
RecvAppWriteFrameNetBeBufRgba(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_BE10_BUF_RGBA);
}

void
RecvAppWriteFrameNetLeBufBgra(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_LE10_BUF_BGRA);
}

void
RecvAppWriteFrameNetBeBufBgra(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr)
{
	RecvAppWriteFrameRgbaInline(app, ptr, ST_422_FMT_CONV_NET_BE10_BUF_BGRA);
}

static void *
RecvAppThread(rvrtp_recv_app_t *app)
{
	while (rte_atomic32_read(&isAppStopped) == 0)
	{
		uint32_t const old = app->frmsRecv;
		while ((old == app->frmsRecv) && (rte_atomic32_read(&isAppStopped) == 0))
		{
			sleep(0); // wait for next frame
		}
		RecvAppLock(app);
		uint8_t *frameBuf = app->frames[app->writeCursor];
		uint8_t const*const sharedMv = app->movie + app->movieCursor;
		RecvAppUnlock(app);
		app->RecvAppWriteFrame(app, (st_rfc4175_422_10_pg2_t const *)frameBuf);
		ShowFrame(app->view, sharedMv, app->fieldId);
	}
	return app;
}

st_status_t
RecvAppInit(rvrtp_recv_app_t *app)
{
	for (uint32_t i = 0; i < RECV_APP_FRAME_MAX; i++)
	{
		app->frames[i] = malloc(app->session->frameSize);
		if (!app->frames[i])
		{
			return ST_NO_MEMORY;
		}
	}
	st21_format_t fmt;
	St21GetFormat(app->session, &fmt);

	switch (app->bufFormat)
	{
	case ST21_BUF_FMT_YUV_422_10BIT_BE:
		// for now there is only BE output buffer format
		app->movieBufSize = 2 * sizeof(uint16_t) * fmt.width * fmt.height;
		switch(fmt.vscan) {
		case ST21_720I:case ST21_1080I: case ST21_2160I:
				app->movieBufSize/=2;
				snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.yuv422p10be.yuv", app, fmt.width, fmt.height/2);
				break;
		default:
				snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.yuv422p10be.yuv", app, fmt.width, fmt.height);
		}
		switch (fmt.pixelFmt)
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
		snprintf(app->fileName, sizeof(app->fileName) - 1, "/tmp/%p.%ux%u.rgba", app, fmt.width,
				 fmt.height);
		app->movieBufSize = sizeof(st_rgba_8b_t) * fmt.width * fmt.height;
		switch (fmt.pixelFmt)
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

	app->dualPixelSize = (2 * fmt.pixelGrpSize) / fmt.pixelsInGrp;
	app->sliceSize = 20 * // at least 20 lines if single pixel group usually 40 lines
					 fmt.width * fmt.pixelGrpSize;
	app->sliceCount = app->session->frameSize / app->sliceSize;
	app->pixelGrpsInSlice = app->sliceSize / fmt.pixelGrpSize;
	app->linesInSlice = 40; // for now TBD

	app->frameSize = app->session->frameSize;

#if 1 // def DEBUG
	RTE_LOG(INFO, USER2, "RecvApp dualPixelSize %u sliceSize %u sliceCount %u\n",
			app->dualPixelSize, app->sliceSize, app->sliceCount);
	RTE_LOG(INFO, USER2, "RecvApp pixelGrpsInSlice %u linesInSlice %u frameSize %u\n",
			app->pixelGrpsInSlice, app->linesInSlice, app->frameSize);
#endif
	app->movieCursor = 0;
	app->movieSize = 4 * app->movieBufSize;
	uint32_t fileSizeBytes = app->movieSize;

	app->fileFd = open(app->fileName, O_CREAT | O_RDWR, 0640);
	int ret = ftruncate(app->fileFd, fileSizeBytes);
	if (ret < 0)
	{
		return ST_GENERAL_ERR;
	}
	app->movie = mmap(NULL, fileSizeBytes, PROT_READ | PROT_WRITE, MAP_SHARED, app->fileFd, 0);

	app->writeCursor = 0;
	app->inputCursor = 0;
	app->frmsRecv = 0;

	rte_atomic32_set(&isAppStopped, 0);

	pthread_create(&app->writeThread, NULL, (void*(*)(void*))RecvAppThread, (void *)app);

	return ST_OK;
}

st_status_t
RecvAppCreateConsumer(st21_session_t *sn, st21_buf_fmt_t bufFormat, rvrtp_recv_app_t **appOut)
{
	if (!sn)
		return ST_INVALID_PARAM;
	rvrtp_recv_app_t *app = rte_malloc_socket("RecvApp", sizeof(rvrtp_recv_app_t), RTE_CACHE_LINE_SIZE, rte_socket_id());;
	if (app)
	{
		memset(app, 0x0, sizeof(rvrtp_recv_app_t));
		app->session = sn;
		app->bufFormat = bufFormat;
		st_status_t status = RecvAppInit(app);
		if (status == ST_OK)
		{
			st21_consumer_t cons;
			memset(&cons, 0, sizeof(cons));
			cons.appHandle = app;
			cons.frameSize = sn->frameSize;
			cons.consType = ST21_CONS_P_FRAME;
			cons.St21NotifyFrameRecv = RecvAppNotifyFrameRecv;
			cons.St21NotifyFrameDone = RecvAppNotifyFrameDone;
			cons.St21GetNextFrameBuf = RecvAppGetNextFrameBuf;
			cons.St21PutFrameTmstamp = RecvAppPutFrameTmstamp;
			status = St21RegisterConsumer(sn, &cons);
			if (status != ST_OK)
			{
				RTE_LOG(INFO, USER2, "St21RegisterConsumer FAILED. ErrNo: %d\n", status);
				rte_free(app);
				return status;
			}
			*appOut = app;
		}
		else
		{
			RTE_LOG(INFO, USER3, "RecvAppInit error of %d\n", status);
		}
		// ToDo ~RecvAppDeInit - app contain open file!
		return status;
	}

	return ST_NO_MEMORY;
}

st_status_t
RecvAppStart(st21_session_t *sn, rvrtp_recv_app_t *app)
{
	st_status_t status = ST_OK;

	if (!sn || !app)
		return ST_INVALID_PARAM;

	status = St21ConsumerStartFrame(sn, app->frames[0], 0);

	return status;
}

st_status_t
RecvAppStop(st21_session_t *sn, rvrtp_recv_app_t *app)
{
	st_status_t status = ST_OK;

	if (!sn || !app)
		return ST_INVALID_PARAM;

	rte_atomic32_set(&isAppStopped, 1);
	(void)pthread_join(app->writeThread, NULL);

	status = St21ConsumerStop(sn);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "St21ConsumerStop FAILED. ErrNo: %d\n", status);
		return status;
	}

	return status;
}

/**
 * Callback to producer application to get next frame buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and then has to call St21ProducerUpdate
 * to restart streaming
 */
uint8_t *
RecvAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize, uint32_t fieldId)
{
	uint8_t *nextBuf;
	rvrtp_recv_app_t *app = (rvrtp_recv_app_t *)appHandle;
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
	// RTE_LOG(INFO, USER3, "RecvAppNotifyFrameDone\n");
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
	rvrtp_recv_app_t *app = (rvrtp_recv_app_t *)appHandle;

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
	RTE_LOG(DEBUG,USER1,"Field %u             %p\n",fieldId, frameBuf);
	app->fieldId=fieldId;
	app->frmsRecv++;
}

void
RecvAppPutFrameTmstamp(void *appHandle, uint32_t tmstamp)
{
	//rvrtp_recv_app_t *app = (rvrtp_recv_app_t *)appHandle;

	// printf("Received timestamp of during frame of %llu of %u\n", (U64)app->frmsRecv, tmstamp);
	return;
}
