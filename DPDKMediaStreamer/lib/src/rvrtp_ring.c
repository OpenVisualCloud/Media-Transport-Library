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
#include <unistd.h>

volatile uint64_t pktsBuild = 0;
volatile uint64_t pktsQueued = 0;

//#define TX_RINGS_DEBUG 1
//#define ST_RING_TIME_PRINT
//#define TX_TIMER_DEBUG 1
//#define _TX_RINGS_DEBUG_ 1

#define ST_CLOCK_PRECISION_TIME 40000ul
#define ST_TPRS_SLOTS_ADVANCE   8

uint64_t adjustCount[6] = { 0, 0, 0, 0, 0, 0 };

/**
* Align each 8th packet to reduce burstiness 
*/
static inline void
RvRtpAlignPacket(rvrtp_session_t *s, struct rte_mbuf *m)
{
	// set mbuf timestmap to expected nanoseconds on a wire - 8 times Tprs time
	m->timestamp = ((U64)s->ctx.epochs * s->fmt.frameTime) + s->sn.trOffset - s->nicTxTime;
	int tprsSlots = (s->ctx.line1Number + 1) * s->fmt.pktsInLine - ST_TPRS_SLOTS_ADVANCE;
	m->timestamp +=	((int64_t)tprsSlots * s->sn.tprs);
	s->ctx.alignTmstamp = 0;
}


/**
* Get the ST2110-21 timestamp of 90k aligned to the epoch
*/
uint32_t
RvRtpGetFrameTmstamp(rvrtp_session_t *s, uint32_t firstWaits, U64 *roundTime, struct rte_mbuf *m)
{
	U64 ntime, ntimeLast;

	if (unlikely(!roundTime)) ST_ASSERT;

	if (*roundTime == 0)
	{
		*roundTime = StPtpGetTime();
	}
	ntime = *roundTime;
	U64 epochs = ntime / s->fmt.frameTime;

	int areSameEpochs = 0, isOneLate = 0;

	if (s->ctx.epochs == 0) 
	{
		s->ctx.epochs = epochs;
	}
	else if ((int64_t)epochs - s->ctx.epochs > 1)
	{
		s->ctx.epochs = epochs;
		__sync_fetch_and_add(&adjustCount[0], 1);
	}
	else if ((int64_t)epochs - s->ctx.epochs == 0)
	{
		areSameEpochs++;
		__sync_fetch_and_add(&adjustCount[1], 1);
	}
	else if ((int64_t)epochs - s->ctx.epochs == 1)
	{
		isOneLate++;
		s->ctx.epochs++;
		__sync_fetch_and_add(&adjustCount[2], 1);
	}
	else
	{	
		//? case
		s->ctx.epochs = epochs;
		__sync_fetch_and_add(&adjustCount[0], 1);
	}

	U64 toEpoch;
	int64_t toElapse;
	U64 st21Tmstamp90k;
	U64 advance = s->nicTxTime + ST_TPRS_SLOTS_ADVANCE * s->sn.tprs;
	double frmTime90k = s->fmt.clockRate * s->fmt.frmRateDen / s->fmt.frmRateMul;
	s->ctx.alignTmstamp = 0;
	//ntime = StPtpGetTime();
	U64 remaind = ntime % s->fmt.frameTime; 

	if ((isOneLate || (!areSameEpochs)) && (remaind < s->sn.trOffset - advance))
	{
		if (remaind > s->sn.trOffset/2)
		{
			toElapse = 0;
			__sync_fetch_and_add(&adjustCount[3], 1);
		}
		else 
		{
			toElapse = s->sn.trOffset - advance - remaind;
			__sync_fetch_and_add(&adjustCount[4], 1);
			
		}
		
		// set 90k timestamp aligned to epoch
		st21Tmstamp90k = s->ctx.epochs * frmTime90k;
		m->timestamp = ((U64)s->ctx.epochs * s->fmt.frameTime) + s->sn.trOffset - advance;
	}
	else
	{
		s->ctx.epochs++;
		toEpoch = s->ctx.epochs * s->fmt.frameTime - ntime;
		toElapse = toEpoch + s->sn.trOffset - advance;
		// set 90k timestamp aligned to epoch
		st21Tmstamp90k = s->ctx.epochs * frmTime90k;
		__sync_fetch_and_add(&adjustCount[5], 1);
		
		// set mbuf timestmap to expected nanoseconds on a wire - ST_TPRS_SLOTS_ADVANCE times Tprs time
		m->timestamp = ((U64)s->ctx.epochs * s->fmt.frameTime) + s->sn.trOffset - advance;
	}


	//leave only complete 128us steps so that waiting will be done with 128us accuracy
	if ((toElapse > 2 * ST_CLOCK_PRECISION_TIME) && firstWaits)
	{
		toElapse -= ST_CLOCK_PRECISION_TIME;

		uint32_t repeats = 0;
		U64 elapsed;
		struct timespec req, rem;

		req.tv_sec = 0;
		if (toElapse > ST_CLOCK_PRECISION_TIME * 10)
		{
			req.tv_nsec = 2 * ST_CLOCK_PRECISION_TIME;
		}
		else
		{
			req.tv_nsec = ST_CLOCK_PRECISION_TIME / 2;
		}
	
		for (;;)
		{
			clock_nanosleep(CLOCK_REALTIME, 0, &req, &rem);
			ntimeLast = StPtpGetTime();
			elapsed = ntimeLast - ntime;
			if (elapsed + MAX(req.tv_sec, ST_CLOCK_PRECISION_TIME) > toElapse) break;
			repeats++;
		}
#ifdef TX_TIMER_DEBUG
		if (s->sn.timeslot == 0)
		{
			uint64_t tmstamp64_ = (U64)ntimeLast / s->tmstampTime;
			uint32_t tmstamp32_ = (uint32_t)tmstamp64_;
			uint32_t tmstamp32 = (uint32_t)st21Tmstamp90k;

			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: elapsed %llu diff %lld tmdelta %d\n",
				elapsed, (long long int)toElapse - elapsed, tmstamp32 - s->lastTmstamp);
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: waiting %llu tmstamp %u delta %d\n",
				toElapse, tmstamp32, (int32_t)(tmstamp32_ - tmstamp32));
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: mbuf->tmstamp %lu time %lu timeDiff %lu\n",
				m->timestamp, (uint64_t)ntimeLast, m->timestamp - ntimeLast);

		}
