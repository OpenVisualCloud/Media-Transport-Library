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

#ifndef _ST_RTP_H
#define _ST_RTP_H

#include "st_api_internal.h"

void *StRtpBuildIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip, uint32_t portId);
void *StRtpUpdateIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip);
void *StRtpBuildUdpHeader(st_session_impl_t *s, struct rte_udp_hdr *udp);

void *StRtpBuildL2Packet(st_session_impl_t *s, struct rte_ether_hdr *l2, uint32_t portId);

st_status_t StRtpIpUdpHdrCheck(st_session_impl_t *s, const struct rte_ipv4_hdr *ip);

void *StRtpFillHeader(st_session_impl_t *s, struct rte_ether_hdr *l2);

void StRtpFillHeaderR(st_session_impl_t *s, uint8_t *l2R, uint8_t *l2);

#endif
