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

#include "dpdk_common.h"
#include "rvrtp_main.h"
#include <math.h>

#define SCHED_ID(t) (t % stMainParams.maxSchThrds)
#define PORT_ID(t) (t / stMainParams.maxSchThrds)
extern int hwts_dynfield_offset[RTE_MAX_ETHPORTS];

static inline uint32_t
PktL1Size(uint32_t l2_size)
{
	return (uint32_t)(l2_size + ST_PHYS_PKT_ADD);
}

static inline uint32_t
PktL2Size(int32_t l1_size)
{
	return (uint32_t)(l1_size - (int32_t)ST_PHYS_PKT_ADD);
}

//#define TX_SCH_DEBUG
//#define _TX_SCH_DEBUG_
//#define _TX_RINGS_DEBUG_
//#define TX_RINGS_DEBUG
//#define ST_SCHED_TIME_PRINT
//#define TX_SCH_DEBUG_PAUSE

#ifdef TX_RINGS_DEBUG
#define RING_LOG(...) RTE_LOG(INFO, USER1, __VA_ARGS__)
#else
#define RING_LOG(...)
#endif

#ifdef TX_SCH_DEBUG_PAUSE
#define PAUSE_LOG(...) RTE_LOG(INFO, USER1, __VA_ARGS__)
#else
#define PAUSE_LOG(...)
#endif

#ifdef TX_SCH_DEBUG_PAUSE
#define PAUSE_PKT_LOG(cond, ...)                                                                   \
	if ((cond % 1000) == 0)                                                                        \
	RTE_LOG(INFO, USER1, __VA_ARGS__)
#else
#define PAUSE_PKT_LOG(cond, ...)
#endif

#ifdef TX_SCH_DEBUG
#define PKT_LOG(cond, ...)                                                                         \
	if ((cond % 1000) == 0)                                                                        \
	RTE_LOG(INFO, USER1, __VA_ARGS__)
#else
#define PKT_LOG(cond, ...)
#endif

static inline void
StSchFillGapBulk(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing,
				 uint32_t phyPktSize, struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	int32_t leftBytes = dev->txPktSizeL1[deqRing] - phyPktSize;
	if (unlikely(leftBytes > ST_MIN_PKT_L1_SZ))
	{
		// only practical case is 720p 3rd frame that is 886 bytes on L1
		// optimize for it so up to 2 such gaps were in 4 packets

		if ((leftBytes << 2) <= ST_DEFAULT_LEFT_BYTES_720P)
		{
			vec[sch->top] = pauseFrame[sch->slot];
			uint16_t pauseSize = leftBytes;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 1);
			dev->pausesTx[sch->thrdId][deqRing] += 1;
			sch->top += 1;
			sch->timeCursor -= pauseSize;
			sch->burstSize += 1;
			PAUSE_LOG("lack of big enought pkt on ring %u, submitting pause of %u, timeCusor = %u\n", deqRing,
					  pauseSize, sch->timeCursor);
		}
		else  // strange not expected behavior, huge gap
		{
			/* put pause if no packet */
			vec[sch->top] = pauseFrame[sch->slot];
			vec[sch->top + 1] = pauseFrame[sch->slot];
			vec[sch->top + 2] = pauseFrame[sch->slot];
			vec[sch->top + 3] = pauseFrame[sch->slot];
			// adjust pause size
			uint16_t pauseSize = leftBytes;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 4);
			dev->pausesTx[sch->thrdId][deqRing] += 4;
			sch->top += 4;
			sch->timeCursor -= pauseSize;
			sch->burstSize += 4;
			PAUSE_LOG("lack of big enought pkt on ring %u, submitting pause of %u, timeCusor = %u\n", deqRing,
					  pauseSize, sch->timeCursor);
		}
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
}

