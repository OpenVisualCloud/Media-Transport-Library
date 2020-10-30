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

#include "rvrtp_main.h"
#include "st_api_internal.h"
#include "st_flw_cls.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static inline void
RvRtpDeviceLock(rvrtp_device_t *d)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&d->lock, 1);
	} while (lock != 0);
}

static inline void
RvRtpDeviceUnlock(rvrtp_device_t *d)
{
	__sync_lock_release(&d->lock, 0);
}

st_status_t
RvRtpSendDeviceAdjustBudget(rvrtp_device_t *dev)
{
	uint32_t budget = dev->quot;

	for (uint32_t i = 0; i < dev->dev.maxSt21Sessions; i++)
	{
		if (dev->snTable[i])
		{
			dev->txPktSizeL1[i] = dev->snTable[i]->fmt.pktSize + ST_PHYS_PKT_ADD;
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
RvRtpValidateSession(st21_session_t *sn)
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

	// search if current session exist in the RX session table
	for (int i = 0; i < stRecvDevice.dev.snCount; i++)
	{
		if (&stRecvDevice.snTable[i]->sn == sn)
		{
			found = true;
			break;
		}
	}

	if (found)
	{
		return ST_OK;
	}
	else
	{
		return ST_SN_ERR_NOT_READY;
	}

	return ST_SN_ERR_NOT_READY;
}

st_status_t
RvRtpValidateDevice(st_device_t *dev)
{
	bool found = false;

	// validate device parameter
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
	else
	{
		return ST_DEV_ERR_NOT_READY;
	}

	return ST_DEV_ERR_NOT_READY;
}

st_status_t
St21GetSessionCount(st_device_t *dev, uint32_t *count)
{
	st_status_t status = ST_OK;

	if (!count)
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_device_t *d = (rvrtp_device_t *)dev;

	*count = d->snCount;

	return status;
}

/**
 * Called by the application to create a new session on NIC device
 */
st_status_t
St21CreateSession(
	st_device_t *dev,		 // IN device on which to create session
	st21_session_t *inSn,	 // IN structure
	st21_format_t *fmt,		 // IN session packet's format, optional for receiver
	st21_session_t **outSn)	 // OUT created session object w/ fields updated respectively
{
	st_status_t status = ST_OK;
	rvrtp_session_t *s;

	if ((!inSn) || (!fmt) || (!outSn))
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_device_t *d = (rvrtp_device_t *)dev;

	RvRtpDeviceLock(d);

	switch (dev->type)
	{
	case ST_DEV_TYPE_PRODUCER:
		status = RvRtpCreateTxSession(d, inSn, fmt, &s);
		break;

	case ST_DEV_TYPE_CONSUMER:
		status = RvRtpCreateRxSession(d, inSn, fmt, &s);
		break;

	default:
		status = ST_GENERAL_ERR;
	}
	if (status == ST_OK)
	{
		// update device and session out ptr
		*outSn = (st21_session_t *)s;
		__sync_fetch_and_add(&d->snTable[s->sn.timeslot], s);
		d->snCount++;
		if (dev->type == ST_DEV_TYPE_PRODUCER)
		{
			status = RvRtpSendDeviceAdjustBudget(d);
		}
	}
	RvRtpDeviceUnlock(d);
	return status;
}

/**
 * Called by the applications to get format information of the session.
 * This is complementary method to St21GetSdp and several St21GetParam
 */
st_status_t
St21GetFormat(st21_session_t *sn, st21_format_t *fmt)
{
	st_status_t status = ST_OK;

	if (!fmt)
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	*fmt = s->fmt;
	return status;
}

/**
 * Called by the application to remove the session from NIC device
 * on which it has been created
 */
st_status_t
St21DestroySession(st21_session_t *sn)
{
	st_status_t status = ST_OK;

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	rvrtp_device_t *d = s->dev;

	if ((d != &stSendDevice) && (d != &stRecvDevice))
	{
		return ST_INVALID_PARAM;
	}

	RvRtpDeviceLock(d);
	RvRtpSessionLock(s);

	__sync_fetch_and_and(&d->snTable[s->sn.timeslot], NULL);
	d->snCount--;
	if (d->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		if (s->prodBuf)
			free(s->prodBuf);
		s->prodBuf = NULL;
		if (s->prod.appHandle)
			free(s->prod.appHandle);
		s->prod.appHandle = NULL;

		status = RvRtpSendDeviceAdjustBudget(d);
		RvRtpSessionUnlock(s);
		free(s);
	}
	else
	{
		if (s->consBufs[FRAME_PREV].buf)
		{
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
										s->ctx.fieldId);
		}
		s->consBufs[FRAME_PREV].buf = NULL;
		if (s->consBufs[FRAME_CURR].buf)
		{
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf,
										s->ctx.fieldId);
		}
		s->consBufs[FRAME_CURR].buf = NULL;
		//? what with consumer app figure out later
		rte_free(s->cons.appHandle);
		s->cons.appHandle = NULL;
		RvRtpSessionUnlock(s);
		rte_free(s);
	}
	RvRtpDeviceUnlock(d);
	return status;
}

