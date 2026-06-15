/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the PTP servo / delta-adjustment unit tests.
 *
 * Includes the production mt_ptp.c directly so that the file-local static
 * functions (pi_sample, ptp_adjust_delta, ptp_correct_ts,
 * ptp_net_tmstamp_to_ns, ...) become visible in this translation unit.
 * Non-static symbols duplicate those in libmtl; --allow-multiple-definition
 * resolves this. USDT is disabled to avoid probe-semaphore link references.
 */

/* mt_main.h defines _GNU_SOURCE, but ut_common.h pulls system headers first;
 * define it here so struct timex / clock_adjtime are exposed to mt_ptp.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timex.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_ptp.c"

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut_ptp_ctx {
  struct mtl_main_impl impl;
  struct mt_ptp_impl ptp;
};

/* pull in the public header after the struct so the opaque typedef resolves */
#include "ptp/ptp_harness.h"

/* The rte_eth_timesync_adjust_{time,freq} calls reached when no_timesync=false
 * are mocked via gmock in timesync_mock.cpp; nothing to override here. */

/* ── init ─────────────────────────────────────────────────────────────── */

int ut_ptp_init(void) {
  return ut_eal_init();
}

/* ── create / destroy ─────────────────────────────────────────────────── */

ut_ptp_ctx* ut_ptp_create(void) {
  ut_ptp_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();

  ctx->ptp.impl = &ctx->impl;
  ctx->ptp.port = MTL_PORT_P;
  ctx->ptp.port_id = 0;
  ctx->ptp.no_timesync = true;
  ctx->ptp.coefficient = 1.0;

  /* feature bit lives on the per-interface descriptor reached via mt_if() */
  ctx->impl.inf[MTL_PORT_P].feature = 0;
  return ctx;
}

void ut_ptp_destroy(ut_ptp_ctx* ctx) {
  free(ctx);
}

/* ── setters ──────────────────────────────────────────────────────────── */

void ut_ptp_set_feature_adjust_freq(ut_ptp_ctx* ctx, bool enable) {
  if (enable)
    ctx->impl.inf[ctx->ptp.port].feature |= MT_IF_FEATURE_TIMESYNC_ADJUST_FREQ;
  else
    ctx->impl.inf[ctx->ptp.port].feature &= ~MT_IF_FEATURE_TIMESYNC_ADJUST_FREQ;
}

void ut_ptp_set_phc2sys_active(ut_ptp_ctx* ctx, bool active) {
  ctx->ptp.phc2sys_active = active;
}

void ut_ptp_set_t2(ut_ptp_ctx* ctx, uint64_t t2) {
  ctx->ptp.t2 = t2;
}

void ut_ptp_set_no_timesync(ut_ptp_ctx* ctx, bool no_timesync) {
  ctx->ptp.no_timesync = no_timesync;
}

void ut_ptp_set_coefficient(ut_ptp_ctx* ctx, double coefficient) {
  ctx->ptp.coefficient = coefficient;
}

void ut_ptp_set_last_sync_ts(ut_ptp_ctx* ctx, uint64_t last_sync_ts) {
  ctx->ptp.last_sync_ts = last_sync_ts;
}

/* ── code under test ──────────────────────────────────────────────────── */

void ut_ptp_adjust_delta(ut_ptp_ctx* ctx, int64_t delta, bool error_correct) {
  ptp_adjust_delta(&ctx->ptp, delta, error_correct);
}

double ut_pi_sample(ut_ptp_ctx* ctx, double offset, double local_ts, int* out_state) {
  enum servo_state state = UNLOCKED;
  double ppb = pi_sample(&ctx->ptp.servo, offset, local_ts, &state);
  if (out_state) *out_state = (int)state;
  return ppb;
}

long ut_ppb_to_freq(double ppb) {
  return -1 * (long)(ppb * 65.536);
}

uint64_t ut_ptp_correct_ts(ut_ptp_ctx* ctx, uint64_t ts) {
  return ptp_correct_ts(&ctx->ptp, ts);
}

uint64_t ut_ptp_net_tmstamp_to_ns(uint16_t sec_msb, uint32_t sec_lsb, uint32_t ns) {
  struct mt_ptp_tmstamp ts;
  ts.sec_msb = htons(sec_msb);
  ts.sec_lsb = htonl(sec_lsb);
  ts.ns = htonl(ns);
  return ptp_net_tmstamp_to_ns(&ts);
}

/* ── getters ──────────────────────────────────────────────────────────── */

int64_t ut_ptp_no_timesync_delta(const ut_ptp_ctx* ctx) {
  return ctx->ptp.no_timesync_delta;
}
int64_t ut_ptp_ptp_delta(const ut_ptp_ctx* ctx) {
  return ctx->ptp.ptp_delta;
}
int64_t ut_ptp_stat_delta_min(const ut_ptp_ctx* ctx) {
  return ctx->ptp.stat_delta_min;
}
int64_t ut_ptp_stat_delta_max(const ut_ptp_ctx* ctx) {
  return ctx->ptp.stat_delta_max;
}
int32_t ut_ptp_stat_delta_cnt(const ut_ptp_ctx* ctx) {
  return ctx->ptp.stat_delta_cnt;
}
uint64_t ut_ptp_delta_result_cnt(const ut_ptp_ctx* ctx) {
  return ctx->ptp.delta_result_cnt;
}
bool ut_ptp_locked(const ut_ptp_ctx* ctx) {
  return ctx->ptp.locked;
}
uint16_t ut_ptp_stat_sync_keep(const ut_ptp_ctx* ctx) {
  return ctx->ptp.stat_sync_keep;
}
int ut_ptp_servo_count(const ut_ptp_ctx* ctx) {
  return ctx->ptp.servo.count;
}
double ut_ptp_servo_drift(const ut_ptp_ctx* ctx) {
  return ctx->ptp.servo.drift;
}
