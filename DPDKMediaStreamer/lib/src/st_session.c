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

#include "rvrtp_main.h"
#include "st_arp.h"
#include "st_flw_cls.h"
#include "st_igmp.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ST_ARP_SEARCH_CHECK_US (5 * 1000 * 1000) /* every 5 seconds */
#define ST_ARP_SEARCH_DELAY_US (50 * 1000)		 /* 50ms */
#define ST_ARP_SEARCH_CHECK_POINT (ST_ARP_SEARCH_CHECK_US / ST_ARP_SEARCH_DELAY_US)

static st_session_method_t sn_method[ST_MAX_ESSENCE];

st_essence_type_t
st_get_essence_type(st_session_t *session)
{
	return session->type;
}

void
st_init_session_method(st_session_method_t *method, st_essence_type_t type)
{
	method->init = 1;
	sn_method[type] = *method;
}

int
StSessionGetPktsize(st_session_impl_t *s)
{
	if(!s)
	{
		return -1;
	}

	st_essence_type_t mtype = st_get_essence_type(&s->sn);

	switch (mtype)
	{
	case ST_ESSENCE_VIDEO:
		return s->fmt.v.pktSize;
	case ST_ESSENCE_AUDIO:
		return s->fmt.a.pktSize;
	case ST_ESSENCE_ANC:
		return s->ancctx.pktSize;
	default:
		return -1;
	}

	return -1;
}

static inline void
StDeviceLock(st_device_impl_t *d)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&d->lock, 1);
	} while (lock != 0);
}

static inline void
StDeviceUnlock(st_device_impl_t *d)
{
	__sync_lock_release(&d->lock, 0);
}

st_status_t
StRtpSendDeviceAdjustBudget(st_device_impl_t *dev)
{
	if(!dev)
	{
		return ST_INVALID_PARAM;
	}

	uint32_t budget = dev->quot;

	for (uint32_t i = 0; i < dev->dev.maxSt21Sessions; i++)
	{
		if (dev->snTable[i])
		{
			dev->txPktSizeL1[i] = StSessionGetPktsize(dev->snTable[i]) + ST_PHYS_PKT_ADD;
		}
		else
		{
			dev->txPktSizeL1[i] = ST_HD_422_10_SLN_L1_SZ;
		}
		budget -= dev->txPktSizeL1[i];
	}
	for (uint32_t i = dev->dev.maxSt21Sessions; i < dev->maxRings; i++)
	{
		if (budget >= ST_DEFAULT_PKT_L1_SZ)
		{
			dev->txPktSizeL1[i] = ST_DEFAULT_PKT_L1_SZ;
			budget -= dev->txPktSizeL1[i];
		}
		else if (budget >= ST_MIN_PKT_L1_SZ)
		{
			dev->txPktSizeL1[i] = budget;
		}
		else
		{
			ST_ASSERT;
			return ST_GENERAL_ERR;
		}
	}
	return ST_OK;
}

st_status_t
StValidateSession(st_session_t *sn)
{
	bool found = false;

	// validate session parameter
	if (!sn)
	{
		return ST_INVALID_PARAM;
	}

	// search if current session exist in the TX session table
	for (int i = 0; i < stSendDevice.dev.snCount; i++)
	{
		if (&stSendDevice.snTable[i]->sn == sn)
		{
			found = true;
			break;
		}
	}
	for (int i = 0; i < stSendDevice.dev.sn30Count; i++)
	{
		if (&stSendDevice.sn30Table[i]->sn == sn)
		{
			found = true;
			break;
		}
	}
	for (int i = 0; i < stSendDevice.dev.sn40Count; i++)
	{
		if (&stSendDevice.sn40Table[i]->sn == sn)
		{
			found = true;
			break;
		}
	}

	// search if current session exist in the RX session table
	for (int i = 0; i < stRecvDevice.dev.snCount; i++)
	{
		if (&stRecvDevice.snTable[i]->sn == sn)
		{
			found = true;
			break;
		}
	}

	for (int i = 0; i < stRecvDevice.dev.sn30Count; i++)
	{
		if (&stRecvDevice.sn30Table[i]->sn == sn)
		{
			found = true;
			break;
		}
	}

	for (int i = 0; i < stRecvDevice.dev.sn40Count; i++)
	{
		if (&stRecvDevice.sn40Table[i]->sn == sn)
		{
			found = true;
			break;
		}
	}

	if (found)
	{
		return ST_OK;
	}

	return ST_SN_ERR_NOT_READY;
}

st_status_t
StValidateDevice(st_device_t *dev)
{
	bool found = false;

	/* validate device parameter */
	if (!dev)
	{
		return ST_INVALID_PARAM;
	}

	if (&stRecvDevice.dev == dev || &stSendDevice.dev == dev)
	{
		found = true;
	}

	if (found)
	{
		return ST_OK;
	}

	return ST_DEV_ERR_NOT_READY;
}

