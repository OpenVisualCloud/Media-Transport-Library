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
#define SEND_APP_FRAME_MAX 3
#define RECV_APP_FRAME_MAX 6

#define ST_ANC_UDW_MAX_SIZE 255 * 10 / 8

#define RECV_APP_SAMPLE_MAX 3072
#define RECV_APP_AUDIO_BUF_MAX 6

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

struct strtp_send_app;

typedef st_status_t (*SendAppReadFrame_f)(struct strtp_send_app *app);

typedef struct strtp_send_app
{
	st_session_t *session;
	st_essence_type_t mtype;
	void *prod;			  //
	uint8_t bufFormat;	  // producer buffer format
	int singleFrameMode;  // if file size is equal to frame size
	int fileFd;			  // -pix_fmt yuv440p10be
	char fileName[256];
	const uint8_t *movieBegin;	// mmap movie (video/audio/anc)
	const uint8_t *movieEnd;	// mmap movie
	const uint8_t *movie;		// current movie frame
	pthread_t movieThread;		// calling SendAppReadFrame
	uint32_t movieBufSize;
	uint32_t audioSampleSize;
	bool isEndOfAncDataBuf;
	uint32_t tmstampTime;
	union
	{
		uint8_t *frames[SEND_APP_FRAME_MAX];
		strtp_ancFrame_t ancFrames[SEND_APP_FRAME_MAX];
	};
	volatile uint8_t frameDone[SEND_APP_FRAME_MAX];
	uint8_t index;	// Identifier

	pthread_t cldThr;  // frameThread
	int iscldThrSet;
	int affinited;

	// functions set per video format
	SendAppReadFrame_f SendAppReadFrame;

	volatile uint32_t frmsSend;	 // sync SendAppGetNextFrameBuf and SendAppReadFrame

	volatile int lock;
	video_stream_info_t *videoStream;
} strtp_send_app_t;

struct strtp_recv_app;

typedef void (*RecvAppWriteFrame_f)(struct strtp_recv_app *app, st_rfc4175_422_10_pg2_t const *ptr);
typedef void (*RecvAppWriteBuffer_f)(struct strtp_recv_app *app, uint8_t const *ptr);

typedef struct strtp_recv_app
{
	char fileName[256];
	st_session_t *session;
	st21_buf_fmt_t bufFormat;  // consumer buffer format
	int fileFd;				   // file descriptor for share yuv422p10be
	uint8_t *movie;			   // mmap of yuv422p10be

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

	union
	{
		uint8_t *frames[RECV_APP_FRAME_MAX];
		uint8_t *samples[RECV_APP_SAMPLE_MAX];
		strtp_ancFrame_t ancFrames[RECV_APP_FRAME_MAX];
	};
	uint32_t inputCursor;
	uint32_t volatile writeCursor;
	uint32_t volatile readCursor;
	uint32_t volatile framesToRead;

	// functions set per video format
	union
	{
		RecvAppWriteFrame_f RecvAppWriteFrame;
		RecvAppWriteBuffer_f RecvAppWriteAudioFrame;
	};

	volatile uint32_t frmsRecv;
	unsigned volatile fieldId; /**< 0 even, 1 odd*/

	pthread_t writeThread;	// calling RecvAppWriteFrame

	volatile int lock;

	video_stream_info_t *videoStream;
	union
	{
		audio_ref_t *ref;
		anc_ref_t *ancref;
	};

} strtp_recv_app_t;

// External declarations (library based)
extern void StGetAppAffinityCores(uint16_t start_id, cpu_set_t *app_cpuset);
// Sender app forward declarations
st_status_t SendAppReadFrameNetLeBufLe(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufBe(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufLe(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufBe(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufRgba(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetLeBufBgra(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufRgba(strtp_send_app_t *app);
st_status_t SendAppReadFrameNetBeBufBgra(strtp_send_app_t *app);

st_status_t SendSt21AppInit(strtp_send_app_t *app, void *prod);
st_status_t SendSt30AppInit(strtp_send_app_t *app, void *prod);
st_status_t SendSt40AppInit(strtp_send_app_t *app, void *prod);
st_status_t SendAppCreateProducer(st_session_t *sn, uint8_t bufFormat, const char *fileName,
								  strtp_send_app_t **appOut);
st_status_t SendAppStart(st_session_t *sn, strtp_send_app_t *app);

st_status_t RecvAppCreateConsumer(st_session_t *sn, st21_buf_fmt_t bufFormat,
								  strtp_recv_app_t **appOut);
st_status_t RecvAppStart(st_session_t *sn, strtp_recv_app_t *app);
st_status_t RecvAppStop(st_session_t *sn, strtp_recv_app_t *app);

uint32_t SendAppReadNextSlice(strtp_send_app_t *app, uint8_t *frameBuf, uint32_t prevOffset,
							  uint32_t sliceSize, uint32_t fieldId);

/**
 * Callback to producer or consumer application to get next frame buffer necessary to continue
 * streaming If application cannot return the next buffer returns NULL and then has to call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming
 */
uint8_t *SendAppGetNextFrameBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize,
								uint32_t fieldId);

uint8_t *SendAppGetNextAudioBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize);

uint8_t *SendAppGetNextAncBuf(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize);

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
void SendAppNotifyBufDone(void *appHandle, uint8_t *frameBuf);

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void SendAppNotifyStopDone(void *appHandle);

/**
 * Callback to producer application to get timestamp as transported in SDI frame
 */
uint32_t SendAppGetFrameTmstamp(void *appHandle);

/**
 * Callback to producer application to get next ancillary buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and TBD
 */
void *SendAppGetNextAncFrame(void *appHandle);

// Receiver App forward declarations

st_status_t RecvAppInit(strtp_recv_app_t *app);

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

void RecvAppWriteFrameNetLeBufRgba(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetLeBufBgra(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetBeBufRgba(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);
void RecvAppWriteFrameNetBeBufBgra(strtp_recv_app_t *app, st_rfc4175_422_10_pg2_t const *ptr);

/**
 * Callback to producer or consumer application with notification about the buffer completion
 * Audio buffer can be released or reused after it but not sooner
 */
void RecvAppNotifyBufferDone(void *appHandle, uint8_t *frameBuf);

/**
 * Callback to producer application to get next audio buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and TBD then has to call St30ProducerUpdate
 * to restart streaming
 */
uint8_t *RecvAppGetNextAudioBuf(void *appHandle, uint8_t *prevAudioBuf, uint32_t bufSize);

/**
 * Callback to consumer application to get next ancillary buffer necessary to continue streaming
 */
void *RecvAppGetNextAncFrame(void *appHandle);

/**
 * Callback to producer or consumer application with notification about completion of the session
 * stop It means that all buffer pointers can be released after it but not sooner
 */
void RecvAppNotifySampleRecv(void *appHandle, uint8_t *audioBuf, uint32_t bufOffset,
							 uint32_t tmstamp);

void AppInitAffinity(int appStartCoreId);
void SetAffinityCore(void *app, st_dev_type_t type);

#endif
