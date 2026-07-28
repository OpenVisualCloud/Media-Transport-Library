/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST40_TX_SESSION_HARNESS_H_
#define _ST40_TX_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_txa_ctx ut_txa_ctx;

int ut_txa_init(void);
ut_txa_ctx* ut_txa_create(void);
void ut_txa_destroy(ut_txa_ctx* ctx);
void ut_txa_set_cur_epochs(ut_txa_ctx* ctx, uint64_t cur_epochs);
void ut_txa_set_user_pacing(ut_txa_ctx* ctx, bool enable);
void ut_txa_set_exact_user_pacing(ut_txa_ctx* ctx, bool enable);
void ut_txa_set_mock_ptp_time(ut_txa_ctx* ctx, uint64_t ptp_ns);
void ut_txa_set_mock_tsc_time(ut_txa_ctx* ctx, uint64_t tsc_ns);
uint64_t ut_txa_calc_epoch(ut_txa_ctx* ctx, uint64_t cur_tai, uint64_t required_tai);
uint64_t ut_txa_pacing_required_tai(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp);
int ut_txa_sync_pacing(ut_txa_ctx* ctx, uint64_t required_tai);
int ut_txa_prepare_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                 uint64_t timestamp, unsigned int packets);
int ut_txa_step_frame_tasklet(ut_txa_ctx* ctx);
unsigned int ut_txa_queued_packets(const ut_txa_ctx* ctx);
int ut_txa_pop_packet_tsc(ut_txa_ctx* ctx, uint64_t* packet_tsc);
void ut_txa_cleanup_frame_tasklet(ut_txa_ctx* ctx);
int ut_txa_run_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc);
uint64_t ut_txa_cur_epochs(const ut_txa_ctx* ctx);
uint64_t ut_txa_ptp_time_cursor(const ut_txa_ctx* ctx);
uint64_t ut_txa_tsc_time_cursor(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_onward(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_drop(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_error_user_timestamp(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_mismatch(const ut_txa_ctx* ctx);
int ut_txa_notify_late_calls(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_late_last_delta(const ut_txa_ctx* ctx);
int ut_txa_get_next_frame_calls(const ut_txa_ctx* ctx);
int ut_txa_notify_frame_done_calls(const ut_txa_ctx* ctx);
uint16_t ut_txa_notify_frame_done_idx(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_frame_done_epoch(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_frame_done_timestamp(const ut_txa_ctx* ctx);
enum st10_timestamp_fmt ut_txa_notify_frame_done_tfmt(const ut_txa_ctx* ctx);
uint32_t ut_txa_notify_frame_done_rtp_timestamp(const ut_txa_ctx* ctx);
bool ut_txa_frame_is_waiting(const ut_txa_ctx* ctx);
int ut_txa_frame_refcnt(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_build(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_packets(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_bytes(const ut_txa_ctx* ctx);
uint32_t ut_txa_packet_len(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_frames(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_recoverable_error(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_unrecoverable_error(const ut_txa_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST40_TX_SESSION_HARNESS_H_ */