#endif

#if 0
	//leave only complete 128us steps so that waiting will be done with 128us accuracy
	if ((toElapse > ST_CLOCK_PRECISION_TIME) && firstWaits)
	{
		int64_t toSet = toElapse - ST_CLOCK_PRECISION_TIME/2;

		uint32_t repeats = 0;
		int64_t elapsed;
		struct timespec req, rem;

		req.tv_sec = 0;
		req.tv_nsec = (toSet * 4) / 5;
	
		for (;;)
		{
			clock_nanosleep(CLOCK_REALTIME, 0, &req, &rem);
			ntimeLast = StPtpGetTime();
			elapsed = ntimeLast - ntime;
			if (elapsed + ST_CLOCK_PRECISION_TIME > toElapse) break;
			int64_t left = toElapse - elapsed;
			if (left > ST_CLOCK_PRECISION_TIME * 10)
			{
				req.tv_nsec = 2 * ST_CLOCK_PRECISION_TIME;
			}
			else
			{
				req.tv_nsec = ST_CLOCK_PRECISION_TIME / 2;
			}
			repeats++;
		}
#ifdef TX_TIMER_DEBUG
		if ((s->sn.timeslot == 0) && (adjustCount[5] % 100 == 0))
		{
			uint64_t tmstamp64_ = (U64)ntimeLast / s->tmstampTime;
			uint32_t tmstamp32_ = (uint32_t)tmstamp64_;
			uint32_t tmstamp32 = (uint32_t)st21Tmstamp90k;

			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: elapsed %ld diff %ld tmdelta %d\n",
				elapsed, (int64_t)toElapse - elapsed, (int)(tmstamp32 - s->lastTmstamp));
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: waiting %ld tmstamp %u delta %d\n",
				toElapse, tmstamp32, (int)(tmstamp32_ - tmstamp32));
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: mbuf->tmstamp %lu time %lu timeDiff %ld\n",
				m->timestamp, (uint64_t)ntimeLast, (int64_t)m->timestamp - (int64_t)ntimeLast);

		}
#endif
#endif
	}
	s->lastTmstamp = st21Tmstamp90k;
	return (uint32_t)st21Tmstamp90k;
}


/*****************************************************************************
 *
 * RvRtpFillHdrs - Copy and fill all pkts headers
 *
 * DESCRIPTION
 * Constructs the pkt header from the template
 *
 * RETURNS: IP header location
 */
static inline void *
RvRtpFillHeader(rvrtp_session_t *s, struct rte_ether_hdr *l2)
{
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)&l2[1];
	memcpy(l2, &s->hdrPrint, sizeof(s->hdrPrint));
	return ip;
}


/* Packet creator thread runned on master lcore*/
int 
LcoreMainPktRingEnqueue(void* args) 
{
	uint32_t threadId = (uint32_t)((uint64_t)args);
	st_main_params_t *mp = &stMainParams;
	int count = 0;

	if (threadId >= mp->maxEnqThrds)
	{
		RTE_LOG(INFO, USER2, "PKT ENQUEUE RUNNING ON %d LCORE in threadId of %u\n", rte_lcore_id(), threadId);
		rte_exit(127, "Invalid threadId - exiting PKT ENQUEUE\n");
	}

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "PKT ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n",
		rte_lcore_id(), rte_lcore_to_socket_id(rte_lcore_id()), threadId);
