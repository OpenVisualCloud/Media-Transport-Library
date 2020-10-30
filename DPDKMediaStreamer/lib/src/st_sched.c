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

//#define TX_SCH_DEBUG   1
//#define _TX_SCH_DEBUG_ 1
//#define _TX_RINGS_DEBUG_ 1
#define TX_RINGS_DEBUG 1
//#define ST_SCHED_TIME_PRINT 1
//#define TX_SCH_DEBUG_PAUSE 1

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

static inline uint32_t
StSchFillPacketBulk(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t i,
					uint32_t vectSize, struct rte_mbuf **vecTemp, struct rte_mbuf **vec)
{
	vec[i] = vecTemp[0];
	vec[i + vectSize] = vecTemp[1];
	vec[i + 2 * vectSize] = vecTemp[2];
	vec[i + 3 * vectSize] = vecTemp[3];
	dev->packetsTx[deqRing] += 4;
	sch->burstSize += 4;

	uint32_t phyPktSize
		= vecTemp[0]->data_len + vecTemp[1]->data_len + vecTemp[2]->data_len + vecTemp[3]->data_len;

	// sch->currentBytes += 4 * ST_PHYS_PKT_ADD + phyPktSize;

	phyPktSize = ST_PHYS_PKT_ADD + (phyPktSize >> 2); // /4
	sch->timeCursor -= phyPktSize;

	PKT_LOG(dev->packetsTx[deqRing], "packet %u ring %u of %llu\n", dev->txPktSizeL1[deqRing],
			deqRing, (U64)dev->packetsTx[deqRing]);

	return phyPktSize;
}

static inline void
StSchFillPauseBulk(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t i,
				   uint32_t vectSize, struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	PAUSE_LOG("lack of packet on ring %u, submitting pause of %u\n", deqRing,
			  dev->txPktSizeL1[deqRing]);
	/* put pause if no packet */
	vec[i] = pauseFrame[sch->slot];
	vec[i + vectSize] = pauseFrame[sch->slot];
	vec[i + 2 * vectSize] = pauseFrame[sch->slot];
	vec[i + 3 * vectSize] = pauseFrame[sch->slot];
	// adjust pause size
	uint16_t pauseSize = sch->pktSize & ~0x1;
	vec[i]->data_len = PktL2Size((int)pauseSize);
	vec[i]->pkt_len = PktL2Size((int)pauseSize);
	rte_mbuf_refcnt_update(vec[i], 4);
	dev->pausesTx[deqRing] += 4;

	sch->timeCursor -= pauseSize;
	// sch->currentBytes += 4 * sch->pktSize;
	sch->burstSize += 4;
	sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
}

static inline void
StSchFillGapBulk(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t phyPktSize,
				 struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	int32_t leftBytes = dev->txPktSizeL1[deqRing] - phyPktSize;
	if (unlikely(leftBytes > ST_MIN_PKT_L1_SZ))
	{
		// only practical case is 720p 3rd frame that is 886 bytes on L1
		// optimize for it so up to 2 such gaps were in 4 packets

		if ((leftBytes << 2) <= ST_DEFAULT_LEFT_BYTES_720P)
		{
			vec[sch->top] = pauseFrame[sch->slot];
			uint16_t pauseSize = leftBytes << 2;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 1);
			dev->pausesTx[deqRing] += 1;
			sch->top += 1;
			sch->timeCursor -= pauseSize;
			sch->burstSize += 1;
		}
		else // strange not expected behavior, huge gap
		{
			PAUSE_LOG("lack of big enought pkt on ring %u, submitting pause of %u\n", deqRing,
					  dev->txPktSizeL1[deqRing]);
			/* put pause if no packet */
			vec[sch->top] = pauseFrame[sch->slot];
			vec[sch->top + 1] = pauseFrame[sch->slot];
			vec[sch->top + 2] = pauseFrame[sch->slot];
			vec[sch->top + 3] = pauseFrame[sch->slot];
			// adjust pause size
			uint16_t pauseSize = leftBytes & ~0x1;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 4);
			dev->pausesTx[deqRing] += 4;
			sch->top += 4;
			sch->timeCursor -= 4 * pauseSize;
			sch->burstSize += 4;
		}
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
}