st_status_t
StValidateProducer(void *producer, st_essence_type_t type)
{
	if (!producer)
	{
		return ST_INVALID_PARAM;
	}

	if (type == ST_ESSENCE_VIDEO)
	{
		st21_producer_t *videoProducer = (st21_producer_t *)producer;
		switch (videoProducer->prodType)
		{
		case ST21_PROD_INVALID:
		case ST21_PROD_P_FRAME:
		case ST21_PROD_P_FRAME_TMSTAMP:
		case ST21_PROD_I_FIELD:
		case ST21_PROD_I_FIELD_TMSTAMP:
		case ST21_PROD_P_FRAME_SLICE:
		case ST21_PROD_P_SLICE_TMSTAMP:
		case ST21_PROD_I_FIELD_SLICE:
		case ST21_PROD_I_SLICE_TMSTAMP:
		case ST21_PROD_RAW_RTP:
		case ST21_PROD_RAW_L2_PKT:
			break;
		default:
			return ST_INVALID_PARAM;
		}
	}
	return ST_OK;
}

st_status_t
StGetSessionCount(st_device_t *dev, uint32_t *count)
{
	st_status_t status = ST_OK;

	if (!count)
	{
		return ST_INVALID_PARAM;
	}

	status = StValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}
	st_device_impl_t *d = (st_device_impl_t *)dev;

	*count = d->snCount + d->sn30Count + d->sn40Count;

	return status;
}

static void
StInitSessionMethod()
{
	/*video */
	rvrtp_method_init();

	/*audio */
	rartp_method_init();

	/* ancillary */
	ranc_method_init();

	return;
}

/**
 * Called by the application to create a new session on NIC device
 */
st_status_t
StCreateSession(st_device_t *dev,	   // IN device on which to create session
				st_session_t *inSn,	   // IN structure
				st_format_t *fmt,	   // IN session packet's format, optional for receiver
				st_session_t **outSn)  // OUT created session object w/ fields updated respectively
{
	st_status_t status = ST_OK;
	st_essence_type_t mtype;
	st_session_impl_t *s;

	if ((!inSn) || (!fmt) || (!outSn))
	{
		return ST_INVALID_PARAM;
	}

	status = StValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	StInitSessionMethod();
	mtype = st_get_essence_type(inSn);
	st_device_impl_t *d = (st_device_impl_t *)dev;

	if (mtype == ST_ESSENCE_AUDIO)
	{
		fmt->a.pktSize = ((stMainParams.audioFrameSize > 0)
						  && (stMainParams.audioFrameSize < ST_MAX_AUDIO_PKT_SIZE))
							 ? (stMainParams.audioFrameSize + ST_PKT_AUDIO_HDR_LEN)
							 : fmt->a.pktSize;
	}

	StDeviceLock(d);
	switch (dev->type)
	{
	case ST_DEV_TYPE_PRODUCER:
		status = sn_method[mtype].create_tx_session(d, inSn, fmt, &s);
		break;

	case ST_DEV_TYPE_CONSUMER:
		status = sn_method[mtype].create_rx_session(d, inSn, fmt, &s);
		break;

	default:
		status = ST_GENERAL_ERR;
	}
	if (status == ST_OK)
	{
		s->sn.fmt = fmt;
		// update device and session out ptr
		*outSn = (st_session_t *)s;
		if (mtype == ST_ESSENCE_VIDEO)
		{
			d->snTable[s->sn.timeslot] = s;
			d->snCount++;
			if (dev->type == ST_DEV_TYPE_PRODUCER)
				status = StRtpSendDeviceAdjustBudget(d);
		}
		else if (mtype == ST_ESSENCE_AUDIO)
		{
			d->sn30Table[s->sn.timeslot] = s;
			d->sn30Count++;
		}
		else if (mtype == ST_ESSENCE_ANC)
		{
			d->sn40Table[s->sn.timeslot] = s;
			d->sn40Count++;
		}
	}
	StDeviceUnlock(d);
	return status;
}

/**
 * Called by the applications to get format information of the session.
 * This is complementary method to St21GetSdp and several St21GetParam
 */
st_status_t
StGetFormat(st_session_t *sn, st_format_t *fmt)
{
	st_status_t status = ST_OK;

	if (!fmt)
	{
		return ST_INVALID_PARAM;
	}

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	*fmt = s->fmt;
	return status;
}

/**
 * Called by the application to remove the session from NIC device
 * on which it has been created
 */
st_status_t
StDestroySession(st_session_t *sn)
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_essence_type_t mtype = sn->type;
	st_session_impl_t *s = (st_session_impl_t *)sn;
	st_device_impl_t *d = s->dev;

	if ((d != &stSendDevice) && (d != &stRecvDevice))
	{
		return ST_INVALID_PARAM;
	}

	StDeviceLock(d);
	StSessionLock(s);

	__sync_fetch_and_and(&d->snTable[s->sn.timeslot], NULL);
	d->snCount--;
	if (d->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		for(int i = 0; i < sn->extMem.numExtBuf; i++)
			StFreeFrame(sn, sn->extMem.addr[i]);

		s->prodBuf = NULL;

		StSessionUnlock(s);
		status = sn_method[mtype].destroy_tx_session(s);
	}
	else
	{
		StSessionUnlock(s);
		status = sn_method[mtype].destroy_rx_session(s);
	}
	StDeviceUnlock(d);
	return status;
}

/**
 * Called by the producer to register live producer for video streaming
 */