#endif

	uint32_t ring = 0;

	rvrtp_device_t *dev = &stSendDevice;
	rvrtp_session_t *s;

	while (count < dev->dev.snCount)
	{
		sleep(1);
		if (dev->snTable[count] != NULL)
		{
			count++;
		}
	}

	uint32_t pktsCount = mp->enqThrds[threadId].pktsCount;
	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktExt[pktsCount];

	struct rte_mempool *pool = dev->mbufPool;
	if (!pool) rte_exit(127, "Packets mbufPool is invalid\n");

	uint32_t thrdSnFirst = mp->enqThrds[threadId].thrdSnFirst;
	uint32_t thrdSnLast = mp->enqThrds[threadId].thrdSnLast;

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER2, "Thread params: %u %u %u %u\n",
		thrdSnFirst, thrdSnLast, pktsCount, threadId);
#endif
#ifdef ST_RING_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds);
	RTE_LOG(INFO, USER2, "Transmitter ready - sending packet STARTED\n");

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles0 = rte_get_tsc_cycles();
#endif
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %u for %u\n",
				(uint32_t)pktsBuild, pktsCount);
			continue;
		}
		/* allocate mbufs for external buffer*/
		if (rte_pktmbuf_alloc_bulk(pool, pktExt, pktsCount) < 0)
		{
			RTE_LOG(INFO, USER2, "Packets Ext allocation problem after: %u for %u\n",
				(uint32_t)pktsBuild, pktsCount);
			continue;
		}
		U64 roundTime = 0;
		uint32_t firstSnInRound = 1;

		RVRTP_BARRIER_SYNC(mp->ringBarrier1, threadId, mp->maxEnqThrds);

		for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
		{
			s = dev->snTable[i];
			if (s == NULL)
			{
				for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
				{
					uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
				}
				continue;
			}

			uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN;
			pktVect[ij]->timestamp = 0;

			do
			{
				if (unlikely(s->ctx.tmstamp == 0))
				{
					s->ctx.tmstamp = RvRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[ij]);
					firstSnInRound = 0;
				}
			} while (RvRtpSessionCheckRunState(s) == 0);

			for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
			{
				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (unlikely(s->state != ST_SN_STATE_RUN))
				{
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
					continue;
				}
				struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(pktVect[ij], struct rte_ether_hdr *);
				struct rte_ipv4_hdr *ip = RvRtpFillHeader(s, l2);

				if (s->ctx.alignTmstamp) RvRtpAlignPacket(s, pktVect[ij]);

				// assemble the RTP packet accordingly to the format
				s->UpdateRtpPkt(s, ip, pktExt[ij]);

				pktVect[ij]->data_len = s->fmt.pktSize - pktExt[ij]->data_len;

				if (pktExt[ij]->data_len)
					rte_pktmbuf_chain(pktVect[ij], pktExt[ij]);
				else
					rte_pktmbuf_free(pktExt[ij]);

				pktVect[ij]->pkt_len = s->fmt.pktSize;
				pktVect[ij]->l2_len = 14;
				pktVect[ij]->l3_len = 20;
				pktVect[ij]->ol_flags = PKT_TX_IPV4;

				// UDP & IP checksum check
				if ((s->ofldFlags & (ST_OFLD_HW_IP_CKSUM | ST_OFLD_HW_UDP_CKSUM)) == (ST_OFLD_HW_IP_CKSUM | ST_OFLD_HW_UDP_CKSUM))
				{
					pktVect[ij]->ol_flags |= PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
					struct udphdr *udp = rte_pktmbuf_mtod_offset(pktVect[ij], struct udphdr *, 34);
					ip->hdr_checksum = 0;
					rte_raw_cksum_mbuf(pktVect[ij], pktVect[ij]->l2_len,
							pktVect[ij]->pkt_len - pktVect[ij]->l2_len,
							&udp->uh_sum);
				}
				__sync_fetch_and_add(&pktsBuild, 1);
			}
		}
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles1 = rte_get_tsc_cycles();
		printf("Ring pkt assembly time elapsed %llu ratio %llu pktTime %llu\n",
			(U64)(cycles1 - cycles0), (U64)ratioClk, (U64)(cycles1 - cycles0) / pktsCount);
#endif
		for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j+=4)
		{
			for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
			{
				if (!dev->snTable[i]) continue;

				uint32_t noFails = 0;
				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (!pktVect[ij]) continue;

				ring = dev->snTable[i]->sn.timeslot;

				while (rte_ring_sp_enqueue_bulk(dev->txRing[ring], (void**)&pktVect[ij], 4, NULL) == 0)
				{
					RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);
#ifdef _TX_RINGS_DEBUG_
					if ((noFails % 1000) == 0)	
						RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring, (U64)pktsQueued, noFails);
#endif
					noFails++;
					__sync_synchronize();
				}
				__sync_fetch_and_add(&pktsQueued, 1);
			}
		}
		RVRTP_BARRIER_SYNC(mp->ringBarrier2, threadId, mp->maxEnqThrds);
		
		RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);

#ifdef ST_RING_TIME_PRINT
		uint64_t cycles2 = rte_get_tsc_cycles();
		printf("Ring finish time elapsed %llu ratio %llu pktTime %llu\n",
			(U64)(cycles2 - cycles1), (U64)ratioClk, (U64)(cycles2 - cycles1) / pktsCount);
#endif
	}
	RTE_LOG(INFO, USER2, "Transmitter closed - sending packet STOPPED\n");
	return 0;
}
