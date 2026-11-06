/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _IPMX_SR_RX_HARNESS_H_
#define _IPMX_SR_RX_HARNESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An st20 RX session with the lib/src/ipmx/ipmx_sr_rx.c queues on fake rxqs */
typedef struct ut_srrx_ctx ut_srrx_ctx;

/* what the SR queue of one port was created with */
struct ut_srrx_flow {
  uint8_t dip[4];
  uint8_t sip[4];
  uint16_t dst_port;
  bool hdr_split;
  bool hdr_split_cb;
  bool force_cni;
};

int ut_srrx_init(void);
/* ipmx_sr_rx_parse() with the session's video Media Info Block type (0x0001 st20,
 * 0x0003 st22) and the poll burst size of lib/src/ipmx/ipmx_sr_rx.c */
int ut_srrx_parse(const uint8_t* rtcp, size_t len, uint16_t video_mib,
                  struct ipmx_rx_sr_info* sr);
int ut_srrx_burst_size(void);
ut_srrx_ctx* ut_srrx_create(int num_port, uint32_t flags);
void ut_srrx_destroy(ut_srrx_ctx* ctx);

/* ipmx_sr_rx_init() and, if it made the state, ipmx_sr_rx_queue_get() on every port with
 * a media flow on dst port 20000 with header split */
int ut_srrx_attach(ut_srrx_ctx* ctx);
/* make the session an st22 one created with st22_flags, as rv_attach() does */
void ut_srrx_set_st22_flags(ut_srrx_ctx* ctx, uint32_t st22_flags);
bool ut_srrx_has_state(const ut_srrx_ctx* ctx);
void ut_srrx_detach(ut_srrx_ctx* ctx);
/* the queue put and get of a source update */
void ut_srrx_queue_put(ut_srrx_ctx* ctx);
int ut_srrx_queue_get(ut_srrx_ctx* ctx);
/* the next mt_rxq_get() returns NULL */
void ut_srrx_fail_next_get(ut_srrx_ctx* ctx);
int ut_srrx_rxq_gets(const ut_srrx_ctx* ctx);
int ut_srrx_rxq_puts(const ut_srrx_ctx* ctx);
bool ut_srrx_has_rxq(const ut_srrx_ctx* ctx, int s_port);
void ut_srrx_flow(const ut_srrx_ctx* ctx, int s_port, struct ut_srrx_flow* flow);

/* the ops ssrc, before attach */
void ut_srrx_set_ops_ssrc(ut_srrx_ctx* ctx, uint32_t ssrc);
/* the port has a CNI queue, the CNI runs as a tasklet or a thread; before attach */
void ut_srrx_set_cni(ut_srrx_ctx* ctx, int s_port, bool on, bool tasklet);
/* the media flow has MT_RXQ_FLOW_F_FORCE_CNI (MTL_FLAG_RX_USE_CNI), before attach */
void ut_srrx_set_media_force_cni(ut_srrx_ctx* ctx, bool on);
/* the next SRs carry a NIC RX timestamp of ns, or none */
void ut_srrx_set_hw_ts(ut_srrx_ctx* ctx, bool on, uint64_t ns);
/* ipmx_sr_rx_stat_log() of the session */
void ut_srrx_stat_log(ut_srrx_ctx* ctx);

/* queue an RTCP payload behind an IPv4/UDP header to port + 1 */
int ut_srrx_push(ut_srrx_ctx* ctx, int s_port, const uint8_t* rtcp, size_t len);
/* queue a frame as is, from the Ethernet header on */
int ut_srrx_push_raw(ut_srrx_ctx* ctx, int s_port, const uint8_t* frame, size_t len);
int ut_srrx_poll(ut_srrx_ctx* ctx, int s_port);
/* one media packet with ssrc seen on s_port, as the RX tasklet passes it */
void ut_srrx_media(ut_srrx_ctx* ctx, int s_port, uint32_t ssrc);
void ut_srrx_set_ptp(ut_srrx_ctx* ctx, uint64_t ns);
/* mbufs of the harness pool not back in it */
unsigned int ut_srrx_mbufs_in_use(const ut_srrx_ctx* ctx);

int ut_srrx_get(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info);
/* the public getters, of a handle of the right or a wrong type */
int ut_srrx_api_get(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                    bool valid_handle);
int ut_srrx_api_get_pipeline(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                             bool valid_handle);
/* the public st22 / st22p getters, of a handle of the right or a wrong type */
int ut_srrx_api_get_st22(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                         bool valid_handle);
int ut_srrx_api_get_st22p(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                          bool valid_handle);

#ifdef __cplusplus
}
#endif

#endif