/**
 * Called by the producer to register live producer for video streaming
 */
st_status_t
St21RegisterProducer(
	st21_session_t *sn,		// IN session pointer
	st21_producer_t *prod)	// IN register callbacks to allow interaction with live producer
{
	st_status_t status = ST_OK;

	if (!prod || prod->prodType < 0x00 || prod->prodType > 0x30)
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;

	if ((!prod->St21GetNextFrameBuf) || (!prod->St21GetNextSliceOffset))
	{
		return ST_BAD_PRODUCER;
	}

	RvRtpSessionLock(s);

	s->prod = *prod;

	RvRtpSessionUnlock(s);

	return status;
}

/**
 * Called by the producer asynchronously to start each frame of video streaming
 */
st_status_t
St21ProducerStartFrame(
	st21_session_t *sn,	   // IN session pointer
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

	rvrtp_session_t *s = (rvrtp_session_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	rvrtp_device_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	RvRtpSessionLock(s);

	s->prodBuf = frameBuf;
	s->sliceOffset = linesOffset;
	s->ctx.sliceOffset = 0;

	if (linesOffset)
	{
		s->state = ST_SN_STATE_RUN;
	}
	else
	{
		s->state = ST_SN_STATE_NO_NEXT_SLICE;
		status = ST_BUFFER_NOT_READY;
	}

	RvRtpSessionUnlock(s);

	return status;
}

/**
 * Called by the producer asynchronously to update video streaming
 * in case producer has more data to send, it also restart streaming
 * if the producer callback failed due to lack of buffer with video
 */
st_status_t
St21ProducerUpdate(
    st21_session_t *sn,	  // IN session pointer
    uint8_t *frameBuf,	  // IN frame buffer for the session from which to restart
    uint32_t linesOffset) // IN offset in complete lines of the frameBuf to which
						  // producer filled the buffer
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!frameBuf))
	{
		return ST_INVALID_PARAM;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;

	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	rvrtp_device_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	RvRtpSessionLock(s);

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

	RvRtpSessionUnlock(s);

	return status;
}

/**
 * Called by the producer asynchronously to stop video streaming,
 * the session will notify the producer about completion with callback
 */
st_status_t
St21ProducerStop(st21_session_t *sn)
{
	st_status_t status = ST_OK;

	// validate parameters
	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	rvrtp_device_t *d = s->dev;

	if (d != &stSendDevice)
	{
		return ST_INVALID_PARAM;
	}

	RvRtpSessionLock(s);

	s->state = ST_SN_STATE_STOP_PENDING;

	RvRtpSessionUnlock(s);

	return status;
}

/**
 * Called by the consumerr to register live receiver for video streaming
 */