st_status_t
StRegisterProducer(st_session_t *sn,  // IN session pointer
				   void *prod)	// IN register callbacks to allow interaction with live producer
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}
	st_session_impl_t *s = (st_session_impl_t *)sn;

	status = StValidateProducer(prod, sn->type);
	if (status != ST_OK)
	{
		return status;
	}

	StSessionLock(s);

	/* TODO use "void *" in the future */
	if (sn->type == ST_ESSENCE_VIDEO)
		s->prod = *(st21_producer_t *)prod;
	else if (sn->type == ST_ESSENCE_AUDIO)
		s->aprod = *(st30_producer_t *)prod;
	else
		s->ancprod = *(st40_producer_t *)prod;

	StSessionUnlock(s);

	return status;
}

/**
 * Called by the producer asynchronously to start each frame of video streaming
 */
st_status_t
St21ProducerStartFrame(
	st_session_t *sn,	   // IN session pointer
	uint8_t *frameBuf,	   // IN 1st frame buffer for the session
	uint32_t linesOffset,  // IN offset in complete lines of the frameBuf to which producer filled
						   // the buffer
	uint32_t tmstamp,	   // IN if not 0 then 90kHz timestamp of the frame
	uint64_t ptpTime)	   // IN if not 0 start new frame at the given PTP timestamp + TROFFSET
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!frameBuf))
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	StSessionLock(s);

	s->prodBuf = NULL;
	s->sliceOffset = linesOffset;
	s->vctx.sliceOffset = 0;
	s->state = ST_SN_STATE_NO_NEXT_FRAME;

	StSessionUnlock(s);

	return status;
}

/**
 * Called by the producer asynchronously to start each frame of video streaming
 */
/* TODO
 * consider merge duplicate code with St21ProducerStartFrame
 */
st_status_t
St30ProducerStartFrame(
	st_session_t *sn,	   // IN session pointer
	uint8_t *audioBuf,	   // IN 1st frame buffer for the session
	uint32_t linesOffset,  // IN offset in complete lines of the audioBuf to which producer filled
						   // the buffer
	uint32_t tmstamp,	   // IN if not 0 then 90kHz timestamp of the frame
	uint64_t ptpTime)	   // IN if not 0 start new frame at the given PTP timestamp + TROFFSET
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!audioBuf))
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	StSessionLock(s);

	s->prodBuf = audioBuf;
	s->bufOffset = linesOffset;

	if (linesOffset)
	{
		s->state = ST_SN_STATE_RUN;
	}
	else
	{
		s->state = ST_SN_STATE_NO_NEXT_SLICE;
		status = ST_BUFFER_NOT_READY;
	}

	StSessionUnlock(s);

	return status;
}
/**
 * Called by the producer asynchronously to start each frame of video streaming
 * TODO
 * consider merge duplicate code with St21ProducerStartFrame
 */
st_status_t
St40ProducerStartFrame(
	st_session_t *sn,	  // IN session pointer
	uint8_t *ancBuf,	  // IN 1st frame buffer for the session
	uint32_t buffOffset,  // IN offset in complete lines of the frameBuf to which producer filled
						  // the buffer
	uint32_t tmstamp,	  // IN if not 0 then 90kHz timestamp of the frame
	uint64_t ptpTime)	  // IN if not 0 start new frame at the given PTP timestamp + TROFFSET
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!ancBuf))
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	StSessionLock(s);

	s->prodBuf = ancBuf;
	s->sliceOffset = buffOffset;

	if (buffOffset)
	{
		s->state = ST_SN_STATE_RUN;
	}
	else
	{
		s->state = ST_SN_STATE_NO_NEXT_SLICE;
		status = ST_BUFFER_NOT_READY;
	}

	StSessionUnlock(s);

	return status;
}
/**
 * Called by the producer asynchronously to update video streaming
 * in case producer has more data to send, it also restart streaming
 * if the producer callback failed due to lack of buffer with video
 */
st_status_t
St21ProducerUpdate(st_session_t *sn,	  // IN session pointer
				   uint8_t *frameBuf,	  // IN frame buffer for the session from which to restart
				   uint32_t linesOffset)  // IN offset in complete lines of the frameBuf to which
										  // producer filled the buffer
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!frameBuf))
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return status;
	}

	StSessionLock(s);

	s->prodBuf = frameBuf;
	s->sliceOffset = linesOffset;

	if (linesOffset)
	{
		s->state = ST_SN_STATE_RUN;
	}
	else
	{
		s->state = ST_SN_STATE_NO_NEXT_SLICE;
		status = ST_BUFFER_NOT_READY;
	}

	StSessionUnlock(s);

	return status;
}

st_status_t
St30ProducerUpdate(st_session_t *sn,
					uint8_t *audioBuf,
					uint32_t bufOffset,
					uint32_t tmstamp,
					uint64_t ptpTime)
{
	return ST_NOT_IMPLEMENTED;
}

/**
 * Called by the producer asynchronously to stop video streaming,
 * the session will notify the producer about completion with callback
 */
st_status_t
StProducerStop(st_session_t *sn)
{
	st_status_t status = ST_OK;

	// validate parameters
	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	st_device_impl_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}
	// before session destroy, should wait enqueue/schedule thread done
	if (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		int i = 0;
		rte_atomic32_set(&isTxDevToDestroy, 1);
		while (d->rte_thread_core[i] != -1)
		{
			rte_eal_wait_lcore(d->rte_thread_core[i]);
			d->rte_thread_core[i] = -1;
			i++;
		}
	}

	StSessionLock(s);

	s->state = ST_SN_STATE_STOP_PENDING;

	StSessionUnlock(s);

	return status;
}

