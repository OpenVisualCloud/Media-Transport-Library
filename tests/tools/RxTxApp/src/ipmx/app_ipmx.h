/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _APP_IPMX_HEAD_H_
#define _APP_IPMX_HEAD_H_

#include "../app_base.h"
#include "../log.h"

struct app_ipmx_tx;

int app_ipmx_set_sdp_dir(struct st_app_context* ctx, const char* dir);
int app_ipmx_set_ts_refclk(struct st_app_context* ctx, const char* ts_refclk);

/* set the JSON block version and --ipmx_ts_refclk; write the SDP file now, or with
 * --ipmx_ts_refclk from app_ipmx_tx_poll() once the Sender Reports carry it */
int app_ipmx_tx_st20p_init(struct st_app_context* ctx, st_json_st20p_session_t* st20p,
                           struct st_app_tx_st20p_session* s, const char* name);
int app_ipmx_tx_st22p_init(struct st_app_context* ctx, st_json_st22p_session_t* st22p,
                           struct st_app_tx_st22p_session* s, const char* name);
void app_ipmx_tx_uinit(struct app_ipmx_tx** ipmx);
/* rewrite the SDP file of every IPMX TX session whose SDP changed */
void app_ipmx_tx_poll(struct st_app_context* ctx);

void app_ipmx_rx_st20p_result(struct st_app_rx_st20p_session* s);
void app_ipmx_rx_st22p_result(struct st_app_rx_st22p_session* s);

#endif