static inline void
StSchPacketOrPauseBulk(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t i,
					   uint32_t vectSize, uint32_t deq, struct rte_mbuf **vecTemp,
					   struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	// put packets
	dev->packetsTx[deqRing] += deq;

	/* put pause if no packet */
	uint32_t pauseCount = 4 - deq;
	uint32_t deqPauseIt = 0;
	while (deqPauseIt < pauseCount)
	{
		vecTemp[deq + deqPauseIt] = pauseFrame[sch->slot];
		deqPauseIt++;
		dev->pausesTx[deqRing]++;
	}

	vec[i] = vecTemp[0];
	vec[i + vectSize] = vecTemp[1];
	vec[i + 2 * vectSize] = vecTemp[2];
	vec[i + 3 * vectSize] = vecTemp[3];
	sch->burstSize += 4;

	if (deqPauseIt)
	{
		// adjust pause size
		uint16_t pauseSize = PktL2Size((int)sch->pktSize) & ~0x1;
		pauseFrame[sch->slot]->data_len = pauseSize;
		pauseFrame[sch->slot]->pkt_len = pauseSize;
		rte_mbuf_refcnt_update(pauseFrame[sch->slot], deqPauseIt);
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}

	uint32_t phyPktSize
		= vecTemp[0]->data_len + vecTemp[1]->data_len + vecTemp[2]->data_len + vecTemp[3]->data_len;

	// sch->currentBytes += 4 * ST_PHYS_PKT_ADD + phyPktSize;

	phyPktSize = ST_PHYS_PKT_ADD + (phyPktSize >> 2); // /4
	sch->timeCursor -= phyPktSize;
}

static inline void
StSchFillOobBulk(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing,
				 struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	if (sch->timeCursor >= ST_MIN_PKT_SIZE)
	{
		if ((sch->timeCursor << 2) <= ST_DEFAULT_PKT_L1_SZ)
		{
			vec[sch->top] = pauseFrame[sch->slot];
			uint16_t pauseSize = sch->timeCursor << 2;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 1);
			// sch->currentBytes += PktL1Size(vec[where]->data_len);
			sch->burstSize += 1;
			sch->timeCursor = 0;
			dev->pausesTx[deqRing] += 1;
			sch->top += 1;
		}
		else
		{
			vec[sch->top] = pauseFrame[sch->slot];
			vec[sch->top + 1] = pauseFrame[sch->slot];
			vec[sch->top + 2] = pauseFrame[sch->slot];
			vec[sch->top + 3] = pauseFrame[sch->slot];
			uint16_t pauseSize = sch->timeCursor & ~0x1;
			vec[sch->top]->data_len = PktL2Size((int)pauseSize);
			vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
			rte_mbuf_refcnt_update(vec[sch->top], 4);
			// sch->currentBytes += 4 * PktL1Size(vec[where]->data_len);
			sch->burstSize += 4;
			sch->timeCursor -= pauseSize;
			dev->pausesTx[deqRing] += 4;
			sch->top += 4;
		}
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
}

static inline uint32_t
StSchFillPacketSingle(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t i,
					  struct rte_mbuf **vec)
{
	dev->packetsTx[deqRing]++;
	sch->burstSize++;

	uint32_t phyPktSize = PktL1Size(vec[i]->data_len);
	sch->timeCursor -= phyPktSize;

	PKT_LOG(dev->packetsTx[deqRing], "packet %u ring %u of %llu\n", dev->txPktSizeL1[deqRing],
			deqRing, (U64)dev->packetsTx[deqRing]);

	return phyPktSize;
}

static inline void
StSchFillPauseSingle(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing, uint32_t i,
					 struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	PAUSE_LOG("lack of packet on ring %u, submitting pause of %u\n", deqRing,
			  dev->txPktSizeL1[deqRing]);
	/* put pause if no packet */
	vec[i] = pauseFrame[sch->slot];
	// adjust pause size
	uint16_t pauseSize = sch->pktSize & ~0x1;
	vec[i]->data_len = PktL2Size((int)pauseSize);
	vec[i]->pkt_len = PktL2Size((int)pauseSize);
	rte_mbuf_refcnt_update(vec[i], 1);
	dev->pausesTx[deqRing]++;
	sch->timeCursor -= pauseSize;
	// sch->currentBytes += sch->pktSize;
	sch->burstSize++;
	sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
}