st_status_t
St21RegisterConsumer(
	st21_session_t *sn,		// IN session pointer
	st21_consumer_t *cons)	// IN register callbacks to allow interaction with live receiver
{
	st_status_t status = ST_OK;

	if (!cons || cons->consType < ST21_CONS_INVALID || cons->consType >= ST21_CONS_LAST)
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;

	switch (cons->consType)
	{
	case ST21_CONS_RAW_L2_PKT:
	case ST21_CONS_RAW_RTP:
		if ((!cons->St21RecvRtpPkt) || (cons->St21GetNextFrameBuf) || (cons->St21NotifyFrameRecv) || (cons->St21PutFrameTmstamp) ||
			(cons->St21NotifyFrameDone) || (cons->St21NotifySliceRecv) || (cons->St21NotifySliceDone) || (cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_P_FRAME:
	case ST21_CONS_I_FIELD:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv) || (!cons->St21PutFrameTmstamp) ||
			(!cons->St21NotifyFrameDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_P_FRAME_TMSTAMP:
	case ST21_CONS_I_FIELD_TMSTAMP:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv) || (!cons->St21PutFrameTmstamp) ||
			(!cons->St21NotifyFrameDone) || (!cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_I_FIELD_SLICE:
	case ST21_CONS_P_FRAME_SLICE:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv) || (!cons->St21PutFrameTmstamp) ||
			(!cons->St21NotifyFrameDone) || (!cons->St21NotifySliceRecv) || (!cons->St21NotifySliceDone))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	case ST21_CONS_I_SLICE_TMSTAMP:
	case ST21_CONS_P_SLICE_TMSTAMP:
		if ((!cons->St21GetNextFrameBuf) || (!cons->St21NotifyFrameRecv) || (!cons->St21PutFrameTmstamp) ||
			(!cons->St21NotifyFrameDone) || (!cons->St21NotifySliceRecv) || (!cons->St21NotifySliceDone) ||
			(!cons->St21PutFrameTmstamp))
		{
			return ST_BAD_CONSUMER;
		}
		break;
	default:
		return ST_INVALID_PARAM;
	}

	RvRtpSessionLock(s);

	s->cons = *cons;
	s->consState = FRAME_PREV;
	s->state = ST_SN_STATE_ON;

	if ((cons->consType == ST21_CONS_RAW_L2_PKT) || (cons->consType == ST21_CONS_RAW_RTP))
	{
		s->RecvRtpPkt = RvRtpReceivePacketCallback;
	}

	RvRtpSessionUnlock(s);
	return status;
}

/**
 * Called by the consumer asynchronously to start 1st frame of video streaming
 */
st_status_t
St21ConsumerStartFrame(
	st21_session_t *sn,	 // IN session pointer
	uint8_t *frameBuf,	 // IN 1st frame buffer for the session
	uint64_t ptpTime)	 // IN if not 0 start receiving session since the given ptp time
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!frameBuf))
	{
		return ST_INVALID_PARAM;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	rvrtp_device_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	if (s->state != ST_SN_STATE_ON)
	{
		return ST_SN_ERR_NOT_READY;	 // logical error in the API use
	}

	RvRtpSessionLock(s);

	if ((s->cons.consType == ST21_CONS_RAW_L2_PKT) ||
		(s->cons.consType == ST21_CONS_RAW_RTP))
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
											s->ctx.fieldId);
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
	
	RvRtpSessionUnlock(s);
	return status;
}

/**
 * Called by the consumer asynchronously to update video streaming
 * in case consumer is ready to get more data, it also restart streaming
 * if the consumerr callback failed due to lack of available buffer
 */
st_status_t
St21ConsumerUpdate(
    st21_session_t *sn,	  // IN session pointer
    uint8_t *frameBuf,	  // IN frame buffer for the session from which to restart
    uint32_t linesOffset) // IN offset in complete lines of the frameBuf to which
						  // consumer can get the buffer
{
	st_status_t status = ST_OK;

	// validate parameters
	if ((!sn) || (!frameBuf))
	{
		return ST_INVALID_PARAM;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	if (s->state < 1 || s->state > 4)
	{
		return ST_SN_ERR_NOT_READY;
	}

	rvrtp_device_t *d = s->dev;
	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}


	if ((s->cons.consType == ST21_CONS_RAW_L2_PKT) ||
		(s->cons.consType == ST21_CONS_RAW_RTP))
	{
		s->state = ST_SN_STATE_RUN;
		return ST_OK;
	}

	RvRtpSessionLock(s);

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

	RvRtpSessionUnlock(s);
	return status;
}