st_status_t
St30ProducerStop(st_session_t *sn)
{
	return ST_NOT_IMPLEMENTED;
}

st_status_t
St21ValidateCons(st21_consumer_t *cons)
{
	if(!cons)
	{
		return ST_INVALID_PARAM;
	}

	switch (cons->consType)
	{
	case ST21_CONS_RAW_L2_PKT:
	case ST21_CONS_RAW_RTP:
		if ((!cons->St21RecvRtpPkt) || (cons->St21GetNextFrameBuf) || (cons->St21NotifyFrameRecv)
			|| (cons->St21PutFrameTmstamp) || (cons->St21NotifyFrameDone)
			|| (cons->St21NotifySliceRecv) || (cons->St21NotifySliceDone)
			|| (cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_P_FRAME:
	case ST21_CONS_I_FIELD:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv)
			|| (!cons->St21PutFrameTmstamp) || (!cons->St21NotifyFrameDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_P_FRAME_TMSTAMP:
	case ST21_CONS_I_FIELD_TMSTAMP:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv)
			|| (!cons->St21PutFrameTmstamp) || (!cons->St21NotifyFrameDone)
			|| (!cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_I_FIELD_SLICE:
	case ST21_CONS_P_FRAME_SLICE:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv)
			|| (!cons->St21PutFrameTmstamp) || (!cons->St21NotifyFrameDone)
			|| (!cons->St21NotifySliceRecv) || (!cons->St21NotifySliceDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_I_SLICE_TMSTAMP:
	case ST21_CONS_P_SLICE_TMSTAMP:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv)
			|| (!cons->St21PutFrameTmstamp) || (!cons->St21NotifyFrameDone)
			|| (!cons->St21NotifySliceRecv) || (!cons->St21NotifySliceDone)
			|| (!cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	default:
		return ST_INVALID_PARAM;
	}

	return ST_OK;
}

/**
 * Called by the consumerr to register live receiver for video streaming
 */
st_status_t
St30ValidateCons(st30_consumer_t *cons)
{
	st_status_t status = ST_OK;

	if(!cons)
	{
		return ST_INVALID_PARAM;
	}
	/* TODO
     * a workarond for st30 consumer
     */
	return ST_OK;

	if (!cons || cons->consType < ST30_CONS_INVALID || cons->consType >= ST30_CONS_LAST)
	{
		return ST_INVALID_PARAM;
	}

	switch (cons->consType)
	{
	case ST30_CONS_RAW_L2_PKT:
	case ST30_CONS_RAW_RTP:
		if ((!cons->St30RecvRtpPkt) || (cons->St30GetNextAudioBuf) || (cons->St30NotifySampleRecv)
			|| (cons->St30NotifyBufferDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST30_CONS_REGULAR:
		if ((!cons->St30GetNextAudioBuf) || (!cons->St30NotifySampleRecv)
			|| (!cons->St30NotifyBufferDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	default:
		return ST_INVALID_PARAM;
	}

	return status;
}

st_status_t
St40ValidateCons(st40_consumer_t *cons)
{
	st_status_t status = ST_OK;

	if(!cons)
	{
		return ST_INVALID_PARAM;
	}

	if (!cons || cons->consType < ST40_CONS_INVALID || cons->consType > ST40_CONS_LAST)
	{
		return ST_INVALID_PARAM;
	}

	switch (cons->consType)
	{
	case ST40_CONS_REGULAR:
		if ((!cons->St40GetNextAncFrame) || (!cons->St40NotifyFrameDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	default:
		return ST_INVALID_PARAM;
	}
	return status;
}
/**
 * Called by the consumerr to register live receiver for video streaming
 */
st_status_t
StRegisterConsumer(st_session_t *sn,  // IN session pointer
				   void *cons)	// IN register callbacks to allow interaction with live receiver
{
	st_status_t status = ST_OK;

	/* TODO need to add this check back */
	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}
	if(!cons)
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	StSessionLock(s);

	/* TODO use "void *" in the future */
	switch (sn->type)
	{
	case ST_ESSENCE_VIDEO:
		status = St21ValidateCons((st21_consumer_t *)cons);
		if (status == ST_OK)
		{
			s->cons = *(st21_consumer_t *)cons;
			/* TODO: need to use a unified way for callback */
			if ((s->cons.consType == ST21_CONS_RAW_L2_PKT)
				|| (s->cons.consType == ST21_CONS_RAW_RTP))
				s->RecvRtpPkt = RvRtpReceivePacketCallback;
		}
		break;

	case ST_ESSENCE_AUDIO:
		status = St30ValidateCons((st30_consumer_t *)cons);
		if (status == ST_OK)
		{
			s->acons = *(st30_consumer_t *)cons;
		}
		break;

	case ST_ESSENCE_ANC:
		status = St40ValidateCons((st40_consumer_t *)cons);
		if (status == ST_OK)
		{
			s->anccons = *(st40_consumer_t *)cons;
		}
		break;

	default:
		break;
	}

	s->consState = FRAME_PREV;
	s->state = ST_SN_STATE_ON;

	StSessionUnlock(s);
	return status;
}

/**
 * Called by the consumer asynchronously to start 1st frame of video streaming
 */
st_status_t
St21ConsumerStartFrame(
	st_session_t *sn,	// IN session pointer
	uint8_t *frameBuf,	// IN 1st frame buffer for the session
	uint64_t ptpTime)	// IN if not 0 start receiving session since the given ptp time
{
	st_status_t status = ST_OK;

	// validate parameters
	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}
	if (!frameBuf)
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	if (s->state != ST_SN_STATE_ON)
	{
		return ST_SN_ERR_NOT_READY;	 // logical error in the API use
	}

	StSessionLock(s);

	if ((s->cons.consType == ST21_CONS_RAW_L2_PKT) || (s->cons.consType == ST21_CONS_RAW_RTP))
	{
		//nothing but in fitire set timer and handle ptpTime
	}
	else
	{
		if (s->consState == FRAME_CURR)	 // already used ? restart?
		{
			if ((s->consBufs[FRAME_CURR].buf) && (s->consBufs[FRAME_CURR].buf != frameBuf))
			{
				s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf,
											s->vctx.fieldId);
			}
			s->consBufs[FRAME_CURR].buf = frameBuf;
			s->consBufs[FRAME_CURR].pkts = 0;
			s->consBufs[FRAME_CURR].tmstamp = 0;
		}
		else  // 1st time
		{
			s->consBufs[FRAME_PREV].buf = frameBuf;
			s->consBufs[FRAME_PREV].pkts = 0;
			s->consBufs[FRAME_PREV].tmstamp = 0;

			s->consBufs[FRAME_CURR].buf = NULL;
			s->consBufs[FRAME_CURR].pkts = 0;
			s->consBufs[FRAME_CURR].tmstamp = 0;
		}
		s->sliceOffset = s->cons.frameSize;
	}
	// so far assume full frame mode only
	s->state = ST_SN_STATE_RUN;

	StSessionUnlock(s);
	return status;
}

/**
 * Called by the consumer asynchronously to start 1st frame of video streaming
 */
st_status_t
St30ConsumerStartFrame(
	st_session_t *sn,	// IN session pointer
	uint8_t *frameBuf,	// IN 1st frame buffer for the session
	uint64_t ptpTime)	// IN if not 0 start receiving session since the given ptp time
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}
	if (!frameBuf)
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	if (s->state != ST_SN_STATE_ON)
	{
		return ST_SN_ERR_NOT_READY;	 // logical error in the API use
	}

	//s->consBuf = frameBuf;
	StSessionLock(s);

	// so far assume full frame mode only
	s->state = ST_SN_STATE_RUN;

	StSessionUnlock(s);
	return status;
}

st_status_t
St40ConsumerStartFrame(st_session_t *sn)
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	if (s->state != ST_SN_STATE_ON)
	{
		return ST_SN_ERR_NOT_READY;	 // logical error in the API use
	}

	//s->consBuf = frameBuf;
	StSessionLock(s);

	// so far assume full frame mode only
	s->state = ST_SN_STATE_RUN;

	StSessionUnlock(s);
	return ST_OK;
}

/**
 * Called by the consumer asynchronously to update video streaming
 * in case consumer is ready to get more data, it also restart streaming
 * if the consumerr callback failed due to lack of available buffer
 */
st_status_t
St21ConsumerUpdate(st_session_t *sn,	  // IN session pointer
				   uint8_t *frameBuf,	  // IN frame buffer for the session from which to restart
				   uint32_t linesOffset)  // IN offset in complete lines of the frameBuf to which
										  // consumer can get the buffer
{
	st_status_t status = ST_OK;

	// validate parameters
	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}
	if (!frameBuf)
	{
		return ST_INVALID_PARAM;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	st_device_impl_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	if ((s->cons.consType == ST21_CONS_RAW_L2_PKT) || (s->cons.consType == ST21_CONS_RAW_RTP))
	{
		s->state = ST_SN_STATE_RUN;
		return ST_OK;
	}

	StSessionLock(s);

	s->consBufs[s->consState].buf = frameBuf;
	s->prodBuf = frameBuf;

	if (linesOffset > s->sliceOffset)
	{
		s->sliceOffset = linesOffset;
		s->state = ST_SN_STATE_RUN;
	}
	else
	{
		s->state = ST_SN_STATE_NO_NEXT_SLICE;
		status = ST_BUFFER_NOT_READY;
	}

	StSessionUnlock(s);
	return status;
}

st_status_t
St30ConsumerUpdate(st_session_t *sn,
					uint8_t *audioBuf,
					uint32_t bufOffset,
					uint32_t tmstamp,
					uint64_t ptpTime)
{
	return ST_NOT_IMPLEMENTED;
}

/**
 * Called by the consumer asynchronously to stop video streaming,
 * the session will notify the consumer about completion with callback
 */
st_status_t
ConsumerStop(st_session_t *sn)
{
	st_status_t status = ST_OK;

	// validate parameters
	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;
	st_device_impl_t *d = s->dev;

	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}
	// before session destroy, should wait rx thread done
	if (rte_atomic32_read(&isRxDevToDestroy) == 0)
	{
		int i = 0;
		rte_atomic32_set(&isRxDevToDestroy, 1);
		while (d->rte_thread_core[i] != -1)
		{
			rte_eal_wait_lcore(d->rte_thread_core[i]);
			d->rte_thread_core[i] = -1;
			i++;
		}
	}

	StSessionLock(s);

	s->state = ST_SN_STATE_STOP_PENDING;

	StSessionUnlock(s);

	return status;
}

st_status_t 
St30ConsumerStop(st_session_t *sn)
{
	return ST_NOT_IMPLEMENTED;
}

/**
 * Called by the both sides to assign/bind IP addresses of the stream.
 * Upon correct scenario completes with ST_OK.
 * Shall be called twice if redundant 2022-7 path mode is used to add both addressed
 * on the ports as required respecitvely
 * path addresses and VLANs
 */
extern rte_atomic32_t isStopBkgTask;
st_status_t
StBindIpAddr(st_session_t *sn, st_addr_t *addr, uint16_t nicPort)
{
	st_status_t status = ST_OK;

	if (!addr)
	{
		return ST_INVALID_PARAM;
	}

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	s->fl[nicPort].dst.addr4.sin_family = addr->src.addr4.sin_family;
	s->fl[nicPort].dst.addr4.sin_port = addr->dst.addr4.sin_port;
	s->fl[nicPort].src.addr4.sin_port = addr->src.addr4.sin_port;
	s->fl[nicPort].src.addr4.sin_addr.s_addr = addr->src.addr4.sin_addr.s_addr;
	s->fl[nicPort].dst.addr4.sin_addr.s_addr = addr->dst.addr4.sin_addr.s_addr;
	// multicast IP addresses filtering and translation IP to the correct MAC
	if (ST_IS_IPV4_MCAST((uint8_t)addr->dst.addr4.sin_addr.s_addr))
	{
		s->fl[nicPort].dstMac[0] = 0x01;
		s->fl[nicPort].dstMac[1] = 0x00;
		s->fl[nicPort].dstMac[2] = 0x5e;
		uint32_t tmpMacChunk = (addr->dst.addr4.sin_addr.s_addr >> 8) & 0xFFFFFF7F;
		memcpy(&s->fl[nicPort].dstMac[3], &tmpMacChunk, sizeof(uint8_t) * 3);
	}
	else if (s->dev->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		int i = 0;
		char *ip = inet_ntoa(s->fl[nicPort].dst.addr4.sin_addr);

		RTE_LOG(INFO, USER1, "Start to receive destination mac on ARP for ip %s\n", ip);
		while (!SearchArpHist(s->fl[nicPort].dst.addr4.sin_addr.s_addr, s->fl[nicPort].dstMac))
		{
			if (rte_atomic32_read(&isStopMainThreadTasks) == 1)
				return ST_ARP_EXITED_WITH_NO_ARP_RESPONSE;
			rte_delay_us_sleep(ST_ARP_SEARCH_DELAY_US);
			i++;
			if (0 == (i % ST_ARP_SEARCH_CHECK_POINT)) /* Log if it hit the check ponit */
				RTE_LOG(INFO, USER1, "Still waiting ARP for ip %s, retry %d\n", ip, i);
		}
		RTE_LOG(INFO, USER1, "Get destination mac done for ip %s\n", ip);
	}

	memcpy(s->fl[nicPort].srcMac, s->dev->srcMacAddr[nicPort], ETH_ADDR_LEN);
#ifdef ST_DSCP_EXPEDITED_PRIORITY
	s->fl[nicPort].dscp = 0x2e;	 // expedited forwarding (46)
#else
	s->fl[nicPort].dscp = 0;
#endif
	s->fl[nicPort].ecn = 0;
	if (s->dev->dev.type == ST_DEV_TYPE_CONSUMER)
	{
		//// start of flow classification
		struct st_udp_flow_conf fl;

		memset(&fl, 0xff, sizeof(struct st_udp_flow_conf));

		struct rte_flow_error err;
		uint16_t rxQ = 1 + s->tid;

#ifdef DEBUG
		RTE_LOG(INFO, USER2, "Flow setup Tid %u Table: sn %u, port %u ip %x sip %x RxQ %u\n",
				s->tid, nicPort, ntohs(s->fl[nicPort].dst.addr4.sin_port),
				ntohl(s->fl[nicPort].dst.addr4.sin_addr.s_addr),
				ntohl(s->fl[nicPort].src.addr4.sin_addr.s_addr), rxQ);
#endif
		fl.dstIp = s->fl[nicPort].dst.addr4.sin_addr.s_addr;
		fl.dstPort = s->fl[nicPort].dst.addr4.sin_port;
		fl.srcIp = s->fl[nicPort].src.addr4.sin_addr.s_addr;
		fl.srcPort = s->fl[nicPort].src.addr4.sin_port;

#ifdef DEBUG
		StPrintPartFilter(" Source IP4     ", fl.srcIp, fl.srcMask, fl.srcPort, fl.srcPortMask);
		StPrintPartFilter(" Destination IP4", fl.dstIp, fl.dstMask, fl.dstPort, fl.dstPortMask);
#endif

		s->dev->flTable[s->sn.timeslot] = StSetUDPFlow(nicPort, rxQ, &fl, &err);
		if (!s->dev->flTable[s->sn.timeslot])
		{
			RTE_LOG(INFO, USER2, "Flow setup failed with error: %s\n", err.message);
			return ST_GENERAL_ERR;
		}
		//// end of flow classification
	}

#ifdef TX_RINGS_DEBUG
	RTE_LOG(DEBUG, USER1, "TX DST MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
			s->fl[nicPort].dstMac[0], s->fl[nicPort].dstMac[1], s->fl[nicPort].dstMac[2],
			s->fl[nicPort].dstMac[3], s->fl[nicPort].dstMac[4], s->fl[nicPort].dstMac[5]);
#endif
	s->etherSize = 14;	// no vlan yet

	sn_method[sn->type].init_packet_ctx(s, sn->timeslot);

	s->ofldFlags |= (ST_OFLD_HW_IP_CKSUM | ST_OFLD_HW_UDP_CKSUM);
	s->state = ST_SN_STATE_ON;

	if (s->dev->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		StUpdateSourcesList(addr->src.addr4.sin_addr.s_addr, nicPort);
	}

	return status;
}

/**
 * Called by the consumer application to join producer session
 * Producer in responce will send SDP to consumer by calling St21SendSDP
 * Consumer is expected to listen on St21ReceiveSDP() the incomming RTCP connection
 */
st_status_t
StJoinMulticastGroup(st_addr_t *addr)
{
	st_status_t status = ST_OK;

	if (!addr)
	{
		return ST_INVALID_PARAM;
	}

	for (int p = 0; p < stMainParams.numPorts; ++p)
	{
		if (ST_IS_IPV4_MCAST((uint8_t)addr->dst.addr4.sin_addr.s_addr))
		{
			status = StCreateMembershipReportV3(addr->dst.addr4.sin_addr.s_addr,
												*(uint32_t *)stMainParams.sipAddr[p],
												MODE_IS_EXCLUDE, 1, p);
			status = StSendMembershipReport(p);
		}
		else
		{
			RTE_LOG(ERR, USER1, "Can't join to the group - IP address not multicast.\n");
			status = ST_IGMP_WRONG_IP_ADDRESS;
		}
	}
	return status;
}

st_status_t
St21SetParam(st_session_t *sn, st_param_t prm, uint64_t val)
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	switch (prm)
	{
	case ST21_FRM_FIX_MODE:
	case ST21_FRM_2022_7_MODE:
		break;
	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_INVALID_PARAM;
	}

	return status;
}