static inline void
StSchFillGapSingle(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing,
				   uint32_t phyPktSize, struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	int32_t leftBytes = dev->txPktSizeL1[deqRing] - phyPktSize;
	if (unlikely(leftBytes >= ST_MIN_PKT_L1_SZ))
	{
		// only practical case is 720p 3rd frame that is 886 bytes on L1
		vec[sch->top] = pauseFrame[sch->slot];
		uint16_t pauseSize = leftBytes & ~0x1;
		vec[sch->top]->data_len = PktL2Size((int)pauseSize);
		vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
		rte_mbuf_refcnt_update(vec[sch->top], 1);
		dev->pausesTx[deqRing]++;
		sch->top++;
		sch->timeCursor -= pauseSize;
		sch->burstSize++;
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
	}
}

static inline void
StSchFillOobSingle(tprs_scheduler_t *sch, rvrtp_device_t *dev, uint32_t deqRing,
				   struct rte_mbuf **pauseFrame, struct rte_mbuf **vec)
{
	if (sch->timeCursor >= ST_MIN_PKT_SIZE)
	{
		/* send pause here always */
		vec[sch->top] = pauseFrame[sch->slot];
		uint16_t pauseSize = sch->timeCursor & ~0x1;
		vec[sch->top]->data_len = PktL2Size((int)pauseSize);
		vec[sch->top]->pkt_len = PktL2Size((int)pauseSize);
		rte_mbuf_refcnt_update(vec[sch->top], 1);
		// sch->currentBytes += PktL1Size(vec[top]->data_len);
		sch->burstSize++;
		sch->timeCursor -= pauseSize;
		dev->pausesTx[deqRing]++;
		sch->top++;
		sch->slot = (sch->slot + 1) % MAX_PAUSE_FRAMES;
		PAUSE_PKT_LOG(dev->pausesTx[deqRing], "Out of bound ring %u, submitting pause of %u\n",
					  sch->ring, sch->pktSize);
	}
}

/*
 * Prepare pause frames
 */
static struct rte_mbuf *
StSchBuildPausePacket(st_main_params_t *mp)
{
	if (mp == NULL)
		return NULL;

	struct rte_ether_addr srcMac;
	rte_eth_macaddr_get(mp->txPortId, &srcMac);

	/*Create the 802.3 PAUSE packet */
	struct rte_mbuf *pausePacket = rte_pktmbuf_alloc(mp->mbufPool);
	if (pausePacket)
	{
		char *pkt = rte_pktmbuf_append(pausePacket, 1514);
		struct ethhdr *eth_hdr = (typeof(eth_hdr))pkt;
		memset(pkt, 0, 1514);
		eth_hdr->h_proto = 0x0888;
		eth_hdr->h_dest[0] = 0x01;
		eth_hdr->h_dest[1] = 0x80;
		eth_hdr->h_dest[2] = 0xC2;
		eth_hdr->h_dest[5] = 0x01;
		memcpy(eth_hdr->h_source, &srcMac, 6);
	}
	else
	{
		RTE_LOG(INFO, USER1, "Packet allocation for pause error\n");
	}
	return pausePacket;
}

/**
 * Initialize scheduler thresholds so that they can be used for timeCursor dispatch
 */