static inline void
StSchFillGapSingleOrDual(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing,
						 uint32_t phyPktSize, struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	int32_t leftBytes = dev->txPktSizeL1[deqRing] - phyPktSize;
	if (unlikely(leftBytes >= ST_MIN_PKT_L1_SZ))
	{
		// only practical case is 720p 3rd frame that is 886 bytes on L1
		vec[sch->top] = pauseFrame[sch->slot];
		uint16_t pauseSize = leftBytes & ~0x1;
		//vec[sch->top]->data_len = PktL2Size((int)pauseSize);
		//vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
		rte_mbuf_refcnt_update(vec[sch->top], 1);
		dev->pausesTx[sch->thrdId][deqRing]++;
		sch->top++;
		sch->timeCursor -= pauseSize;
		sch->burstSize++;
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
}

/*
 * Prepare pause frames
 */
static struct rte_mbuf *
StSchBuildPausePacket(st_main_params_t *mp, int port)
{
	if (mp == NULL)
		return NULL;

	struct rte_ether_addr srcMac;
	rte_eth_macaddr_get(mp->txPortId[port], &srcMac);

	/*Create the 802.3 PAUSE packet */
	struct rte_mbuf *pausePacket = rte_pktmbuf_alloc(mp->mbufPool);
	if (unlikely(pausePacket == NULL))
	{
		RTE_LOG(INFO, USER1, "Packet allocation for pause error\n");
		return pausePacket;
	}

	rte_pktmbuf_append(pausePacket, 1514);
	pausePacket->pkt_len = pausePacket->data_len = 1514;
	struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(pausePacket, struct rte_ether_hdr *);
	memset((char *)ethHdr, 0, 1514);

	ethHdr->ether_type = 0x0888;
	/* reserve usage for IP packet 
	 * ethHdr->ether_type = htons(0x0800);
	 */
	ethHdr->d_addr.addr_bytes[0] = 0x01;
	ethHdr->d_addr.addr_bytes[1] = 0x80;
	ethHdr->d_addr.addr_bytes[2] = 0xC2;
	ethHdr->d_addr.addr_bytes[5] = 0x01;
	rte_memcpy(&ethHdr->s_addr, &srcMac, 6);

	return pausePacket;
}

/**
 * Initialize scheduler thresholds so that they can be used for timeCursor dispatch
 */
st_status_t
StSchInitThread(tprs_scheduler_t *sch, st_device_impl_t *dev, st_main_params_t *mp, uint32_t thrdId)
{
	uint16_t schedId = SCHED_ID(thrdId);
	uint32_t leftQuot = 0;

	memset(sch, 0x0, sizeof(tprs_scheduler_t));
	// sch->currentBytes = 0; /* Current time in Ethernet bytes */
	sch->pktSize = ST_HD_422_10_SLN_L1_SZ;
	sch->thrdId = PORT_ID(thrdId);
	sch->queueId = schedId;
	sch->adjust = dev->adjust;

	// allocate resources
	sch->ringThreshHi = rte_malloc_socket("ringThreshHi", dev->maxRings * sizeof(uint32_t),
										  RTE_CACHE_LINE_SIZE, rte_socket_id());

	sch->ringThreshLo = rte_malloc_socket("ringThreshLo", dev->maxRings * sizeof(uint32_t),
										  RTE_CACHE_LINE_SIZE, rte_socket_id());

	sch->deqRingMap = rte_malloc_socket("deqRingMap", (dev->maxRings + 1) * sizeof(uint32_t),
										RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((!sch->ringThreshHi) || (!sch->ringThreshLo) || (!sch->deqRingMap))
	{
		rte_exit(ST_NO_MEMORY, "Lack of memory for TPRS scheduler structures");
	}

	memset(sch->ringThreshHi, 0, dev->maxRings * sizeof(uint32_t));
	memset(sch->ringThreshLo, 0, dev->maxRings * sizeof(uint32_t));
	memset(sch->deqRingMap, 0, (dev->maxRings + 1) * sizeof(uint32_t));

	for (uint32_t i = 0; i < dev->maxRings; i++)
	{
		leftQuot += dev->txPktSizeL1[i];
	}
	leftQuot = dev->quot - leftQuot;

	RING_LOG("Quot %u LEFT Quot %u\n", dev->quot, leftQuot);

	if (mp->maxSchThrds > 1)  // 1 or 2 max
	{
		if ((dev->maxRings & 0x1) == 1)	 // odd number of rings
		{
			sch->lastSnRing = (dev->dev.maxSt21Sessions / 2) - 1;
			sch->outOfBoundRing = dev->maxRings / 2;
			sch->lastTxRing = sch->outOfBoundRing - 1;
			if (dev->outOfBoundRing)
			{
				if (schedId == 0)
				{
					sch->lastTxRing = sch->outOfBoundRing;
				}
				// out of bound ring is on thread 1 if any
			}
			else
			{
				if (schedId == 0)
				{
					sch->lastTxRing = sch->outOfBoundRing;
				}
				else
				{
					sch->outOfBoundRing--;
				}
			}
		}
		else  // even number of rings
		{
			sch->outOfBoundRing = dev->maxRings / 2;
			sch->lastSnRing = (dev->dev.maxSt21Sessions / 2) - 1;
			sch->lastTxRing = sch->outOfBoundRing - 1;
			if (dev->outOfBoundRing)
			{
				if (schedId == 0)
				{
					// only last thread has OutOfBound ring
					// so disable on 1st thread
					sch->outOfBoundRing--;
				}
			}
			else
			{
				// no OOB ring so disable by making them
				// equal to lastTxRing
				sch->outOfBoundRing--;
			}
		}
		uint32_t quot = 0;
		for (uint32_t i = 0; i <= sch->lastTxRing; i++)
		{
			uint32_t devTxQueue = i * mp->maxSchThrds + schedId;
			quot += dev->txPktSizeL1[devTxQueue];
		}
		sch->quot = quot;
		if (schedId == 0)
		{
			sch->remaind = 0;
		}
		else
		{
			sch->quot += leftQuot;
			sch->remaind = (uint64_t)dev->remaind;
			sch->deqRingMap[sch->outOfBoundRing] = dev->maxRings;
		}
	}
	else
	{
		sch->lastTxRing = dev->maxRings - 1;
		if (dev->outOfBoundRing)
		{
			sch->outOfBoundRing = dev->maxRings;
			sch->deqRingMap[sch->outOfBoundRing] = dev->maxRings;
		}
		else
			sch->outOfBoundRing = dev->maxRings - 1;
		sch->quot = (uint64_t)dev->quot;
		sch->remaind = (uint64_t)dev->remaind;
		sch->lastSnRing = dev->dev.maxSt21Sessions - 1;
	}
	sch->minPktSize = ST_MIN_PKT_SIZE + ST_PHYS_PKT_ADD;

	uint64_t quot = sch->quot;

	for (uint32_t i = 0; i <= sch->lastSnRing; i++)
	{
		sch->ringThreshHi[i] = quot + sch->minPktSize;
		uint32_t devTxQueue = i * mp->maxSchThrds + schedId;
		quot -= dev->txPktSizeL1[devTxQueue];
		sch->ringThreshLo[i] = quot + sch->minPktSize;
		sch->deqRingMap[i] = devTxQueue;
	}

	for (uint32_t i = sch->lastSnRing + 1; i <= sch->lastTxRing; i++)
	{
		sch->ringThreshHi[i] = quot + sch->minPktSize;
		uint32_t devTxQueue = i * mp->maxSchThrds + schedId;
		quot -= dev->txPktSizeL1[devTxQueue];
		sch->ringThreshLo[i] = quot + sch->minPktSize;
		sch->deqRingMap[i] = devTxQueue;
	}

	for (uint32_t i = 0; i <= sch->lastTxRing; i++)
	{
		RING_LOG("THRD %u ThresholdHi: %u ThresholdLo: %u ring: %u\n", thrdId, sch->ringThreshHi[i],
				 sch->ringThreshLo[i], sch->deqRingMap[i]);
	}
	return ST_OK;
}

/**
 * Dispatch timeCursor into timeslot (derived from TRoffset) of the ring session
 */
static inline uint32_t
StSchDispatchTimeCursor(tprs_scheduler_t *sch, st_device_impl_t *dev)
{
	if ((sch->ring == sch->outOfBoundRing) || (sch->timeCursor <= 0))
	{
		sch->ring = 0;
		sch->pktSize = dev->txPktSizeL1[sch->deqRingMap[sch->ring]];
		return sch->deqRingMap[sch->ring];
	}

	for (uint32_t i = sch->ring + 1; i <= sch->lastTxRing; i++)
	{
		if ((sch->timeCursor <= sch->ringThreshHi[i]) && (sch->timeCursor > sch->ringThreshLo[i]))
		{
			sch->ring = i;
			sch->pktSize = dev->txPktSizeL1[sch->deqRingMap[sch->ring]];
			return sch->deqRingMap[sch->ring];
		}
	}
	PAUSE_LOG("StSchDispatchTimeCursor: OOBR %u bytes: %u\n", sch->outOfBoundRing, sch->timeCursor);
	sch->ring = sch->outOfBoundRing;
	sch->pktSize = sch->timeCursor;
	return sch->deqRingMap[sch->outOfBoundRing];
}

uint16_t
StSchAlignToEpoch(uint16_t port_id, uint16_t queue, struct rte_mbuf *pkts[], uint16_t pktsCount,
				  st_device_impl_t *dev)
{
#define ST_SCHED_TMSTAMP_TOLERANCE 100
	uint64_t const t = StPtpGetTime() + ST_SCHED_TMSTAMP_TOLERANCE;

	for (uint32_t i = 0; i < pktsCount; ++i)
	{
		uint64_t now = rte_rdtsc();
		uint64_t timeStamp = (hwts_dynfield_offset[port_id] > 0) ?
					*RTE_MBUF_DYNFIELD(pkts[i], hwts_dynfield_offset[port_id], rte_mbuf_timestamp_t *) : now;

		if (timeStamp > t)
		{
			if (timeStamp > (t + 34 * MEGA))  // 34ms limit
			{
				//RTE_LOG(INFO, USER1, "Wrong Time %lu to wait = %lu\n", pkts[i]->timestamp, pkts[i]->timestamp - t);
				StMbufSetTimestamp(pkts[i], 0);
			}
			return i;
		}
	}
	return pktsCount;
}

static inline uint32_t
StSchFillPacket(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing, uint32_t i,
				uint32_t vectSize, struct rte_mbuf **vecTemp, struct rte_mbuf **vec,
				uint32_t bulkNum)
{
	uint32_t phyPktSize = 0;

	for (uint32_t idx = 0; idx < bulkNum; idx++)
	{ /* Fill vec as vectSize */
		vec[i] = vecTemp[idx];
		i += vectSize;
		phyPktSize += vecTemp[idx]->pkt_len;
	}

	dev->packetsTx[sch->thrdId][deqRing] += bulkNum;
	sch->burstSize += bulkNum;
	phyPktSize = ST_PHYS_PKT_ADD + (phyPktSize / bulkNum);
	sch->timeCursor -= phyPktSize;

	PKT_LOG(dev->packetsTx[sch->thrdId][deqRing], "packet %u ring %u of %llu\n",
			dev->txPktSizeL1[deqRing], deqRing, (U64)dev->packetsTx[sch->thrdId][deqRing]);
	return phyPktSize;
}

static inline void
StSchFillPause(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing, uint32_t i,
			   uint32_t vectSize, struct rte_mbuf **pauseFrame, struct rte_mbuf **vec,
			   uint32_t bulkNum)
{
	PAUSE_LOG("lack of packet on ring %u, submitting pause of %u %u, cursor = %d\n", deqRing,
			  dev->txPktSizeL1[deqRing], sch->pktSize & ~0x1, sch->timeCursor);
	uint16_t pauseSize = sch->pktSize & ~0x1;
	int curTimeCursor = sch->timeCursor + sch->adjust;

	if (curTimeCursor <= ST_MIN_PKT_SIZE) {
		pauseSize = 50; /* wait for be filtered */
	}

	for (uint32_t idx = 0; idx < bulkNum; idx++)
	{
		vec[i + idx * vectSize] = pauseFrame[sch->slot];
	}
	// adjust pause size
	vec[i]->data_len = PktL2Size((int)pauseSize);
	vec[i]->pkt_len = PktL2Size((int)pauseSize);
	rte_mbuf_refcnt_update(pauseFrame[sch->slot],
						   bulkNum); /* bulkNum vec point to same pauseFrame */
	dev->pausesTx[sch->thrdId][deqRing] += bulkNum;

	sch->timeCursor -= pauseSize;
	sch->burstSize += bulkNum;
	sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
}

static inline void
StSchFillGap(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing, uint32_t phyPktSize,
			 struct rte_mbuf **pauseFrame, struct rte_mbuf **vec, uint32_t bulkNum)
{
	switch (bulkNum)
	{
	case 4:
		StSchFillGapBulk(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
		break;
	case 2:
	case 1:
		StSchFillGapSingleOrDual(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
		break;
	default:
		RTE_LOG(ERR, USER1, "%s, invalid bulkNum %u", __func__, bulkNum);
		break;
	}
}

static inline void
StSchFillOob(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing,
			 struct rte_mbuf **pauseFrame, struct rte_mbuf **vec, uint32_t bulkNum)
{
	int curTimeCursor = sch->timeCursor + sch->adjust;

	if (curTimeCursor >= ST_MIN_PKT_SIZE)
	{
		if ((curTimeCursor / bulkNum) <= ST_DEFAULT_PKT_L1_SZ)
		{
			vec[sch->top] = pauseFrame[sch->slot];
			uint16_t pauseSize = curTimeCursor / bulkNum;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 1);
			sch->burstSize += 1;
			dev->pausesTx[sch->thrdId][deqRing] += 1;
			sch->top += 1;
		}
		else
		{
			int pkt_num = curTimeCursor / ST_DEFAULT_PKT_L1_SZ + 1;
			for (uint32_t idx = 0; idx < pkt_num; idx++)
			{
				vec[sch->top + idx] = pauseFrame[sch->slot];
			}
			uint16_t pauseSize = ST_DEFAULT_PKT_L1_SZ;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], pkt_num);
			sch->burstSize += pkt_num;
			dev->pausesTx[sch->thrdId][deqRing] += pkt_num;
			sch->top += pkt_num;
		}
		sch->timeCursor = 0;
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
	else
	{
		sch->timeCursor = 0;
	}
	return;
}

static inline void
StSchPacketOrPause(tprs_scheduler_t *sch, st_device_impl_t *dev, uint32_t deqRing, uint32_t i,
				   uint32_t vectSize, uint32_t deq, struct rte_mbuf **vecTemp,
				   struct rte_mbuf **pauseFrame, struct rte_mbuf **vec, uint32_t bulkNum)
{
	/* put packets */
	dev->packetsTx[sch->thrdId][deqRing] += deq;

	/* put pause if no packet */
	uint32_t pauseCount = bulkNum - deq;
	uint32_t deqPauseIt = 0;
	while (deqPauseIt < pauseCount)
	{
		vecTemp[deq + deqPauseIt] = pauseFrame[sch->slot];
		deqPauseIt++;
		dev->pausesTx[sch->thrdId][deqRing]++;
	}

	if (deqPauseIt)
	{ /* adjust pause size */
		uint32_t phyPktSize = 0;
		for (uint32_t idx = 0; idx < deq; idx++)
		{
			phyPktSize += vecTemp[idx]->pkt_len;
		}
		uint16_t leftBytes = (dev->txPktSizeL1[deqRing] - ST_PHYS_PKT_ADD) * bulkNum - phyPktSize;
		uint16_t pauseSize = (leftBytes+pauseCount-1)/pauseCount;
		pauseFrame[sch->slot]->data_len = pauseSize;
		pauseFrame[sch->slot]->pkt_len = pauseSize;
		rte_mbuf_refcnt_update(pauseFrame[sch->slot], deqPauseIt);
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}

	uint32_t phyPktSize = 0;
	for (uint32_t idx = 0; idx < bulkNum; idx++)
	{
		vec[i + idx * vectSize] = vecTemp[idx];
		phyPktSize += vecTemp[idx]->pkt_len;
	}
	sch->burstSize += bulkNum;

	phyPktSize = ST_PHYS_PKT_ADD + (phyPktSize / bulkNum);
	sch->timeCursor -= phyPktSize;

}

static uint16_t
StSchPreCheckPkts(struct rte_mbuf **pkts, uint16_t nb_pkts)
{
	if (unlikely(nb_pkts == 0))
		return nb_pkts;

	int i, j, k;
	struct rte_mbuf *ptr;
	struct rte_mbuf *replace[nb_pkts];

	for (i = 0, j = 0, k = 0; i < nb_pkts; i++)
	{
		ptr = pkts[i];
		if (unlikely((ptr == NULL) || (ptr->pkt_len < 60) || (ptr->nb_segs > 2)
					 || (ptr->pkt_len > 1514)))
		{
			RTE_LOG(INFO, USER1, "%s, invalid packet on %d, len %d segs %d\n", __func__,
				i, ptr->pkt_len, ptr->nb_segs);
			replace[k++] = ptr;
			continue;
		}
		pkts[j++] = ptr;
	}

	if (unlikely(k))
	{
		for (i = 0; i < k; i++)
			if (replace[i])
				rte_pktmbuf_free(replace[i]);
	}

	return j;
}

static inline void
StSchtTxBurst(uint16_t port_id, uint16_t queue_id, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	uint32_t sent = 0;
	int32_t actual_nb_pkts = StSchPreCheckPkts(tx_pkts, nb_pkts);

	/* Send this vector with busy looping */
	while (sent < actual_nb_pkts)
	{
		sent += rte_eth_tx_burst(port_id, queue_id, &tx_pkts[sent], actual_nb_pkts - sent);
	}
}

static int
LcoreMainTransmitterPause(void *args)
{
	RING_LOG("TRANSMITTER RUNNED ON LCORE %d SOCKET %d\n", rte_lcore_id(),
			 rte_lcore_to_socket_id(rte_lcore_id()));

	st_main_params_t *mp = &stMainParams;
	st_device_impl_t *dev = &stSendDevice;
	lcore_transmitter_args_t *lt_args = args;
	uint32_t threadId = lt_args->threadId;
	uint32_t bulkNum = lt_args->bulkNum;
	uint32_t schedId = SCHED_ID(threadId);
	uint16_t txPortId = PORT_ID(threadId);
	tprs_scheduler_t *sch = rte_malloc_socket("tprsSch", sizeof(tprs_scheduler_t),
											  RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((schedId > mp->maxSchThrds) || !sch)
		rte_exit(ST_NO_MEMORY, "Transmitter init memory error\n");

	StSchInitThread(sch, dev, mp, threadId);

	uint32_t vectSize = sch->lastTxRing + 1;
	uint32_t vectSizeNPauses = vectSize;
	if (sch->lastTxRing != sch->outOfBoundRing)
	{
		vectSizeNPauses++;
	}
	const uint32_t pktVecSize = bulkNum * 2 * vectSizeNPauses;
	struct rte_mbuf *vec[pktVecSize]; /* one for pause, one for packet */
	struct rte_mbuf *vecTemp[bulkNum];
	RING_LOG("TRANSMITTER: VECTOR SIZE %u, threadId %u, bulkNum %u\n", vectSize, threadId, bulkNum);

	if (bulkNum != 1 && bulkNum != 2 && bulkNum != 4) /* currently it only support 1, 2 or 4 */
		rte_exit(ST_INVALID_PARAM, "Invalid Transmitter bulkNum\n");

	struct rte_mbuf *pauseFrame[MAX_PAUSE_FRAMES];

	/*Create the 802.3 PAUSE frames*/
	for (uint32_t i = 0; i < MAX_PAUSE_FRAMES; i++)
	{
		pauseFrame[i] = StSchBuildPausePacket(mp, txPortId);
		if (!pauseFrame[i])
			rte_exit(ST_NO_MEMORY, "ST SCHEDULER pause allocation problem\n");
	}

	RING_LOG("ST SCHEDULER on port named %s\n", mp->outPortName[txPortId]);
	int rv = rte_eth_dev_get_port_by_name(mp->outPortName[txPortId], &txPortId);
	if (rv < 0)
	{
		rte_exit(ST_INVALID_PARAM, "TX Port : %s not found\n", mp->outPortName[txPortId]);
	}
	RING_LOG("ST SCHEDULER on port %u\n", txPortId);
#ifdef ST_SCHED_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	/* TODO
     * Before silicon feature is ready, disable timestamp check
     */
	//rte_eth_add_tx_callback(txPortId, sch->queueId, (rte_tx_callback_fn)StSchAlignToEpoch, dev);

	// Firstly synchronize the moment both schedulers are ready
	RVRTP_BARRIER_SYNC(mp->schedStart, threadId, mp->maxSchThrds * mp->numPorts);

	// Since all ready now can release ring enqueue threads
	RVRTP_SEMAPHORE_GIVE(mp->ringStart, 1);

	int asn_cnt = mp->sn30Count; /* audio session count */
	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef _TX_SCH_DEBUG_
		uint32_t cnt = 0;
		memset(vec, 0x0, sizeof(struct rte_mbuf *) * pktVecSize);
#endif
		while ((!mp->schedStart) && (rte_atomic32_read(&isTxDevToDestroy) == 0))
		{
#ifdef _TX_SCH_DEBUG_
			cnt++;
			if ((cnt % 2000) == 0)
			{
				RTE_LOG(INFO, USER1, "Waiting under starvation thread Id of %u...\n", threadId);
			}
#endif	// TX_SCH_DEBUG

			if (schedId == 0 && asn_cnt != 0)
			{
				struct rte_mbuf *mbuf[asn_cnt + 1];
				int rv = rte_ring_sc_dequeue_bulk(dev->txRing[txPortId][dev->dev.maxSt21Sessions],
												  (void **)mbuf, asn_cnt, NULL);
				if (rv == 0)
					continue;

				int32_t sent = 0;
				int32_t actualSent = StSchPreCheckPkts(&mbuf[0], rv);
				while (sent < actualSent)
				{
					sent
						+= rte_eth_tx_burst(txPortId, sch->queueId, &mbuf[sent], actualSent - sent);
				}
				dev->packetsTx[txPortId][dev->dev.maxSt21Sessions] += actualSent;
			}
		}

		sch->slot = 0;
		uint32_t eos = 0;
		sch->timeCursor = 0;

		while (!eos && (rte_atomic32_read(&isTxDevToDestroy) == 0))
		{
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles0 = rte_get_tsc_cycles();
#endif
			sch->burstSize = 0;
			sch->top = bulkNum * vectSize;

			for (uint32_t i = 0; i < vectSizeNPauses; i++)
			{
				uint32_t deqRing = StSchDispatchTimeCursor(sch, dev);
				if (sch->ring == 0 && deqRing == 0)
				{
					while ((mp->interSchedStart[txPortId] == 1) && (mp->maxSchThrds > 1) && (rte_atomic32_read(&isTxDevToDestroy) == 0))
					{
						__sync_synchronize();
					}
					uint32_t rv = rte_ring_sc_dequeue_bulk(dev->txRing[txPortId][deqRing],
														   (void **)vecTemp, bulkNum, NULL);
					if (unlikely(rv == 0))
					{
						__sync_synchronize();
						eos = 1;
						break;
					}
					else
					{

						if (mp->maxSchThrds > 1)
						{
							__sync_synchronize();
							mp->interSchedStart[txPortId] = 1;
						}
						/* initialize from available budget*/
						sch->timeCursor = sch->quot;
						uint32_t phyPktSize = StSchFillPacket(sch, dev, deqRing, i, vectSize,
															  vecTemp, vec, bulkNum);

						StSchFillGap(sch, dev, deqRing, phyPktSize, pauseFrame, vec, bulkNum);
					}
				}
				else if (sch->ring <= sch->lastSnRing || (deqRing != dev->dev.maxSt21Sessions && sch->ring <= sch->lastTxRing))
				{
					if (schedId != 0 && sch->ring == 0)
					{
						while ((mp->interSchedStart[txPortId] == 0) && (rte_atomic32_read(&isTxDevToDestroy) == 0))
						{
							__sync_synchronize();
						}
						__sync_lock_test_and_set(&mp->interSchedStart[txPortId], 0);
						/* initialize from available budget*/
						sch->timeCursor = sch->quot;
					}
					uint32_t rv = rte_ring_sc_dequeue_bulk(dev->txRing[txPortId][deqRing],
														   (void **)vecTemp, bulkNum, NULL);

					if (unlikely(rv == 0))
					{
						StSchFillPause(sch, dev, deqRing, i, vectSize, pauseFrame, vec, bulkNum);
					}
					else
					{
						uint32_t phyPktSize = StSchFillPacket(sch, dev, deqRing, i, vectSize,
															  vecTemp, vec, bulkNum);

						StSchFillGap(sch, dev, deqRing, phyPktSize, pauseFrame, vec, bulkNum);
					}
				}
				else if (deqRing == dev->dev.maxSt21Sessions)
				{
					// kni and audio, anc
					uint32_t deq = 0;
					if (asn_cnt)
					{
						struct rte_mbuf *mbuf[asn_cnt];
						int rv = rte_ring_sc_dequeue_bulk(dev->txRing[txPortId][deqRing],
														  (void **)mbuf, asn_cnt, NULL);
						if (rv)
						{
							int32_t sent = 0;
							int32_t actualSent = StSchPreCheckPkts(&mbuf[0], rv);
							/* Now send this mbuf and keep trying */
							while (sent < actualSent)
							{
								sent += rte_eth_tx_burst(txPortId, sch->queueId, &mbuf[sent],
														 actualSent - sent);
							}
							dev->packetsTx[txPortId][deqRing] += actualSent;
						}
					}
					while (deq < bulkNum)
					{
						int rv = rte_ring_sc_dequeue(dev->txRing[txPortId][deqRing],
													 (void **)&vecTemp[deq]);
						if (unlikely(rv < 0))
						{
							break;
						}
						deq++;
					}
					if (deq < bulkNum)
					{
						// put packets or pauses
						StSchPacketOrPause(sch, dev, deqRing, i, vectSize, deq, vecTemp, pauseFrame,
										   vec, bulkNum);
					}
					else
					{
						// have bulkNum packets of same size
						uint32_t phyPktSize = StSchFillPacket(sch, dev, deqRing, i, vectSize,
															  vecTemp, vec, bulkNum);
						StSchFillGap(sch, dev, deqRing, phyPktSize, pauseFrame, vec, bulkNum);
					}
				}
				else if (sch->ring == sch->outOfBoundRing)
				{
					/* send pause here always */
					PAUSE_PKT_LOG(dev->pausesTx[sch->thrdId][deqRing],
								  "Out of bound ring %u, submitting pause of %u\n", sch->ring,
								  sch->pktSize);
#ifdef TX_SCH_DEBUG
					if (i != vectSize)
					{
						rte_exit(ST_GENERAL_ERR,
								 "Invalid indices %u and timeCursor for thread %u: %u!\n", i,
								 threadId, sch->timeCursor);
					}
#endif
					StSchFillOob(sch, dev, deqRing, pauseFrame, vec, bulkNum);
					break;	// for loop
				}
				else
				{
					rte_exit(ST_GENERAL_ERR, "Invalid timeCursor for thread %u: %u!\n", threadId,
							 sch->timeCursor);
				}
			}
			if (eos)
			{
				break;
			}

#define TX_PREFETCH 4
			for (int index = 0; index < (sch->burstSize / TX_PREFETCH) * TX_PREFETCH;
				 index += TX_PREFETCH)
			{
				rte_prefetch_non_temporal(vec[index]);
				rte_prefetch_non_temporal(vec[index]->next ? vec[index]->next : NULL);
				rte_prefetch_non_temporal(vec[index + 1]);
				rte_prefetch_non_temporal(vec[index + 1]->next ? vec[index + 1]->next : NULL);
				rte_prefetch_non_temporal(vec[index + 2]);
				rte_prefetch_non_temporal(vec[index + 2]->next ? vec[index + 2]->next : NULL);
				rte_prefetch_non_temporal(vec[index + 3]);
				rte_prefetch_non_temporal(vec[index + 3]->next ? vec[index + 3]->next : NULL);
			}

			/* precheck the packet buffer*/
			int32_t sent = 0;
			int16_t actualSent = StSchPreCheckPkts(&vec[0], sch->burstSize);

			/* Now send this vector and keep trying */
			while (sent < actualSent)
			{
				sent += rte_eth_tx_burst(txPortId, sch->queueId, &vec[sent], actualSent - sent);
			}
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles1 = rte_get_tsc_cycles();
			if ((dev->packetsTx[sch->thrdId][0] % 1000) == 0)
				RTE_LOG(INFO, USER1, "Time elapsed %llu ratio %llu pktTime %llu\n",
						(U64)(cycles1 - cycles0), (U64)ratioClk,
						(U64)(cycles1 - cycles0) / (U64)sch->burstSize);
#endif
		}
		if (!threadId)
		{
			__sync_lock_test_and_set(&mp->schedStart, 0);
		}
	}
	return 0;
}

static int
LcoreMainTransmitterTscPacing(void *args)
{
	st_main_params_t *mp = &stMainParams;
	st_device_impl_t *dev = &stSendDevice;
	lcore_transmitter_args_t *lt_args = args;
	uint32_t threadId = lt_args->threadId;
	uint32_t schedId = SCHED_ID(threadId);
	uint16_t txPortId = PORT_ID(threadId);

	tprs_scheduler_t *sch = rte_malloc_socket("tprsSch", sizeof(tprs_scheduler_t), RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((schedId > mp->maxSchThrds) || !sch)
		rte_exit(ST_NO_MEMORY, "%s, Transmitter init memory error\n", __func__);

	StSchInitThread(sch, dev, mp, threadId);
	uint32_t max_ring = mp->snCount;
	uint32_t start_ring = schedId;

	// Firstly synchronize the moment both schedulers are ready
	RVRTP_BARRIER_SYNC(mp->schedStart, threadId, mp->maxSchThrds * mp->numPorts);

	RTE_LOG(INFO, USER2, "%s(%d), rte_lcore_id %d\n", __func__, threadId, rte_lcore_id());
	struct rte_mbuf *mbuf[1];
	struct rte_mbuf *inbuf[max_ring];
	for (uint32_t ring = 0; ring < max_ring; ring++)
	{
		inbuf[ring] = NULL;
	}
	int rv;
	uint64_t timestamp;
	uint32_t burst_size = 1;
	uint32_t sn_per_thread = mp->snCount / mp->maxSchThrds;
	if (sn_per_thread > 12)
	{
		burst_size = 4;
	}
	else if (sn_per_thread > 8)
	{
		burst_size = 2;
	}
	uint32_t burst_idx = 0;
	struct rte_mbuf *burst_buf[burst_size];
	RTE_LOG(INFO, USER2, "%s(%d), max_ring %d audio ring %d burst_size %d\n", __func__, threadId, max_ring, dev->dev.maxSt21Sessions, burst_size);

	// Since all ready now can release ring enqueue threads
	RVRTP_SEMAPHORE_GIVE(mp->ringStart, 1);

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		for (uint32_t ring = start_ring; ring < max_ring; ring = ring + mp->maxSchThrds)
		{ /* ring for video session */
			if (inbuf[ring])
			{ /* First checking exist infight buffer */
				timestamp = StMbufGetTimestamp(inbuf[ring]);
				if (timestamp > StGetTscTimeNano())
				{ /* sch time not reach */
					continue;
				}
				else
				{
					burst_buf[burst_idx] = inbuf[ring]; /* put it in burst_buf */
					burst_idx++;
					inbuf[ring] = NULL;
					dev->packetsTx[txPortId][ring] += 1;
				}
			}
			else
			{ /* dequeue from ring */
				rv = rte_ring_sc_dequeue(dev->txRing[txPortId][ring], (void **)&mbuf[0]);
				if (unlikely(rv < 0))
				{
					continue;
				}
				timestamp = StMbufGetTimestamp(mbuf[0]);

				uint64_t cur = StGetTscTimeNano();
				if (timestamp < cur)
				{ /* time already reach, not expected */
					uint64_t delta = cur - timestamp;

					dev->pacingDeltaCnt[txPortId][ring] += 1;
					dev->pacingDeltaSum[txPortId][ring] += delta;
					if (delta > dev->pacingDeltaMax[txPortId][ring])
					{
						dev->pacingDeltaMax[txPortId][ring] = delta;
					}

					burst_buf[burst_idx] = mbuf[0]; /* put it in burst_buf */
					burst_idx++;
					dev->packetsTx[txPortId][ring] += 1;
				}
				else
				{ /* sch time not reach, put it in inflight buf */
					inbuf[ring] = mbuf[0];
					continue;
				}
			}

			if (burst_idx >= burst_size)
			{ /* Sending burst pkts now */
				StSchtTxBurst(txPortId, sch->queueId, &burst_buf[0], burst_size);
				burst_idx = 0;
			}
		}

		if (schedId == 0)
		{
			uint32_t ring
				= dev->dev.maxSt21Sessions; /* audio/anc/kni ring without timestamp check */
			rv = rte_ring_sc_dequeue(dev->txRing[txPortId][ring], (void **)&mbuf[0]);
			if (unlikely(rv < 0))
			{
				continue;
			}

			/* Sending this pkt directly */
			StSchtTxBurst(txPortId, sch->queueId, &mbuf[0], 1);
			dev->packetsTx[txPortId][ring] += 1;
		}
	}

	return 0;
}

static struct rte_mbuf *
StSchBuildPadPacket(st_main_params_t *mp, int port, uint16_t ether_type, uint16_t len)
{
	struct rte_ether_addr srcMac;
	struct rte_mbuf *padPacket;
	struct rte_ether_hdr *ethHdr;

	rte_eth_macaddr_get(mp->txPortId[port], &srcMac);
	padPacket = rte_pktmbuf_alloc(mp->mbufPool);
	if (unlikely(padPacket == NULL))
	{
		RTE_LOG(INFO, USER1, "%s, packet allocation for pad error\n", __func__);
		return NULL;
	}

	rte_pktmbuf_append(padPacket, len);
	padPacket->data_len = len;
	padPacket->pkt_len = len;

	ethHdr = rte_pktmbuf_mtod(padPacket, struct rte_ether_hdr *);
	memset((char *)ethHdr, 0, len);
	ethHdr->ether_type = htons(ether_type);
	ethHdr->d_addr.addr_bytes[0] = 0x01;
	ethHdr->d_addr.addr_bytes[1] = 0x80;
	ethHdr->d_addr.addr_bytes[2] = 0xC2;
	ethHdr->d_addr.addr_bytes[5] = 0x01;
	rte_memcpy(&ethHdr->s_addr, &srcMac, 6);

	return padPacket;
}

static int
LcoreMainTransmitterNicRlPacing(void *args)
{
	st_main_params_t *mp = &stMainParams;
	st_device_impl_t *dev = &stSendDevice;
	lcore_transmitter_args_t *lt_args = args;
	uint32_t threadId = lt_args->threadId;
	uint32_t bulkNum = lt_args->bulkNum;
	uint32_t schedId = SCHED_ID(threadId);
	uint16_t txPortId = PORT_ID(threadId);

	tprs_scheduler_t *sch = rte_malloc_socket("tprsSch", sizeof(tprs_scheduler_t), RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((schedId > mp->maxSchThrds) || !sch)
		rte_exit(ST_NO_MEMORY, "%s, Transmitter init memory error\n", __func__);

	StSchInitThread(sch, dev, mp, threadId);
	uint32_t max_ring = mp->snCount;
	uint32_t start_ring = schedId;
	unsigned int rv;
	struct rte_mbuf *mbuf[bulkNum];
	struct rte_mbuf *padPacket[1];
	st_session_impl_t *s;
	rvrtp_pacing_t *pacing;
	uint32_t pktIdx;
	st21_format_t *vfmt = StGetVfmtByRing(0);

	padPacket[0] = StSchBuildPadPacket(mp, txPortId, 0x0800, vfmt->pktSize);
	if (!padPacket[0])
		rte_exit(ST_NO_MEMORY, "%s, pad packet fail\n", __func__);

	// Firstly synchronize the moment both schedulers are ready
	RVRTP_BARRIER_SYNC(mp->schedStart, threadId, mp->maxSchThrds * mp->numPorts);

	RTE_LOG(INFO, USER2, "%s(%d), rte_lcore_id %d\n", __func__, threadId, rte_lcore_id());
	RTE_LOG(INFO, USER2, "%s(%d), max_ring %d lastTxRing %d bulkNum %d\n", __func__,
			threadId, max_ring, sch->lastTxRing, bulkNum);

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(5 * 1000 * 1000);

	if (schedId == 0 && txPortId == 0) /* todo: add train for redunant ? */
	{ /* training padIntervalForRL */
		int padPkts;
		int loopCnt = 100;
		double pktsPerSecSum = 0;

		padPkts = 2048 * 10;
		for (int i = 0; i < padPkts; i++) /* warm stage to consume all nix tx buf */
		{
			rte_mbuf_refcnt_update(padPacket[0], 1);
			StSchtTxBurst(txPortId, 0, &padPacket[0], 1);
		}

		padPkts = 2048 * 10;
		for (int loop = 0; loop < loopCnt; loop++) /* training stage */
		{
			uint64_t startTsc = StGetTscTimeNano();
			for (int i = 0; i < padPkts; i++)
			{
				rte_mbuf_refcnt_update(padPacket[0], 1);
				StSchtTxBurst(txPortId, 0, &padPacket[0], 1);
			}
			uint64_t endTsc = StGetTscTimeNano();
			double timeSec = (double)(endTsc - startTsc) / NS_PER_S;
			pktsPerSecSum += padPkts / timeSec;
		}
		double pktsPerSec = pktsPerSecSum / loopCnt;

		/* parse the rlPadsInterval */
		double pktsPerFrame = pktsPerSec * vfmt->frmRateDen / vfmt->frmRateMul;
		pktsPerFrame = pktsPerFrame * vfmt->height / vfmt->totalLines; /* adjust as tr offset */
		if (pktsPerFrame < vfmt->pktsInFrame)
		{
			rte_exit(ST_SN_ERR_RATE_NO_FIT, "%s, error caculated pktsPerFrame %f(%d) for RL\n", __func__,
				pktsPerFrame, vfmt->pktsInFrame);
		}
		float rlPadsInterval = (float)vfmt->pktsInFrame / (pktsPerFrame - vfmt->pktsInFrame);
		StSetRlPadsInterval(rlPadsInterval);
		RTE_LOG(INFO, USER2, "%s(%d), RL training, pktsPerSec %f pktsPerFrame %f rlPadsInterval %f\n",
			__func__, threadId, pktsPerSec, pktsPerFrame, rlPadsInterval);
	}

	// Since all ready now can release ring enqueue threads
	RVRTP_SEMAPHORE_GIVE(mp->ringStart, 1);

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		for (uint32_t ring = start_ring; ring < max_ring; ring = ring + mp->maxSchThrds)
		{ /* ring for video session */
			s = StGetSessionByRing(ring);
			if (unlikely(!s))
			{
				continue;
			}
			pacing = &s->pacing;

			rv = rte_ring_sc_dequeue_bulk(dev->txRing[txPortId][ring], (void **)&mbuf[0], bulkNum, NULL);
			if (unlikely(rv == 0))
			{
				continue;
			}

			pktIdx = StMbufGetIdx(mbuf[0]);
			if (unlikely(!pktIdx)) /* warm start for the first packet */
			{
				int32_t warmPkts = pacing->warmPktsForRL;
				struct rte_mbuf *pads[warmPkts];
				uint64_t curTsc = StGetTscTimeNano();
				uint64_t targetTsc = StMbufGetTimestamp(mbuf[0]);

				if (curTsc < targetTsc)
				{
					StTscTimeNanoSleepTo(StMbufGetTimestamp(mbuf[0]));
				}
				else
				{ /* reduce padding pkts as the time */
					int32_t deltaPkts = (curTsc - targetTsc) / pacing->trs;

					if (deltaPkts > warmPkts)
					{
						warmPkts = 0;
						dev->pacingVrxCnt[txPortId][ring] += 1;
					}
					else
					{
						warmPkts -= deltaPkts;
					}
				}

				for (int i = 0; i < warmPkts; i++)
				{
					pads[i] = padPacket[0];
				}
				rte_mbuf_refcnt_update(padPacket[0], warmPkts);
				StSchtTxBurst(txPortId, ring, &pads[0], warmPkts);
				dev->pausesTx[txPortId][ring] += warmPkts;
			}

			/* check if it need insert padding packet */
			if (fmodf(pktIdx + 1, pacing->padIntervalForRL) < bulkNum)
			{
				rte_mbuf_refcnt_update(padPacket[0], 1);
				StSchtTxBurst(txPortId, ring, &padPacket[0], 1);
				dev->pausesTx[txPortId][ring] += 1;
			}

			/* Sending this pkt on each rl queue now */
			StSchtTxBurst(txPortId, ring, &mbuf[0], bulkNum);
			dev->packetsTx[txPortId][ring] += bulkNum;
		}

		if (schedId == 0)
		{
			uint32_t ring
				= dev->dev.maxSt21Sessions; /* audio/anc/kni ring */
			int arv = rte_ring_sc_dequeue(dev->txRing[txPortId][ring], (void **)&mbuf[0]);
			if (unlikely(arv < 0))
			{
				continue;
			}

			/* Sending this pkt on audio/anc/kni queue(last) */
			StSchtTxBurst(txPortId, StGetMaxTxQueues() - 1, &mbuf[0], 1);
			dev->packetsTx[txPortId][ring] += 1;
		}
	}

	return 0;
}

int
LcoreMainTransmitter(void *args)
{
	if (StIsTscPacing())
		LcoreMainTransmitterTscPacing(args);
	else if (StIsNicRlPacing())
		LcoreMainTransmitterNicRlPacing(args);
	else
		LcoreMainTransmitterPause(args);

	return 0;
}