st_status_t
St21GetParam(st_session_t *sn, st_param_t prm, uint64_t *val)
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	if (!val)
	{
		return ST_INVALID_PARAM;
	}

	switch (prm)
	{
	case ST21_FRM_FIX_MODE:
		*val = (uint64_t)ST21_FRM_FIX_PREV;
		break;

	case ST21_FRM_2022_7_MODE:
		*val = (uint64_t)ST21_FRM_2022_7_MODE_OFF;
		break;

	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_INVALID_PARAM;
	}

	return status;
}

/**
 * Called by the application to get SDP text in newly allocated text buffer
 * Reading SDP allows to understand session and format
 * This is complementary method to St21GetFormat and several St21GetParam
 */
st_status_t
St21GetSdp(st_session_t *sn, char *sdpBuf, uint32_t sdpBufSize)
{
	st_status_t status = ST_OK;

	status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	uint32_t depth = 0;
	char tmpBuf[2048];

	st_session_impl_t *s = (st_session_impl_t *)sn;
	char const *pacerType;
	switch (s->dev->dev.pacerType)
	{
	case ST_2110_21_TPW:
		pacerType = "2110TPW";
		break;
	case ST_2110_21_TPNL:
		pacerType = "2110TPNL";
		break;
	case ST_2110_21_TPN:
		pacerType = "2110TPN";
		break;
	default:
		pacerType = "";
		break;
	}

	switch (s->fmt.v.pixelFmt)
	{
	case ST21_PIX_FMT_RGB_8BIT:
	case ST21_PIX_FMT_BGR_8BIT:
	case ST21_PIX_FMT_YCBCR_420_8BIT:
	case ST21_PIX_FMT_YCBCR_422_8BIT:
		depth = 8;
		break;
	case ST21_PIX_FMT_RGB_10BIT_BE:
	case ST21_PIX_FMT_RGB_10BIT_LE:
	case ST21_PIX_FMT_BGR_10BIT_BE:
	case ST21_PIX_FMT_BGR_10BIT_LE:
	case ST21_PIX_FMT_YCBCR_420_10BIT_BE:
	case ST21_PIX_FMT_YCBCR_420_10BIT_LE:
	case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
	case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
		depth = 10;
		break;
	case ST21_PIX_FMT_RGB_12BIT_BE:
	case ST21_PIX_FMT_RGB_12BIT_LE:
	case ST21_PIX_FMT_BGR_12BIT_BE:
	case ST21_PIX_FMT_BGR_12BIT_LE:
	case ST21_PIX_FMT_YCBCR_420_12BIT_BE:
	case ST21_PIX_FMT_YCBCR_420_12BIT_LE:
	case ST21_PIX_FMT_YCBCR_422_12BIT_BE:
	case ST21_PIX_FMT_YCBCR_422_12BIT_LE:
		depth = 12;
		break;
	default:
		break;
	}

	sprintf(tmpBuf, "v=0\n \
		m=video %u RTP / AVP %u\n \
		c=IN IP4 %x\n \
		a=rtpmap:%u raw/%u\n \
		a=fmtp:%u sampling=YCbCr-4:2:2; width=%u; height=%u; \
		exactframerate=%u/%u; depth=%u; colorimetry=BT709;\n \
		TP=%s",
			ntohs(s->fl[0].dst.addr4.sin_port), 96, ntohl(s->fl[0].src.addr4.sin_addr.s_addr), 96,
			s->fmt.v.clockRate, 96, s->fmt.v.width, s->fmt.v.height, s->fmt.v.frmRateMul,
			s->fmt.v.frmRateDen, depth, pacerType);
	/*
	TODO: Add a = ts-refclk:ptp=IEEE1588-2008:00-0C-17-FF-FE-4B-A3_01
	to the sdp file when ptp will be ready
	*/

	if (sdpBufSize < strlen(tmpBuf))
	{
		RTE_LOG(
			INFO, USER1,
			"Provided size of output SDP buffer not enough. Please allocate more space (for %zu "
			"characters)\n",
			strlen(tmpBuf));
		return ST_NO_MEMORY;
	}

	if (!sdpBuf)
	{
		return ST_INVALID_PARAM;
	}
	else
	{
		memset(sdpBuf, 0, sdpBufSize);
		if (!sdpBuf)
			return ST_NO_MEMORY;
	}

	if (s->state < ST_SN_STATE_ON)
	{
		return ST_SN_ERR_NOT_READY;
	}
	else
	{
		snprintf(sdpBuf, sdpBufSize, "%s", tmpBuf);
	}

	return status;
}