st_status_t
StSchInitThread(tprs_scheduler_t *sch, rvrtp_device_t *dev, st_main_params_t *mp, uint32_t thrdId)
{
	memset(sch, 0x0, sizeof(tprs_scheduler_t));
	// sch->currentBytes = 0; /* Current time in Ethernet bytes */
	sch->pktSize = ST_HD_422_10_SLN_L1_SZ;
	sch->thrdId = thrdId;
	sch->queueId = thrdId;

	uint32_t leftQuot = 0;

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

	if (mp->maxSchThrds == 2) // 1 or 2 max
	{
		if ((dev->maxRings & 0x1) == 1) // odd number of rings
		{
			sch->lastSnRing = (dev->dev.maxSt21Sessions / 2) - 1;
			sch->outOfBoundRing = dev->maxRings / 2;
			sch->lastTxRing = sch->outOfBoundRing - 1;
			if (dev->outOfBoundRing)
			{
				if (thrdId == 0)
				{
					sch->lastTxRing = sch->outOfBoundRing;
				}
				// out of bound ring is on thread 1 if any
			}
			else
			{
				if (thrdId == 0)
				{
					sch->lastTxRing = sch->outOfBoundRing;
				}
				else
				{
					sch->outOfBoundRing--;
				}
			}
		}
		else // even number of rings
		{
			sch->outOfBoundRing = dev->maxRings / 2;
			sch->lastSnRing = (dev->dev.maxSt21Sessions / 2) - 1;
			sch->lastTxRing = sch->outOfBoundRing - 1;
			if (dev->outOfBoundRing)
			{
				if (thrdId == 0)
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
			uint32_t devTxQueue = i * mp->maxSchThrds + thrdId;
			quot += dev->txPktSizeL1[devTxQueue];
		}
		sch->quot = quot;
		if (thrdId == 0)
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
		uint32_t devTxQueue = i * mp->maxSchThrds + thrdId;
		quot -= dev->txPktSizeL1[devTxQueue];
		sch->ringThreshLo[i] = quot + sch->minPktSize;
		sch->deqRingMap[i] = devTxQueue;
	}

	for (uint32_t i = sch->lastSnRing + 1; i <= sch->lastTxRing; i++)
	{
		sch->ringThreshHi[i] = quot + sch->minPktSize;
		uint32_t devTxQueue = i * mp->maxSchThrds + thrdId;
		quot -= dev->txPktSizeL1[devTxQueue];
		sch->ringThreshLo[i] = quot + sch->minPktSize;
		sch->deqRingMap[i] = dev->dev.maxSt21Sessions + thrdId;
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
StSchDispatchTimeCursor(tprs_scheduler_t *sch, rvrtp_device_t *dev)
{
	if ((sch->ring == sch->outOfBoundRing) || (sch->timeCursor == 0))
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
				  rvrtp_device_t *dev)
{
#define ST_SCHED_TMSTAMP_TOLERANCE 100
	uint64_t const t = StPtpGetTime() + ST_SCHED_TMSTAMP_TOLERANCE;

	for (uint32_t i = 0; i < pktsCount; ++i)
	{
		if (pkts[i]->timestamp > t)
		{
			if (pkts[i]->timestamp > (t + 34 * MEGA)) // 34ms limit
			{
				//RTE_LOG(INFO, USER1, "Wrong Time %lu to wait = %lu\n", pkts[i]->timestamp, pkts[i]->timestamp - t);			
				pkts[i]->timestamp = 0;//-= 33 * MEGA;
			}
			return i;
		}
	}
	return pktsCount;
}

int
LcoreMainTransmitterBulk(void *args)
{
	RING_LOG("TRANSMITTER BULK RUNNED ON LCORE %d SOCKET %d\n", rte_lcore_id(),
			 rte_lcore_to_socket_id(rte_lcore_id()));

	st_main_params_t *mp = &stMainParams;
	rvrtp_device_t *dev = &stSendDevice;
	uint32_t threadId = (uint32_t)((uint64_t)args);
	tprs_scheduler_t *sch = rte_malloc_socket("tprsSch", sizeof(tprs_scheduler_t),
											  RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((threadId > mp->maxSchThrds) || !sch)
		rte_exit(ST_NO_MEMORY, "Transmitter init memory error\n");

	StSchInitThread(sch, dev, mp, threadId);

	uint32_t vectSize = sch->lastTxRing + 1;
	uint32_t vectSizeNPauses = vectSize;
	if (sch->lastTxRing != sch->outOfBoundRing)
	{
		vectSizeNPauses++;
	}
	const uint32_t pktVecSize = 4 * 2 * vectSizeNPauses;
	struct rte_mbuf *vec[pktVecSize];
	struct rte_mbuf *vecTemp[4];

	RING_LOG("TRANSMITTER VECTOR SIZE of %u: %u\n", vectSize, threadId);

	struct rte_mbuf *pauseFrame[MAX_PAUSE_FRAMES];

	/*Create the 802.3 PAUSE frames*/
	for (uint32_t i = 0; i < MAX_PAUSE_FRAMES; i++)
	{
		pauseFrame[i] = StSchBuildPausePacket(mp);
		if (!pauseFrame[i])
			rte_exit(ST_NO_MEMORY, "ST SCHEDULER pause allocation problem\n");
	}

	uint16_t txPortId = 0;

	RING_LOG("ST SCHEDULER on port named %s\n", mp->outPortName);
	int rv = rte_eth_dev_get_port_by_name(mp->outPortName, &txPortId);
	if (rv < 0)
	{
		rte_exit(ST_INVALID_PARAM, "TX Port : %s not found\n", mp->outPortName);
	}
	RING_LOG("ST SCHEDULER on port %u\n", txPortId);
#ifdef ST_SCHED_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	rte_eth_add_tx_callback(txPortId, sch->queueId, (rte_tx_callback_fn)StSchAlignToEpoch, dev);

	// Firstly synchronize the moment both schedulers are ready
	RVRTP_BARRIER_SYNC(mp->schedStart, threadId, mp->maxSchThrds);

	// Since all ready now can release ring enqueue threads
	RVRTP_SEMAPHORE_GIVE(mp->ringStart, 1);

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef _TX_SCH_DEBUG_
		uint32_t cnt = 0;
		memset(vec, 0x0, sizeof(struct rte_mbuf *) * pktVecSize);
#endif
		while (!mp->schedStart)
		{
#ifdef _TX_SCH_DEBUG_
			cnt++;
			if ((cnt % 2000) == 0)
			{
				RTE_LOG(INFO, USER1, "Waiting under starvation thread Id of %u...\n", threadId);
			}
#endif // TX_SCH_DEBUG
			struct rte_mbuf *mbuf;
			int rv = rte_ring_sc_dequeue(dev->txRing[dev->dev.maxSt21Sessions + threadId],
										 (void **)&mbuf);
			if (rv < 0)
				continue;
			uint32_t sent = 0;
			/* Now send this mbuf and keep trying */
			while (sent < 1)
			{
				rv = rte_eth_tx_burst(txPortId, sch->queueId, &mbuf, 1);
				sent += rv;
			}
		}

		sch->slot = 0;
		uint32_t eos = 0;
		sch->timeCursor = 0;

		while (!eos)
		{
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles0 = rte_get_tsc_cycles();
#endif
			sch->burstSize = 0;
			sch->top = 4 * vectSize;
			for (uint32_t i = 0; i < vectSizeNPauses; i++)
			{
				uint32_t deqRing = StSchDispatchTimeCursor(sch, dev);
				if (sch->ring == 0)
				{
					uint32_t rv
						= rte_ring_sc_dequeue_bulk(dev->txRing[deqRing], (void **)vecTemp, 4, NULL);
					if (unlikely(rv == 0))
					{
						__sync_synchronize();
						eos = 1;
						break;
					}
					else
					{
						/* initialize from available budget*/
						sch->timeCursor += sch->quot;
						sch->timeRemaind += sch->remaind;
						if (unlikely(sch->timeRemaind >= ST_DENOM_DEFAULT))
						{
							sch->timeRemaind -= ST_DENOM_DEFAULT;
							sch->timeCursor++;
						}
						uint32_t phyPktSize
							= StSchFillPacketBulk(sch, dev, deqRing, i, vectSize, vecTemp, vec);

						StSchFillGapBulk(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring <= sch->lastSnRing)
				{
					uint32_t rv
						= rte_ring_sc_dequeue_bulk(dev->txRing[deqRing], (void **)vecTemp, 4, NULL);
					if (unlikely(rv == 0))
					{
						StSchFillPauseBulk(sch, dev, deqRing, i, vectSize, pauseFrame, vec);
					}
					else
					{
						uint32_t phyPktSize
							= StSchFillPacketBulk(sch, dev, deqRing, i, vectSize, vecTemp, vec);

						StSchFillGapBulk(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring <= sch->lastTxRing)
				{
					uint32_t deq = 0;
					/* Now send this vector and keep trying */
					while (deq < 4)
					{
						int rv = rte_ring_sc_dequeue(dev->txRing[deqRing], (void **)&vecTemp[deq]);
						if (unlikely(rv < 0))
						{
							break;
						}
						deq++;
					}
					if (deq < 4)
					{
						// put packets or pauses
						StSchPacketOrPauseBulk(sch, dev, deqRing, i, vectSize, deq, vecTemp,
											   pauseFrame, vec);
					}
					else
					{
						// have 4 packets of some size
						uint32_t phyPktSize
							= StSchFillPacketBulk(sch, dev, deqRing, i, vectSize, vecTemp, vec);
						StSchFillGapBulk(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring == sch->outOfBoundRing)
				{
					/* send pause here always */
					PAUSE_PKT_LOG(dev->pausesTx[deqRing],
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
					StSchFillOobBulk(sch, dev, deqRing, pauseFrame, vec);
					break; // for loop
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

			uint32_t sent = 0;
			/* Now send this vector and keep trying */

			while (sent < sch->burstSize)
			{
				int rv = rte_eth_tx_burst(txPortId, sch->queueId, &vec[sent], sch->burstSize - sent);
				sent += rv;
			}
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles1 = rte_get_tsc_cycles();
			if ((dev->packetsTx[0] % 1000) == 0)
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

int
LcoreMainTransmitterSingle(void *args)
{
	RING_LOG("TRANSMITTER SINGLE RUNNED ON LCORE %d SOCKET %d\n", rte_lcore_id(),
			 rte_lcore_to_socket_id(rte_lcore_id()));

	st_main_params_t *mp = &stMainParams;
	rvrtp_device_t *dev = &stSendDevice;
	uint32_t threadId = (uint32_t)((uint64_t)args);
	tprs_scheduler_t *sch = rte_malloc_socket("tprsSch", sizeof(tprs_scheduler_t),
											  RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((threadId > mp->maxSchThrds) || !sch)
		rte_exit(ST_NO_MEMORY, "Transmitter init memory error\n");

	StSchInitThread(sch, dev, mp, threadId);

	const uint32_t vectSize = sch->lastTxRing + 1; // MIN(16, dev->maxRings);
	RING_LOG("TRANSMITTER VECTOR SIZE of %u: %u\n", vectSize, threadId);

	uint32_t vectSizeNPauses = vectSize;
	if (sch->lastTxRing != sch->outOfBoundRing)
	{
		vectSizeNPauses++;
	}

	struct rte_mbuf *vec[2 * vectSizeNPauses];
	struct rte_mbuf *pauseFrame[MAX_PAUSE_FRAMES];

	/*Create the 802.3 PAUSE frames*/
	for (uint32_t i = 0; i < MAX_PAUSE_FRAMES; i++)
	{
		pauseFrame[i] = StSchBuildPausePacket(mp);
		if (!pauseFrame[i])
			rte_exit(ST_NO_MEMORY, "ST SCHEDULER pause allocation problem\n");
	}

	uint16_t txPortId = 0;

	RING_LOG("ST SCHEDULER on port named %s\n", mp->outPortName);
	int rv = rte_eth_dev_get_port_by_name(mp->outPortName, &txPortId);
	if (rv < 0)
	{
		rte_exit(ST_INVALID_PARAM, "TX Port : %s not found\n", mp->outPortName);
	}
	RING_LOG("ST SCHEDULER on port %u\n", txPortId);
#ifdef ST_SCHED_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	rte_eth_add_tx_callback(txPortId, sch->queueId, (rte_tx_callback_fn)StSchAlignToEpoch, dev);

	// Firstly synchronize the moment both schedulers are ready
	RVRTP_BARRIER_SYNC(mp->schedStart, threadId, mp->maxSchThrds);

	// Since all ready now can release ring enqueue threads
	RVRTP_SEMAPHORE_GIVE(mp->ringStart, 1);

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef _TX_SCH_DEBUG_
		uint32_t cnt = 0;
#endif

		while (!mp->schedStart)
		{
#ifdef _TX_SCH_DEBUG_
			cnt++;
			if ((cnt % 2000) == 0)
			{
				RTE_LOG(INFO, USER1, "Waiting under starvation thread Id of %u...\n", threadId);
			}
#endif // TX_SCH_DEBUG
			struct rte_mbuf *mbuf;
			int rv = rte_ring_sc_dequeue(dev->txRing[dev->dev.maxSt21Sessions + threadId],
										 (void **)&mbuf);
			if (rv < 0)
				continue;
			uint32_t sent = 0;
			/* Now send this mbuf and keep trying */
			while (sent < 1)
			{
				rv = rte_eth_tx_burst(txPortId, sch->queueId, &mbuf, 1);
				sent += rv;
			}
		}
		sch->slot = 0;
		sch->timeCursor = 0;
		uint32_t eos = 0;

		while (!eos)
		{
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles0 = rte_get_tsc_cycles();
#endif
			sch->burstSize = 0;
			sch->top = vectSize;
			for (uint32_t i = 0; i < vectSizeNPauses; i++)
			{
				uint32_t deqRing = StSchDispatchTimeCursor(sch, dev);
				if (sch->ring == 0)
				{
					int rv = rte_ring_sc_dequeue(dev->txRing[deqRing], (void **)&vec[i]);
					if (unlikely(rv < 0))
					{
						__sync_synchronize();
						eos = 1;
						break;
					}
					else
					{
						/* initialize from available budget*/
						sch->timeCursor += sch->quot;
						sch->timeRemaind += sch->remaind;
						if (unlikely(sch->timeRemaind >= ST_DENOM_DEFAULT))
						{
							sch->timeRemaind -= ST_DENOM_DEFAULT;
							sch->timeCursor++;
						}
						uint32_t phyPktSize = StSchFillPacketSingle(sch, dev, deqRing, i, vec);
						StSchFillGapSingle(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring <= sch->lastSnRing)
				{
					int rv = rte_ring_sc_dequeue(dev->txRing[deqRing], (void **)&vec[i]);
					if (unlikely(rv < 0))
					{
						StSchFillPauseSingle(sch, dev, deqRing, i, pauseFrame, vec);
					}
					else
					{
						uint32_t phyPktSize = StSchFillPacketSingle(sch, dev, deqRing, i, vec);
						StSchFillGapSingle(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring <= sch->lastTxRing)
				{
					/* take packet from a flow ring */
					int rv = rte_ring_sc_dequeue(dev->txRing[deqRing], (void **)&vec[i]);
					if (unlikely(rv < 0))
					{
						StSchFillPauseSingle(sch, dev, deqRing, i, pauseFrame, vec);
					}
					else
					{
						uint32_t phyPktSize = StSchFillPacketSingle(sch, dev, deqRing, i, vec);
						StSchFillGapSingle(sch, dev, deqRing, phyPktSize, pauseFrame, vec);
					}
				}
				else if (sch->ring == sch->outOfBoundRing)
				{
#ifdef TX_SCH_DEBUG
					if (i != vectSize)
					{
						rte_exit(ST_GENERAL_ERR,
								 "Invalid indices %u and timeCursor for thread %u: %u!\n", i,
								 threadId, sch->timeCursor);
					}
#endif
					StSchFillOobSingle(sch, dev, deqRing, pauseFrame, vec);
				}
				else
				{
					rte_exit(127, "Invalid timeCursor for thread %u: %u!\n", threadId,
							 sch->timeCursor);
				}
			}
			if (eos)
			{
				break;
			}

			uint32_t sent = 0;
			/* Now send this vector and keep trying */
			while (sent < sch->burstSize)
			{
				int rv
					= rte_eth_tx_burst(txPortId, sch->queueId, &vec[sent], sch->burstSize - sent);
				sent += rv;
			}
#ifdef ST_SCHED_TIME_PRINT
			uint64_t cycles1 = rte_get_tsc_cycles();
			if ((dev->packetsTx[0] % 1000) == 0)
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
