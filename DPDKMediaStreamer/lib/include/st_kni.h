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

#ifndef _ST_KNI_H
#define _ST_KNI_H

#include <rte_kni.h>

#include "rvrtp_main.h"
#include "st_api.h"

#include <unistd.h>

struct st_kni_ms_conf;
typedef struct st_kni_ms_conf st_kni_ms_conf_t;

extern st_kni_ms_conf_t *StInitKniConf(int32_t ethPortId, struct rte_mempool *mbufPool,
									   uint16_t rxRingNb, uint32_t txThread,
									   struct rte_ring *txRing, int32_t userPortId);

extern int32_t StStartKni(uint32_t slvCoreRx, uint32_t slvCoreTx, st_kni_ms_conf_t **c);

extern int32_t StInitKni(int32_t nbs);
extern int32_t StStopKni(st_kni_ms_conf_t **cs);

st_status_t StKniUpdateLink(st_kni_ms_conf_t **c, unsigned int linkup);

// implemented in st_dev.c
extern const char *StDevGetKniInterName(int portId);
extern st_status_t StKniBkgTask(void);

#endif	// _ST_KNI_H