/**
 * Called by the consumer asynchronously to stop video streaming,
 * the session will notify the consumer about completion with callback
 */
st_status_t
St21ConsumerStop(st21_session_t *sn)
{
	st_status_t status = ST_OK;

	// validate parameters
	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	rvrtp_device_t *d = s->dev;

	if (d != &stRecvDevice)
	{
		return ST_INVALID_PARAM;
	}

	RvRtpSessionLock(s);

	s->state = ST_SN_STATE_STOP_PENDING;

	RvRtpSessionUnlock(s);

	return status;
}

/**
 * Called by the both sides to assign/bind IP addresses of the stream.
 * Upon correct scenario completes with ST_OK.
 * Shall be called twice if redundant 2022-7 path mode is used to add both addressed
 * on the ports as required respecitvely
 * path addresses and VLANs
 */
st_status_t
St21BindIpAddr(st21_session_t *sn, st_addr_t *addr, uint16_t nicPort)
{
	st_status_t status = ST_OK;

	if (!addr)
	{
		return ST_INVALID_PARAM;
	}

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_session_t *s = (rvrtp_session_t *)sn;

	s->fl[0].dst.addr4.sin_family = addr->src.addr4.sin_family;
	s->fl[0].dst.addr4.sin_port = addr->dst.addr4.sin_port;
	s->fl[0].src.addr4.sin_port = addr->src.addr4.sin_port;
	s->fl[0].src.addr4.sin_addr.s_addr = addr->src.addr4.sin_addr.s_addr;
	s->fl[0].dst.addr4.sin_addr.s_addr = addr->dst.addr4.sin_addr.s_addr;
	// multicast IP addresses filtering and translation IP to the correct MAC
	if ((uint8_t)addr->dst.addr4.sin_addr.s_addr >= 0xe0
		&& (uint8_t)addr->dst.addr4.sin_addr.s_addr <= 0xef)
	{
		s->fl[0].dstMac[0] = 0x01;
		s->fl[0].dstMac[1] = 0x00;
		s->fl[0].dstMac[2] = 0x5e;
		uint32_t tmpMacChunk = (addr->dst.addr4.sin_addr.s_addr >> 8) & 0xFFFFFF7F;
		memcpy(&s->fl[0].dstMac[3], &tmpMacChunk, sizeof(uint8_t) * 3);
	}
	else
	{
		memcpy(s->fl[0].dstMac, &stMainParams.macAddr, ETH_ADDR_LEN);
	}

	memcpy(s->fl[0].srcMac, &(s->dev->srcMacAddr[0][0]), ETH_ADDR_LEN);
#ifdef ST_DSCP_EXPEDITED_PRIORITY	
	s->fl[0].dscp = 0x2e;  // expedited forwarding (46)
#else
	s->fl[0].dscp = 0;
#endif
	s->fl[0].ecn = 0;
	if (s->dev->dev.type == ST_DEV_TYPE_CONSUMER)
	{
		s->fl[1] = s->fl[0];
		//// start of flow classification
		struct st_udp_flow_conf fl;

		memset(&fl, 0xff, sizeof(struct st_udp_flow_conf));

		struct rte_flow_error err;

#ifdef DEBUG
		RTE_LOG(INFO, USER2, "Flow setup Tid %u Table: sn %u, port %u ip %x sip %x\n", tid, i,
				ntohs(s->fl[0].dst.addr4.sin_port), ntohl(s->fl[0].dst.addr4.sin_addr.s_addr),
				ntohl(s->fl[0].src.addr4.sin_addr.s_addr));
#endif
		fl.dstIp = s->fl[0].dst.addr4.sin_addr.s_addr;
		fl.dstPort = s->fl[0].dst.addr4.sin_port;
		fl.srcIp = s->fl[0].src.addr4.sin_addr.s_addr;
		fl.srcPort = s->fl[0].src.addr4.sin_port;

		if (((fl.dstIp & 0xff) >= 224) && ((fl.dstIp & 0xff) <= 239))
			fl.srcMask = 0;

#ifdef DEBUG
		StPrintPartFilter(" Source IP4     ", fl.srcIp, fl.srcMask, fl.srcPort, fl.srcPortMask);
		StPrintPartFilter(" Destination IP4", fl.dstIp, fl.dstMask, fl.dstPort, fl.dstPortMask);
#endif

		s->dev->flTable[s->sn.timeslot]	= StSetUDPFlow(s->dev->dev.port[0] /*rxPortId*/, 1 + s->tid, &fl, &err);
		if (!s->dev->flTable[s->sn.timeslot])
		{
			RTE_LOG(INFO, USER2, "Flow setup failed with error: %s\n", err.message);
			return ST_GENERAL_ERR;
		}
		//// end of flow classification
	}

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "TX DST MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", s->fl[0].dstMac[0],
			s->fl[0].dstMac[1], s->fl[0].dstMac[2], s->fl[0].dstMac[3], s->fl[0].dstMac[4],
			s->fl[0].dstMac[5]);