int
StGetExtIndex(st_session_t *sn, uint8_t *addr)
{
	if (!sn)
	{
		return ST_INVALID_PARAM;
	}

	for (int i = 0; i < sn->extMem.numExtBuf; ++i)
		if (addr >= sn->extMem.addr[i] && addr <= sn->extMem.endAddr[i])
			return i;
	return -1;
}

static void
extBufFreeCb(void *extMem, void *arg)
{
	st_session_impl_t *rsn = arg;
	st_session_t *sn = arg;
	int idx = StGetExtIndex(sn, extMem);
#ifdef DEBUG
	if (extMem == NULL)
	{
		RTE_LOG(INFO, USER1, "External buffer address is invalid in extBufFreeCb\n");
		return;
	}

	if (sn == NULL)
	{
		RTE_LOG(INFO, USER1, "Session address is invalid in extBufFreeCb\n");
		return;
	}
	RTE_LOG(INFO, USER1, "Freecb:External buffer %p %p for session %d can be Freed\n", extMem,
			sn->extMem.addr[idx], sn->ssid);
#endif
	if (likely(rsn->prod.appHandle))
		rsn->prod.St21NotifyFrameDone(rsn->prod.appHandle, sn->extMem.addr[idx], 0);
	else
		StFreeFrame(sn, sn->extMem.addr[idx]);
}

