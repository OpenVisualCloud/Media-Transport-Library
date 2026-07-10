/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_VIDEO_TRANSMITTER_HARNESS_H_
#define _ST_VIDEO_TRANSMITTER_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_trs_ctx ut_trs_ctx;

int ut_trs_init(void);

ut_trs_ctx* ut_trs_create(void);
void ut_trs_destroy(ut_trs_ctx* ctx);

void ut_trs_set_trs(ut_trs_ctx* ctx, long double trs_ns);
void ut_trs_set_warm_pkts_cap(ut_trs_ctx* ctx, uint32_t warm_pkts_cap);
void ut_trs_set_target_tsc(ut_trs_ctx* ctx, uint64_t target_tsc);

void ut_trs_set_mock_tsc_script(ut_trs_ctx* ctx, const uint64_t* values, int count);

void ut_trs_warm_up(ut_trs_ctx* ctx);

uint32_t ut_trs_pad_send_count(const ut_trs_ctx* ctx);
uint64_t ut_trs_last_tsc(const ut_trs_ctx* ctx);
uint64_t ut_trs_get_mock_tsc(ut_trs_ctx* ctx);
uint64_t ut_trs_stat_troffset_mismatch(const ut_trs_ctx* ctx);
uint64_t ut_trs_stat_recalculate_warmup(const ut_trs_ctx* ctx);

void ut_trs_set_burst_force_fail(ut_trs_ctx* ctx, bool fail);
uint32_t ut_trs_burst_call_count(const ut_trs_ctx* ctx);
uint32_t ut_trs_real_send_count(const ut_trs_ctx* ctx);
uint64_t ut_trs_last_pad_send_tsc(const ut_trs_ctx* ctx);
uint64_t ut_trs_last_real_send_tsc(const ut_trs_ctx* ctx);
uint16_t ut_trs_pad_refcnt(const ut_trs_ctx* ctx);
unsigned int ut_trs_pad_inflight_num(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_num(const ut_trs_ctx* ctx);
uint64_t ut_trs_target_tsc(const ut_trs_ctx* ctx);
int ut_trs_rl_state(const ut_trs_ctx* ctx);
bool ut_trs_recovery_pending(const ut_trs_ctx* ctx);

int ut_trs_rl_state_port(const ut_trs_ctx* ctx, int port);
void ut_trs_set_rl_state_port(ut_trs_ctx* ctx, int port, int state);
bool ut_trs_recovery_pending_port(const ut_trs_ctx* ctx, int port);
void ut_trs_set_recovery_pending_port(ut_trs_ctx* ctx, int port, bool pending);
void ut_trs_call_port_cleanup(ut_trs_ctx* ctx, int port);

void ut_trs_set_pad_inflight_num(ut_trs_ctx* ctx, unsigned int n);
void ut_trs_set_inflight_num(ut_trs_ctx* ctx, unsigned int n);
void ut_trs_set_inflight_num2(ut_trs_ctx* ctx, unsigned int n);
void ut_trs_prepare_cleanup_state(ut_trs_ctx* ctx);
void ut_trs_cleanup_state(ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_num2(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_idx(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_idx2(const ut_trs_ctx* ctx);
unsigned int ut_trs_priv_pool_avail(void);

void ut_trs_set_last_burst_succ_tsc(ut_trs_ctx* ctx, uint64_t tsc);
uint64_t ut_trs_get_last_burst_succ_tsc(const ut_trs_ctx* ctx);
void ut_trs_set_hang_detect_thresh_ns(ut_trs_ctx* ctx, uint64_t thresh_ns);

int ut_trs_get_stat_trs_ret_code(const ut_trs_ctx* ctx);
int ut_trs_get_stat_pkts_burst(const ut_trs_ctx* ctx);

uint16_t ut_trs_call_burst_pad(ut_trs_ctx* ctx);
int ut_trs_call_rl_tasklet(ut_trs_ctx* ctx);

void ut_trs_enqueue_ring_pkt(ut_trs_ctx* ctx);
void ut_trs_enqueue_first_pkt(ut_trs_ctx* ctx, uint64_t target_tsc);

#ifdef __cplusplus
}
#endif

#endif /* _ST_VIDEO_TRANSMITTER_HARNESS_H_ */