#endif
	s->etherSize = 14;	// no vlan yet

	RvRtpInitPacketCtx(s, sn->timeslot);

	s->ofldFlags |= (ST_OFLD_HW_IP_CKSUM | ST_OFLD_HW_UDP_CKSUM);
	s->state = ST_SN_STATE_ON;

	return status;
}

/**
 * Called by the producer to listen and accept the incoming IGMP multicast reports to the
 * producer.
 */
st_status_t
St21ListenSession(st21_session_t *sn, st_addr_t *addr)
{
	st_status_t status = ST_NOT_IMPLEMENTED;

	return status;
}

/**
 * Called by the consumer application to join producer session
 * Producer in responce will send SDP to consumer by calling St21SendSDP
 * Consumer is expected to listen on St21ReceiveSDP() the incomming RTCP connection
 */
st_status_t
St21JoinSession(st21_session_t *sn, st_addr_t *addr)
{
	st_status_t status = ST_NOT_IMPLEMENTED;

	return status;
}

/**
 * Called by the consumer application to drop producer session via RTCP
 * Producer in expected to stop if all consumers dropped
 * Consumer is expected to listen on St21Accept() the incomming RTCP connection
 */
st_status_t
St21DropSession(st21_session_t *sn)
{
	st_status_t status = ST_NOT_IMPLEMENTED;

	return status;
}

st_status_t
St21SetParam(st21_session_t *sn, st_param_t prm, uint64_t val)
{
	st_status_t status = ST_OK;

	status = RvRtpValidateSession(sn);
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
St21GetParam(st21_session_t *sn, st_param_t prm, uint64_t *val)
{
	st_status_t status = ST_OK;

	status = RvRtpValidateSession(sn);
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
St21GetSdp(st21_session_t *sn, char *sdpBuf, uint32_t sdpBufSize)
{
	st_status_t status = ST_OK;

	status = RvRtpValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	uint32_t depth = 0;
	char tmpBuf[2048];

	rvrtp_session_t *s = (rvrtp_session_t *)sn;
	char const *pacerType;
	switch (s->dev->dev.pacerType)
	{
	case ST_2110_21_TPW:
		pacerType = "2110TPW";
		break;
	case ST_2110_21_TPNL:
		pacerType ="2110TPNL";
		break;
	case ST_2110_21_TPN:
		pacerType = "2110TPN";
		break;
	default:
		pacerType = "";
		break;
	}

	switch (s->fmt.pixelFmt)
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
			s->fmt.clockRate, 96, s->fmt.width, s->fmt.height, s->fmt.frmRateMul, s->fmt.frmRateDen,
			depth, pacerType);
	/*
	TODO: Add a = ts-refclk:ptp=IEEE1588-2008:00-0C-17-FF-FE-4B-A3_01
	to the sdp file when ptp will be ready
	*/

	if (sdpBufSize < strlen(tmpBuf))
	{
		RTE_LOG(INFO, USER1,
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
