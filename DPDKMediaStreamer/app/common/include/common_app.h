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

#ifndef _COMMON_APP_H
#define _COMMON_APP_H

/* Self includes */
#include "rx_view.h"
#include "st_api.h"
#include "st_fmt.h"
#include "st_pack.h"

/* DPDK includes */
#include <rte_eal.h>
#include <rte_malloc.h>

/* Defines */
#define U64 unsigned long long
#define SEND_APP_FRAME_MAX 2
#define RECV_APP_FRAME_MAX 6

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

#define ST_APP_ASSERT                                                                              \
	rte_exit(127, "ASSERT error file %s function %s line %u\n", __FILE__, __FUNCTION__, __LINE__)

typedef enum
{
	ST_422_FMT_CONV_NET_LE10_BUF_LE10 = 0,
	ST_422_FMT_CONV_NET_LE10_BUF_BE10,
	ST_422_FMT_CONV_NET_BE10_BUF_LE10,
	ST_422_FMT_CONV_NET_BE10_BUF_BE10,

	ST_422_FMT_CONV_NET_LE10_BUF_RGBA,
	ST_422_FMT_CONV_NET_BE10_BUF_RGBA,
	ST_422_FMT_CONV_NET_LE10_BUF_BGRA,
	ST_422_FMT_CONV_NET_BE10_BUF_BGRA,

} st_vid_fmt_conv_t;

struct rvrtp_send_app;

typedef st_status_t (*SendAppReadFrame_f)(struct rvrtp_send_app *app);

typedef struct rvrtp_send_app
{
	st21_session_t *session;
	st21_producer_t prod;
	st21_buf_fmt_t bufFormat;  // producer buffer format
	int singleFrameMode;	   // if file size is equal to frame size
	int fileFd;				   // -pix_fmt yuv440p10be
	char fileName[256];
	const uint8_t *movieBegin;	// mmap movie
	const uint8_t *movieEnd;	// mmap movie
	const uint8_t *movie;		// current movie frame
	pthread_t movieThread;		// calling SendAppReadFrame
	uint32_t movieBufSize;
	uint32_t sliceSize;	   // at least 2 lines
	uint32_t sliceOffset;  //
	uint32_t sliceCount;   //
	uint32_t frameSize;
	uint32_t tmstampTime;

	uint32_t dualPixelSize;
	uint32_t pixelGrpsInSlice;
	uint32_t linesInSlice;
	uint32_t firstTmstamp;

	uint8_t *frameBuf;	// current frameBuffer
	uint8_t *frames[SEND_APP_FRAME_MAX];
	uint32_t frameCursor;
	volatile uint32_t frameCursorSending;
	uint64_t lastTmr;
	uint32_t frmLocCnt;
	pthread_t cldThr;  // frameThread
	int iscldThrSet;
	int affinited;

	// functions set per video format
	SendAppReadFrame_f SendAppReadFrame;

	volatile uint32_t frmsSend;	 // sync SendAppGetNextFrameBuf and SendAppReadFrame

	volatile int lock;
	view_info_t *view;
} rvrtp_send_app_t;

struct rvrtp_recv_app;

typedef void (*RecvAppWriteFrame_f)(struct rvrtp_recv_app *app, st_rfc4175_422_10_pg2_t const *ptr);

typedef struct rvrtp_recv_app
{
	st21_session_t *session;
	st21_buf_fmt_t bufFormat;  // consumer buffer format
	int fileFd;				   // file descriptor for share yuv422p10be
	char fileName[256];
	uint8_t *movie; // mmap of yuv422p10be

	uint32_t volatile movieCursor;
	uint32_t movieBufSize;
	uint32_t movieSize;

	uint32_t sliceSize;	   // at least 2 lines
	uint32_t sliceOffset;  //
	uint32_t sliceCount;   //
	uint32_t frameSize;

	uint32_t dualPixelSize;
	uint32_t pixelGrpsInSlice;
	uint32_t linesInSlice;

	uint8_t *frames[RECV_APP_FRAME_MAX];
	uint32_t inputCursor;
	uint32_t volatile writeCursor;

	// functions set per video format
	RecvAppWriteFrame_f RecvAppWriteFrame;

	volatile uint32_t frmsRecv;
	unsigned volatile fieldId; /**< 0 even, 1 odd*/

	pthread_t writeThread;	// calling RecvAppWriteFrame

	volatile int lock;

	view_info_t *view;
} rvrtp_recv_app_t;

// Sender app forward declarations
st_status_t SendAppReadFrameNetLeBufLe(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufBe(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufLe(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufBe(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufRgba(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufBgra(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufRgba(rvrtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufBgra(rvrtp_send_app_t *app);

st_status_t SendAppInit(rvrtp_send_app_t *app, const char *fileName);
st_status_t SendAppCreateProducer(st21_session_t *sn, st21_buf_fmt_t bufFormat, uint32_t fmtIndex,
								  rvrtp_send_app_t **appOut);
st_status_t SendAppStart(st21_session_t *sn, rvrtp_send_app_t *app);

st_status_t RecvAppCreateConsumer(st21_session_t *sn, st21_buf_fmt_t bufFormat,
								  rvrtp_recv_app_t **appOut);
st_status_t RecvAppStart(st21_session_t *sn, rvrtp_recv_app_t *app);
st_status_t RecvAppStop(st21_session_t *sn, rvrtp_recv_app_t *app);

uint32_t SendAppReadNextSlice(rvrtp_send_app_t *app, uint8_t *frameBuf, uint32_t prevOffset,
							  uint32_t sliceSize, uint32_t fieldId);

/**
 * Callback to producer or consumer application to get next frame buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming
 */
uint8_t *SendAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize,
								uint32_t fieldId);

/**
 * Callback to producer or consumer application to get next slice buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate to restart streaming
 */
uint32_t SendAppGetNextSliceOffset(void *appHandle, uint8_t *frameBuf, uint32_t prevOffset,
								   uint32_t fieldId);

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
void SendAppNotifyFrameDone(void *appHandle, uint8_t *frameBuf, uint32_t fieldId);

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void SendAppNotifyStopDone(void *appHandle);

/**
 * Callback to producer application to get timestamp as transported in SDI frame
 */
uint32_t SendAppGetFrameTmstamp(void *appHandle);

// Receiver App forward declarations

st_status_t RecvAppInit(rvrtp_recv_app_t *app);

/**
 * Callback to producer application to get next frame buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and then has to call St21ProducerUpdate
 * to restart streaming
 */
uint8_t *RecvAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize,
								uint32_t fieldId);

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
void RecvAppNotifyFrameDone(void *appHandle, uint8_t *frameBuf, uint32_t fieldId);

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void RecvAppNotifyFrameRecv(void *appHandle, uint8_t *frameBuf, uint32_t tmstamp, uint32_t fieldId);

void RecvAppPutFrameTmstamp(void *appHandle, uint32_t tmstamp);

void RecvAppWriteFrameNetLeBufRgba(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetLeBufBgra(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetBeBufRgba(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetBeBufBgra(rvrtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);

#endif