uint8_t *
StAllocFrame(st_session_t *sn, uint32_t frameSize)
{
	if (StValidateSession(sn) != ST_OK)
	{
		return NULL;
	}
	uint8_t *extMem;
	struct rte_mbuf_ext_shared_info *shInfo;
	rte_iova_t bufIova;
	uint16_t sharedInfoSize = sizeof(struct rte_mbuf_ext_shared_info);

	extMem = rte_malloc("External buffer", frameSize, RTE_CACHE_LINE_SIZE);
	if (extMem == NULL)
	{
		RTE_LOG(ERR, USER1, "Failed to allocate external memory of size %d\n", frameSize);
		return NULL;
	}

	shInfo = rte_malloc("SharedInfo", sharedInfoSize, RTE_CACHE_LINE_SIZE);
	if (shInfo == NULL)
	{
		RTE_LOG(ERR, USER1, "Failed to allocate shinfo memory of size %d\n", sharedInfoSize);
		return NULL;
	}
	shInfo->free_cb = extBufFreeCb;
	shInfo->fcb_opaque = sn;
	rte_mbuf_ext_refcnt_set(shInfo, 0);

	bufIova = rte_mem_virt2iova(extMem);
	sn->extMem.shInfo[sn->extMem.numExtBuf] = shInfo;
	sn->extMem.addr[sn->extMem.numExtBuf] = extMem;
	sn->extMem.endAddr[sn->extMem.numExtBuf] = extMem + frameSize - 1;
	sn->extMem.bufIova[sn->extMem.numExtBuf] = bufIova;
	sn->extMem.numExtBuf++;
	RTE_LOG(INFO, USER1, "External buffer %p (IOVA: %lx size %d) allocated for session %d\n",
			extMem, bufIova, frameSize, sn->ssid);

	return extMem;
}

st_status_t
StFreeFrame(st_session_t *sn, uint8_t *frame)
{
	if ((!sn) || (!frame))
	{
		return ST_INVALID_PARAM;
	}

	int idx = StGetExtIndex(sn, frame);

	if (idx == -1)
	{
		RTE_LOG(ERR, USER1, "Ext memory %p does not belong to session %d\n", frame, sn->ssid);
		return ST_GENERAL_ERR;
	}

	if (rte_mbuf_ext_refcnt_read(sn->extMem.shInfo[idx]) != 0)
	{
		return ST_SN_ERR_IN_USE;
	}

	rte_free(frame);
	rte_free(sn->extMem.shInfo[idx]);
	sn->extMem.addr[idx] = sn->extMem.endAddr[idx] = NULL;
	sn->extMem.bufIova[idx] = 0;

	return ST_OK;
}
