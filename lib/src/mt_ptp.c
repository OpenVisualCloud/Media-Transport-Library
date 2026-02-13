/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_ptp.h"

#include "datapath/mt_queue.h"
#include "mt_cni.h"
// #define DEBUG
#include "mt_log.h"
#include "mt_mcast.h"
#include "mt_sch.h"
#include "mt_stat.h"
#include "mt_util.h"

#define MT_PTP_USE_TX_TIME_STAMP (1)
#define MT_PTP_USE_TX_TIMER (1)
#define MT_PTP_CHECK_TX_TIME_STAMP (0)
#define MT_PTP_CHECK_RX_TIME_STAMP (0)
#define MT_PTP_CHECK_HW_SW_DELTA (0)
#define MT_PTP_PRINT_ERR_RESULT (0)

#define MT_PTP_TP_SYNC_MS (10)

#define MT_PTP_DEFAULT_KP 5e-10 /* to be tuned */
#define MT_PTP_DEFAULT_KI 1e-10 /* to be tuned */

#ifdef WINDOWSENV
// clang-format off
#define be64toh(x) \
  ((1 == ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
// clang-format on

typedef BOOL(WINAPI* PGSTAP)(PDWORD64 lpTimeAdjustment, PDWORD64 lpTimeIncrement,
                             PBOOL lpTimeAdjustmentDisabled);
static PGSTAP win_get_systime_adj;

typedef BOOL(WINAPI* PSSTAP)(DWORD64 dwTimeAdjustment, BOOL bTimeAdjustmentDisabled);
static PSSTAP win_set_systime_adj;
#endif

static char* ptp_mode_strs[MT_PTP_MAX_MODE] = {
    "l2",
    "l4",
};

enum servo_state {
  UNLOCKED,
  JUMP,
  LOCKED,
};

static inline char* ptp_mode_str(enum mt_ptp_l_mode mode) {
  return ptp_mode_strs[mode];
}

static inline uint64_t ptp_net_tmstamp_to_ns(struct mt_ptp_tmstamp* ts) {
  uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
  return (sec * NS_PER_S) + ntohl(ts->ns);
}

static inline void ptp_timesync_lock(struct mt_ptp_impl* ptp) { /* todo */
  MTL_MAY_UNUSED(ptp);
}

static inline void ptp_timesync_unlock(struct mt_ptp_impl* ptp) { /* todo */
  MTL_MAY_UNUSED(ptp);
}

static inline uint64_t ptp_correct_ts(struct mt_ptp_impl* ptp, uint64_t ts) {
  int64_t ts_local_advanced = ts - ptp->last_sync_ts;
  int64_t ts_ptp_advanced = ptp->coefficient * ts_local_advanced;
  return ptp->last_sync_ts + ts_ptp_advanced;
}

static inline uint64_t ptp_no_timesync_time(struct mt_ptp_impl* ptp) {
  uint64_t tsc = mt_get_tsc(ptp->impl);
  return tsc + ptp->no_timesync_delta;
}

static inline void ptp_no_timesync_adjust(struct mt_ptp_impl* ptp, int64_t delta) {
  ptp->no_timesync_delta += delta;
}

static inline uint64_t ptp_timesync_read_time_no_lock(struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  int ret;
  struct timespec spec;

  if (ptp->no_timesync) return ptp_no_timesync_time(ptp);

  memset(&spec, 0, sizeof(spec));

  ret = rte_eth_timesync_read_time(port_id, &spec);

  if (ret < 0) {
    err("%s(%d), err %d\n", __func__, port, ret);
    return 0;
  }
  return mt_timespec_to_ns(&spec);
}

static inline uint64_t ptp_timesync_read_time(struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  int ret;
  struct timespec spec;

  if (ptp->no_timesync) return ptp_no_timesync_time(ptp);

  memset(&spec, 0, sizeof(spec));

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_time(port_id, &spec);
  ptp_timesync_unlock(ptp);

  if (ret < 0) {
    err("%s(%d), err %d\n", __func__, port, ret);
    return 0;
  }
  return mt_timespec_to_ns(&spec);
}

static inline double pi_sample(struct mt_pi_servo* s, double offset, double local_ts,
                               enum servo_state* state) {
  double ppb = 0.0;

  switch (s->count) {
    case 0:
      s->offset[0] = offset;
      s->local[0] = local_ts;
      *state = UNLOCKED;
      s->count = 1;
      break;
    case 1:
      s->offset[1] = offset;
      s->local[1] = local_ts;
      *state = UNLOCKED;
      s->count = 2;
      break;
    case 2:
      s->drift += (s->offset[1] - s->offset[0]) / (s->local[1] - s->local[0]);
      *state = UNLOCKED;
      s->count = 3;
      break;
    case 3:
      *state = JUMP;
#ifndef WINDOWSENV /* windows always adj offset since adj freq not ready */
      s->count = 4;
#endif
      break;
    case 4:
      s->drift += 0.7 * offset;
      ppb = 0.3 * offset + s->drift;
      *state = LOCKED;
      break;
  }

  return ppb;
}

static void ptp_adj_system_clock_time(struct mt_ptp_impl* ptp, int64_t delta) {
  int ret;
#ifndef WINDOWSENV
  struct timex adjtime;
  int sign = 1;

  if (delta < 0) {
    sign = -1;
    delta *= -1;
  }

  memset(&adjtime, 0, sizeof(adjtime));
  adjtime.modes = ADJ_SETOFFSET | ADJ_NANO;
  adjtime.time.tv_sec = sign * (delta / NS_PER_S);
  adjtime.time.tv_usec = sign * (delta % NS_PER_S);
  if (adjtime.time.tv_usec < 0) {
    adjtime.time.tv_sec -= 1;
    adjtime.time.tv_usec += 1000000000;
  }

  ret = clock_adjtime(CLOCK_REALTIME, &adjtime);
#else
  FILETIME ft;
  SYSTEMTIME st;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER ui;
  ui.LowPart = ft.dwLowDateTime;
  ui.HighPart = ft.dwHighDateTime;
  ui.QuadPart += delta / 100; /* in 100ns */
  ft.dwLowDateTime = ui.LowPart;
  ft.dwHighDateTime = ui.HighPart;
  FileTimeToSystemTime(&ft, &st);
  ret = SetSystemTime(&st) ? 0 : -1;
#endif
  dbg("%s(%d), delta %" PRId64 "\n", __func__, ptp->port, delta);
  if (ret < 0) {
    err("%s(%d), adj system time offset fail %d\n", __func__, ptp->port, ret);
    if (ret == -EPERM)
      err("%s(%d), please add capability to the app: sudo setcap 'cap_sys_time+ep' "
          "<app>\n",
          __func__, ptp->port);
  }
}

static void ptp_adj_system_clock_freq(struct mt_ptp_impl* ptp, double ppb) {
  int ret = -1;
#ifndef WINDOWSENV
  struct timex adjfreq;
  memset(&adjfreq, 0, sizeof(adjfreq));

  if (ptp->phc2sys.realtime_nominal_tick) {
    adjfreq.modes |= ADJ_TICK;
    adjfreq.tick =
        round(ppb / 1e3 / ptp->phc2sys.realtime_hz) + ptp->phc2sys.realtime_nominal_tick;
    ppb -= 1e3 * ptp->phc2sys.realtime_hz *
           (adjfreq.tick - ptp->phc2sys.realtime_nominal_tick);
  }

  adjfreq.modes |= ADJ_FREQUENCY;
  adjfreq.freq =
      (long)(ppb * 65.536); /* 1 ppm = 1000 ppb = 2^16 freq unit (scaled ppm) */
  ret = clock_adjtime(CLOCK_REALTIME, &adjfreq);
#else /* TBD */
  uint64_t cur_adj = 0;
  uint64_t time_inc = 0;
  int time_adj_disable = 0;

  if ((*win_get_systime_adj)(&cur_adj, &time_inc, &time_adj_disable))
    ret = (*win_set_systime_adj)(cur_adj - ppb / 100, FALSE) ? 0 : -1;
#endif
  if (ret < 0) {
    err("%s(%d), adj system time freq fail %d\n", __func__, ptp->port, ret);
    if (ret == -EPERM)
      err("%s(%d), please add capability to the app: sudo setcap 'cap_sys_time+ep' "
          "<app>\n",
          __func__, ptp->port);
  }
}

static void phc2sys_adjust(struct mt_ptp_impl* ptp) {
  enum servo_state state = UNLOCKED;
  double ppb;
  struct timespec ts1_sys, ts2_sys;
  uint64_t t_phc, t1_sys, t2_sys, t_sys, shortest_delay, delay;
  int64_t offset;
  int ret;

  ptp_timesync_lock(ptp);
  shortest_delay = UINT64_MAX;
  offset = 0;
  t_sys = 0;
  t_phc = 0;
  ret = 0;
  for (uint8_t i = 0; i < 10; i++) {
    ret = ret + clock_gettime(CLOCK_REALTIME, &ts1_sys);
    t_phc = ptp_timesync_read_time_no_lock(ptp);
    ret = ret + clock_gettime(CLOCK_REALTIME, &ts2_sys);
    if (!ret && t_phc > 0) {
      t1_sys = mt_timespec_to_ns(&ts1_sys);
      t2_sys = mt_timespec_to_ns(&ts2_sys);

      delay = t2_sys - t1_sys;
      if (shortest_delay > delay) {
        t_sys = (t1_sys + t2_sys) / 2;
        offset = t_sys - t_phc;
        shortest_delay = delay;
      }
    }
  }
  ptp_timesync_unlock(ptp);
  if (!ret && t_phc > 0) {
    ppb = pi_sample(&ptp->phc2sys.servo, offset, t_sys, &state);
    dbg("%s(%d), state %d\n", __func__, ptp->port, state);

    switch (state) {
      case UNLOCKED:
        break;
      case JUMP:
        ptp_adj_system_clock_time(ptp, -offset);
        dbg("%s(%d), CLOCK_REALTIME offset %" PRId64 ", delay %" PRIu64 " adjust time.\n",
            __func__, ptp->port_id, offset, shortest_delay);
        break;
      case LOCKED:
        ptp_adj_system_clock_freq(ptp, -ppb);
        dbg("%s(%d), CLOCK_REALTIME offset %" PRId64 ", delay %" PRIu64
            " adjust freq %lf ppb.\n",
            __func__, ptp->port_id, offset, shortest_delay, ppb);
        break;
    }

    ptp->phc2sys.stat_delta_max = RTE_MAX(labs(offset), ptp->phc2sys.stat_delta_max);

    if (!ptp->phc2sys.locked) {
      /*
       * Be considered as synchronized while the max delta is continuously below
       * 300ns.
       */
      if (ptp->phc2sys.stat_delta_max < 300 && ptp->phc2sys.stat_delta_max > 0) {
        if (ptp->phc2sys.stat_sync_keep > 100)
          ptp->phc2sys.locked = true;
        else
          ptp->phc2sys.stat_sync_keep++;
      } else {
        ptp->phc2sys.stat_sync_keep = 0;
      }
    }
  } else {
    err("%s(%d), PHC or system time retrieving failed.\n", __func__, ptp->port_id);
  }
}

static inline int ptp_timesync_read_tx_time(struct mt_ptp_impl* ptp, uint64_t* tai) {
  uint16_t port_id = ptp->port_id;
  int ret;
  struct timespec spec;

  if (ptp->no_timesync) {
    if (tai) *tai = ptp_no_timesync_time(ptp);
    return 0;
  }

  memset(&spec, 0, sizeof(spec));

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_tx_timestamp(port_id, &spec);
  ptp_timesync_unlock(ptp);

  if (ret < 0) dbg("%s(%d), err %d\n", __func__, ptp->port, ret);
  if (tai) *tai = mt_timespec_to_ns(&spec);
  return ret;
}

static inline int ptp_timesync_read_rx_time(struct mt_ptp_impl* ptp, uint32_t flags,
                                            uint64_t* tai) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  int ret;
  struct timespec spec;

  if (ptp->no_timesync) {
    if (tai) *tai = ptp_no_timesync_time(ptp);
    return 0;
  }

  memset(&spec, 0, sizeof(spec));

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_rx_timestamp(port_id, &spec, flags);
  ptp_timesync_unlock(ptp);

  if (ret < 0) err("%s(%d), err %d\n", __func__, port, ret);
  if (tai) *tai = mt_timespec_to_ns(&spec);
  return ret;
}

static inline int ptp_timesync_adjust_time(struct mt_ptp_impl* ptp, int64_t delta) {
  int ret;

  if (ptp->no_timesync) {
    ptp_no_timesync_adjust(ptp, delta);
    return 0;
  }

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_adjust_time(ptp->port_id, delta);
  ptp_timesync_unlock(ptp);

  return ret;
}

#ifdef MTL_HAS_DPDK_TIMESYNC_ADJUST_FREQ
static inline int ptp_timesync_adjust_freq(struct mt_ptp_impl* ptp, int64_t ppm,
                                           int64_t delta) {
  int ret;

  if (ptp->no_timesync) {
    ptp_no_timesync_adjust(ptp, delta);
    return 0;
  }

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_adjust_freq(ptp->port_id, ppm);
  ptp_timesync_unlock(ptp);

  if (ret) ptp_timesync_adjust_time(ptp, delta);

  return ret;
}
#endif

static inline uint64_t ptp_get_raw_time(struct mt_ptp_impl* ptp) {
  return ptp_timesync_read_time(ptp);
}

static inline uint64_t ptp_get_correct_time(struct mt_ptp_impl* ptp) {
  return ptp_correct_ts(ptp, ptp_get_raw_time(ptp));
}

static uint64_t ptp_from_eth(struct mtl_main_impl* impl, enum mtl_port port) {
  return ptp_get_correct_time(mt_get_ptp(impl, port));
}

static void ptp_print_port_id(enum mtl_port port, struct mt_ptp_port_id* pid) {
  uint8_t* id = &pid->clock_identity.id[0];
  info(
      "mt_ptp_port_id(%d), port_number: %04x, clk_id: "
      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
      port, pid->port_number, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

static inline bool ptp_port_id_equal(struct mt_ptp_port_id* s, struct mt_ptp_port_id* t) {
  if (!memcmp(s, t, sizeof(struct mt_ptp_port_id)))
    return true;
  else
    return false;
}

static struct rte_ether_addr ptp_l4_multicast_eaddr = {
    {0x01, 0x00, 0x5e, 0x00, 0x01, 0x81}}; /* 224.0.1.129 */

static struct rte_ether_addr ptp_l2_multicast_eaddr = {
    {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00}};

static inline void ptp_set_master_addr(struct mt_ptp_impl* ptp,
                                       struct rte_ether_addr* d_addr) {
  if (ptp->master_addr_mode == MT_PTP_MULTICAST_ADDR) {
    if (ptp->t2_mode == MT_PTP_L4)
      rte_ether_addr_copy(&ptp_l4_multicast_eaddr, d_addr);
    else
      rte_ether_addr_copy(&ptp_l2_multicast_eaddr, d_addr);
  } else {
    rte_ether_addr_copy(&ptp->master_addr, d_addr);
  }
}

static void ptp_coefficient_result_reset(struct mt_ptp_impl* ptp) {
  ptp->coefficient_result_sum = 0.0;
  ptp->coefficient_result_min = 2.0;
  ptp->coefficient_result_max = 0.0;
  ptp->coefficient_result_cnt = 0;
}

static void ptp_update_coefficient(struct mt_ptp_impl* ptp, int64_t error) {
  ptp->integral += (error + ptp->prev_error) / 2;
  ptp->prev_error = error;
  double offset = ptp->kp * error + ptp->ki * ptp->integral;
  if (ptp->t2_mode == MT_PTP_L4) offset /= 4; /* where sync interval is 0.25s for l4 */
  ptp->coefficient += RTE_MIN(RTE_MAX(offset, -1e-7), 1e-7);
  dbg("%s(%d), error %" PRId64 ", offset %.15lf\n", __func__, ptp->port, error, offset);
}

static void ptp_calculate_coefficient(struct mt_ptp_impl* ptp, int64_t delta) {
  if (delta > 1000 * 1000) return;
  uint64_t ts_s = ptp_get_raw_time(ptp);
  uint64_t ts_m = ts_s + delta;
  double coefficient = (double)(ts_m - ptp->last_sync_ts) / (ts_s - ptp->last_sync_ts);
  ptp->coefficient_result_sum += coefficient;
  ptp->coefficient_result_min = RTE_MIN(coefficient, ptp->coefficient_result_min);
  ptp->coefficient_result_max = RTE_MAX(coefficient, ptp->coefficient_result_max);
  ptp->coefficient_result_cnt++;
  if (ptp->coefficient - 1.0 < 1e-15) /* store first result */
    ptp->coefficient = coefficient;
  if (ptp->coefficient_result_cnt == 10) {
    /* get every 10 results' average */
    ptp->coefficient_result_sum -= ptp->coefficient_result_min;
    ptp->coefficient_result_sum -= ptp->coefficient_result_max;
    ptp->coefficient = ptp->coefficient_result_sum / 8;
    ptp_coefficient_result_reset(ptp);
  }
  ptp->last_sync_ts = ts_m;
  dbg("%s(%d), delta %" PRId64 ", co %.15lf, ptp %" PRIu64 "\n", __func__, ptp->port,
      delta, ptp->coefficient, ts_m);
}

static void ptp_adjust_delta(struct mt_ptp_impl* ptp, int64_t delta, bool error_correct) {
  MTL_MAY_UNUSED(error_correct);

#ifdef MTL_HAS_DPDK_TIMESYNC_ADJUST_FREQ
  double ppb;
  enum servo_state state = UNLOCKED;

  if (ptp->phc2sys_active) {
    if (!error_correct) {
      ppb = pi_sample(&ptp->servo, -1 * delta, ptp->t2, &state);

      switch (state) {
        case UNLOCKED:
          break;
        case JUMP:
          if (!ptp_timesync_adjust_time(ptp, delta))
            dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64
                " adjust time.\n",
                __func__, ptp->port_id, delta, ptp->path_delay);
          else
            err("%s(%d), PHC time adjust failed.\n", __func__, ptp->port_id);
          break;
        case LOCKED:
          if (!ptp_timesync_adjust_freq(ptp, -1 * (long)(ppb * 65.536), delta))
            dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64
                " adjust freq.\n",
                __func__, ptp->port_id, delta, ptp->path_delay);
          else
            err("%s(%d), PHC freqency adjust failed.\n", __func__, ptp->port_id);
          break;
      }
      phc2sys_adjust(ptp);
    }
  } else {
    if (!ptp_timesync_adjust_time(ptp, delta))
      dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64 " adjust time.\n",
          __func__, ptp->port_id, delta, ptp->path_delay);
    else
      err("%s(%d), PHC time adjust failed.\n", __func__, ptp->port_id);
  }
#else
  if (!ptp_timesync_adjust_time(ptp, delta))
    dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64 " adjust time.\n",
        __func__, ptp->port_id, delta, ptp->stat_path_delay_max);
  else
    err("%s(%d), PHC time adjust failed.\n", __func__, ptp->port_id);
  if (ptp->phc2sys_active) phc2sys_adjust(ptp);
#endif
  dbg("%s(%d), delta %" PRId64 ", ptp %" PRIu64 "\n", __func__, ptp->port, delta,
      ptp_get_raw_time(ptp));
  ptp->ptp_delta += delta;

  if (5 == ptp->delta_result_cnt) /* clear the first 5 results */
    ptp->delta_result_sum = labs(delta) * ptp->delta_result_cnt;
  else
    ptp->delta_result_sum += labs(delta);

  ptp->delta_result_cnt++;
  /* update status */
  ptp->stat_delta_min = RTE_MIN(delta, ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(delta, ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += labs(delta);

  if (!ptp->locked) {
    /*
     * Be considered as locked while the max delta is continuously below 100ns.
     */
    if (labs(ptp->stat_delta_max) < 100 && labs(ptp->stat_delta_max) > 0 &&
        labs(ptp->stat_delta_min) < 100 && labs(ptp->stat_delta_min) > 0) {
      if (ptp->stat_sync_keep > 100)
        ptp->locked = true;
      else
        ptp->stat_sync_keep++;
    } else {
      ptp->stat_sync_keep = 0;
    }
  }
}

static void ptp_delay_req_read_tx_time_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  uint64_t tx_ns = 0;
  int ret;

  ret = ptp_timesync_read_tx_time(ptp, &tx_ns);
  if (ret >= 0) {
    ptp->t3 = tx_ns;
  } else {
    if (!ptp->t4) rte_eal_alarm_set(5, ptp_delay_req_read_tx_time_handler, ptp);
  }
}

static void ptp_expect_result_clear(struct mt_ptp_impl* ptp) {
  ptp->expect_result_cnt = 0;
  ptp->expect_result_sum = 0;
  ptp->expect_correct_result_sum = 0;
  ptp->expect_t2_t1_delta_sum = 0;
  ptp->expect_t4_t3_delta_sum = 0;
  ptp->expect_result_start_ns = 0;
}

static void ptp_t_result_clear(struct mt_ptp_impl* ptp) {
  ptp->t1 = 0;
  ptp->t2 = 0;
  ptp->t3 = 0;
  ptp->t4 = 0;
}

static void ptp_result_reset(struct mt_ptp_impl* ptp) {
  ptp->delta_result_err = 0;
  ptp->delta_result_cnt = 0;
  ptp->delta_result_sum = 0;
  ptp->expect_result_avg = 0;
  ptp->expect_correct_result_avg = 0;
  ptp->expect_t2_t1_delta_avg = 0;
  ptp->expect_t2_t1_delta_avg = 0;
}

static int ptp_sync_expect_result(struct mt_ptp_impl* ptp) {
  if (ptp->expect_correct_result_avg) {
    if (ptp->use_pi) {
      /* fine tune coefficient */
      ptp_update_coefficient(ptp, ptp->expect_correct_result_avg);
      ptp->last_sync_ts =
          ptp_get_raw_time(ptp) + ptp->expect_result_avg; /* approximation */
    } else {
      /* re-calculate coefficient */
      ptp_calculate_coefficient(ptp, ptp->expect_result_avg);
    }
  }
  if (ptp->expect_result_avg) ptp_adjust_delta(ptp, ptp->expect_result_avg, true);
  return 0;
}

static void ptp_monitor_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  uint64_t expect_result_period_us = ptp->expect_result_period_ns / 1000;

  ptp->stat_sync_timeout_err++;

  ptp_sync_expect_result(ptp);
  if (expect_result_period_us) {
    dbg("%s(%d), next timer %" PRIu64 "\n", __func__, ptp->port, expect_result_period_us);
    rte_eal_alarm_set(expect_result_period_us, ptp_monitor_handler, ptp);
  }
}

static void ptp_sync_timeout_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  uint64_t expect_result_period_us = ptp->expect_result_period_ns / 1000;

  ptp_expect_result_clear(ptp);
  ptp_t_result_clear(ptp);
  ptp->stat_sync_timeout_err++;

  ptp_sync_expect_result(ptp);
  if (expect_result_period_us) {
    dbg("%s(%d), next timer %" PRIu64 "\n", __func__, ptp->port, expect_result_period_us);
    rte_eal_alarm_set(expect_result_period_us, ptp_monitor_handler, ptp);
  }
}

static int ptp_parse_result(struct mt_ptp_impl* ptp) {
  struct mtl_main_impl* impl = ptp->impl;
  int64_t t2_t1_delta = ((int64_t)ptp->t2 - ptp->t1);
  int64_t t4_t3_delta = ((int64_t)ptp->t4 - ptp->t3);

  dbg("%s(%d), t1 %" PRIu64 " t2 %" PRIu64 " t3 %" PRIu64 " t4 %" PRIu64 "\n", __func__,
      ptp->port, ptp->t1, ptp->t2, ptp->t3, ptp->t4);
  dbg("%s(%d), t2-t1 delta %" PRId64 " t4-t3 delta %" PRIu64 "\n", __func__, ptp->port,
      t2_t1_delta, t4_t3_delta);
  if (ptp->calibrate_t2_t3) {
    /* max 1us delta */
    int32_t max_diff = 1000;
    if (ptp->expect_t2_t1_delta_avg) { /* check t2_t1_delta */
      if (t2_t1_delta < (ptp->expect_t2_t1_delta_avg - max_diff) ||
          t2_t1_delta > (ptp->expect_t2_t1_delta_avg + max_diff)) {
        ptp->t2_t1_delta_continuous_err++;
        if (ptp->t2_t1_delta_continuous_err > 20) {
          err("%s(%d), t2_t1_delta %" PRId64 ", reset as too many continuous errors\n",
              __func__, ptp->port, t2_t1_delta);
        }
        t2_t1_delta = ptp->expect_t2_t1_delta_avg;
        ptp->t2 = ptp->t1 + t2_t1_delta; /* update t2 */
        ptp->stat_t2_t1_delta_calibrate++;

        if (ptp->t2_t1_delta_continuous_err > 20) {
          ptp->expect_t2_t1_delta_avg = 0;
          ptp->t2_t1_delta_continuous_err = 0;
          ptp_expect_result_clear(ptp);
        }
      } else {
        ptp->t2_t1_delta_continuous_err = 0;
      }
    }
    if (ptp->expect_t4_t3_delta_avg) { /* check t4_t3_delta */
      if (t4_t3_delta < (ptp->expect_t4_t3_delta_avg - max_diff) ||
          t4_t3_delta > (ptp->expect_t4_t3_delta_avg + max_diff)) {
        ptp->t4_t3_delta_continuous_err++;
        if (ptp->t4_t3_delta_continuous_err > 20) {
          err("%s(%d), t4_t3_delta %" PRId64 ", reset as too many continuous errors\n",
              __func__, ptp->port, t4_t3_delta);
        }
        t4_t3_delta = ptp->expect_t4_t3_delta_avg;
        ptp->t3 = ptp->t4 - t4_t3_delta; /* update t3 */
        ptp->stat_t4_t3_delta_calibrate++;

        if (ptp->t4_t3_delta_continuous_err > 20) {
          ptp->expect_t4_t3_delta_avg = 0;
          ptp->t4_t3_delta_continuous_err = 0;
          ptp_expect_result_clear(ptp);
        }
      } else {
        ptp->t4_t3_delta_continuous_err = 0;
      }
    }
  }

  int64_t delta = t4_t3_delta - t2_t1_delta;
  int64_t path_delay = t2_t1_delta + t4_t3_delta;
  uint64_t abs_delta, expect_delta;

  delta /= 2;

  path_delay /= 2;
  abs_delta = labs(delta);

  /* cancel the monitor */
  rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
  rte_eal_alarm_cancel(ptp_monitor_handler, ptp);
  if (ptp->delta_result_cnt) {
    expect_delta = abs(ptp->expect_result_avg) * (RTE_MIN(ptp->delta_result_err + 2, 5));
    if (!expect_delta) {
      expect_delta = ptp->delta_result_sum / ptp->delta_result_cnt * 2;
      expect_delta = RTE_MAX(expect_delta, 100 * 1000); /* min 100us */
    }
    if (abs_delta > expect_delta) {
#if MT_PTP_PRINT_ERR_RESULT
      err("%s(%d), error abs_delta %" PRIu64 "\n", __func__, ptp->port, abs_delta);
      err("%s(%d), t1 %" PRIu64 " t2 %" PRIu64 " t3 %" PRIu64 " t4 %" PRIu64 "\n",
          __func__, ptp->port, ptp->t1, ptp->t2, ptp->t3, ptp->t4);
#endif
      ptp_t_result_clear(ptp);
      ptp_expect_result_clear(ptp);
      ptp->delta_result_err++;
      ptp->stat_result_err++;
      if (ptp->delta_result_err > 10) {
        dbg("%s(%d), reset the result as too many errors\n", __func__, ptp->port);
        ptp_result_reset(ptp);
      }
      ptp_sync_expect_result(ptp);
#ifdef MTL_HAS_DPDK_TIMESYNC_ADJUST_FREQ
      if (!ptp->phc2sys_active) return -EIO;
#else
      return -EIO;
#endif
    }
  }
  ptp->delta_result_err = 0;

  /* measure frequency corrected delta */
  int64_t correct_delta = ((int64_t)ptp->t4 - ptp_correct_ts(ptp, ptp->t3)) -
                          ((int64_t)ptp_correct_ts(ptp, ptp->t2) - ptp->t1);
  correct_delta /= 2;
  dbg("%s(%d), correct_delta %" PRId64 "\n", __func__, ptp->port, correct_delta);
  /* update correct delta and path delay result */
  ptp->stat_correct_delta_min = RTE_MIN(correct_delta, ptp->stat_correct_delta_min);
  ptp->stat_correct_delta_max = RTE_MAX(correct_delta, ptp->stat_correct_delta_max);
  ptp->stat_correct_delta_cnt++;
  ptp->stat_correct_delta_sum += labs(correct_delta);
  ptp->stat_path_delay_min = RTE_MIN(path_delay, ptp->stat_path_delay_min);
  ptp->stat_path_delay_max = RTE_MAX(path_delay, ptp->stat_path_delay_max);
  ptp->stat_path_delay_cnt++;
  ptp->stat_path_delay_sum += labs(path_delay);

  if (ptp->use_pi && labs(correct_delta) < 1000) {
    /* fine tune coefficient */
    ptp_update_coefficient(ptp, correct_delta);
    ptp->last_sync_ts = ptp_get_raw_time(ptp) + delta; /* approximation */
  } else {
    /* re-calculate coefficient */
    ptp_calculate_coefficient(ptp, delta);
  }

  ptp_adjust_delta(ptp, delta, false);
  MT_USDT_PTP_RESULT(ptp->port, delta, correct_delta);
  ptp_t_result_clear(ptp);
  ptp->connected = true;

  /* notify the sync event if ptp_sync_notify is enabled */
  struct mtl_init_params* p = mt_get_user_params(impl);
  if (p->ptp_sync_notify && (MTL_PORT_P == ptp->port)) {
    struct mtl_ptp_sync_notify_meta meta;
    meta.master_utc_offset = ptp->master_utc_offset;
    meta.delta = delta;
    p->ptp_sync_notify(p->priv, &meta);
  }

  if (ptp->delta_result_cnt > 10) {
    if (labs(delta) < 30000) {
      ptp->expect_result_cnt++;
      if (!ptp->expect_result_start_ns)
        ptp->expect_result_start_ns = mt_get_monotonic_time();
      ptp->expect_result_sum += delta;
      ptp->expect_correct_result_sum += correct_delta;
      ptp->expect_t2_t1_delta_sum += t2_t1_delta;
      ptp->expect_t4_t3_delta_sum += t4_t3_delta;
      ptp->expect_result_sum += delta;
      if (ptp->expect_result_cnt >= 10) {
        ptp->expect_result_avg = ptp->expect_result_sum / ptp->expect_result_cnt;
        ptp->expect_correct_result_avg =
            ptp->expect_correct_result_sum / ptp->expect_result_cnt;
        ptp->expect_t2_t1_delta_avg =
            ptp->expect_t2_t1_delta_sum / ptp->expect_result_cnt;
        ptp->expect_t4_t3_delta_avg =
            ptp->expect_t4_t3_delta_sum / ptp->expect_result_cnt;
        ptp->expect_result_period_ns =
            (mt_get_monotonic_time() - ptp->expect_result_start_ns) /
            (ptp->expect_result_cnt - 1);
        dbg("%s(%d), expect result avg %d(correct: %d), t2_t1_delta %d, t4_t3_delta %d, "
            "period %fs\n",
            __func__, ptp->port, ptp->expect_result_avg, ptp->expect_correct_result_avg,
            ptp->expect_t2_t1_delta_avg, ptp->expect_t4_t3_delta_avg,
            (float)ptp->expect_result_period_ns / NS_PER_S);
        ptp_expect_result_clear(ptp);
      }
    } else {
      ptp_expect_result_clear(ptp);
    }
  }

  return 0;
}

static void ptp_delay_req_task(struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  size_t hdr_offset;
  struct mt_ptp_sync_msg* msg;
  uint64_t tx_ns = 0;

  if (ptp->t3) return; /* t3 already sent */

  struct rte_mbuf* m = rte_pktmbuf_alloc(ptp->mbuf_pool);
  if (!m) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, port);
    return;
  }

  if (ptp->t2_vlan) {
    err("%s(%d), todo for vlan\n", __func__, port);
  } else {
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
    m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;
#else
    m->ol_flags |= PKT_TX_IEEE1588_TMST;
#endif
  }

  struct rte_ether_hdr* hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  hdr_offset = sizeof(struct rte_ether_hdr);

  if (ptp->t2_mode == MT_PTP_L4) {
    struct mt_ipv4_udp* ipv4_hdr =
        rte_pktmbuf_mtod_offset(m, struct mt_ipv4_udp*, hdr_offset);
    hdr_offset += sizeof(struct mt_ipv4_udp);
    rte_memcpy(ipv4_hdr, &ptp->dst_udp, sizeof(*ipv4_hdr));
    ipv4_hdr->udp.src_port = htons(MT_PTP_UDP_EVENT_PORT);
    ipv4_hdr->udp.dst_port = ipv4_hdr->udp.src_port;
    ipv4_hdr->udp.dgram_cksum = 0;
    ipv4_hdr->ip.time_to_live = 255;
    ipv4_hdr->ip.fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
    ipv4_hdr->ip.next_proto_id = IPPROTO_UDP;
    ipv4_hdr->ip.hdr_checksum = 0;
    mt_mbuf_init_ipv4(m);
    hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  } else {
    hdr->ether_type = htons(RTE_ETHER_TYPE_1588);
  }

  msg = rte_pktmbuf_mtod_offset(m, struct mt_ptp_sync_msg*, hdr_offset);
  memset(msg, 0x0, sizeof(*msg));
  msg->hdr.message_type = PTP_DELAY_REQ;
  msg->hdr.version = 2;
  msg->hdr.message_length = htons(sizeof(struct mt_ptp_sync_msg));
  msg->hdr.domain_number = ptp->t1_domain_number;
  msg->hdr.log_message_interval = 0x7f;
  rte_memcpy(&msg->hdr.source_port_identity, &ptp->our_port_id,
             sizeof(struct mt_ptp_port_id));
  ptp->t3_sequence_id++;
  msg->hdr.sequence_id = htons(ptp->t3_sequence_id);

  mt_macaddr_get(ptp->impl, port, mt_eth_s_addr(hdr));
  ptp_set_master_addr(ptp, mt_eth_d_addr(hdr));
  m->pkt_len = hdr_offset + sizeof(struct mt_ptp_sync_msg);
  m->data_len = m->pkt_len;

  // mt_mbuf_dump(port, 0, "PTP_DELAY_REQ", m);
  uint16_t tx = mt_sys_queue_tx_burst(ptp->impl, port, &m, 1);
  if (tx < 1) {
    rte_pktmbuf_free(m);
    err("%s(%d), tx fail\n", __func__, port);
    return;
  }
#if MT_PTP_CHECK_HW_SW_DELTA
  uint64_t burst_time = ptp_get_raw_time(ptp);
#endif

#if MT_PTP_USE_TX_TIME_STAMP
  if (ptp->qbv_enabled) {
    /*
     * The DELAY_REQ packet will be blocked max 1.2ms by Qbv scheduler.
     * The Tx timestamp will not be created immediately. So, start an
     * alarm task to poll the Tx timestamp.
     */
    rte_eal_alarm_set(5, ptp_delay_req_read_tx_time_handler, ptp);
  } else {
    /* Wait max 50 us to read TX timestamp. */
    int max_retry = 50;
    int ret;

    while (max_retry > 0) {
      ret = ptp_timesync_read_tx_time(ptp, &tx_ns);
      if (ret >= 0) {
#if MT_PTP_CHECK_HW_SW_DELTA
        info("%s(%d), t3 hw-sw delta %" PRId64 "\n", __func__, ptp->port,
             tx_ns - burst_time);
#endif
        break;
      }

      mt_delay_us(1);
      max_retry--;
    }

    if (max_retry <= 0) {
      err("%s(%d), read tx reach max retry\n", __func__, port);
    }

#if MT_PTP_CHECK_TX_TIME_STAMP
    uint64_t ptp_ns = ptp_timesync_read_time(ptp);
    uint64_t delta = ptp_ns - tx_ns;
#define TX_MAX_DELTA (1 * 1000 * 1000) /* 1ms */
    if (unlikely(delta > TX_MAX_DELTA)) {
      err("%s(%d), tx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, tx_ns,
          delta);
      ptp->stat_tx_sync_err++;
    }
#endif

    ptp->t3 = tx_ns;
#else
  ptp->t3 = ptp_get_raw_time(ptp);
#endif
    dbg("%s(%d), t3 %" PRIu64 ", seq %d, max_retry %d, ptp %" PRIu64 "\n", __func__, port,
        ptp->t3, ptp->t3_sequence_id, max_retry, ptp_get_raw_time(ptp));
    MT_USDT_PTP_MSG(ptp->port, 3, ptp->t3);

    /* all time get */
    if (ptp->t4 && ptp->t2 && ptp->t1) {
      ptp_parse_result(ptp);
    }
  }
}

#if MT_PTP_USE_TX_TIMER
static void ptp_delay_req_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  return ptp_delay_req_task(ptp);
}
#endif

static int ptp_parse_sync(struct mt_ptp_impl* ptp, struct mt_ptp_sync_msg* msg, bool vlan,
                          enum mt_ptp_l_mode mode, uint16_t timesync) {
  uint64_t rx_ns = 0;
#define RX_MAX_DELTA (1 * 1000 * 1000) /* 1ms */

  ptp->stat_sync_cnt++;

  uint64_t monitor_period_us = ptp->expect_result_period_ns / 1000 / 2;
  if (monitor_period_us) {
    monitor_period_us = RTE_MAX(monitor_period_us, 100 * 1000 * 1000); /* min 100ms */
    if (ptp->t2) { /* already has a pending t2 */
      ptp_expect_result_clear(ptp);
      ptp_t_result_clear(ptp);
      ptp->stat_sync_timeout_err++;
      ptp_sync_expect_result(ptp);
    }
    rte_eal_alarm_cancel(ptp_monitor_handler, ptp);
    rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
    rte_eal_alarm_set(monitor_period_us, ptp_sync_timeout_handler, ptp);
  }

  ptp_timesync_read_rx_time(ptp, timesync, &rx_ns);
#if MT_PTP_CHECK_HW_SW_DELTA
  info("%s(%d), t2 hw-sw delta %" PRId64 "\n", __func__, ptp->port,
       ptp_get_raw_time(ptp) - rx_ns);
#endif

#if MT_PTP_CHECK_TX_TIME_STAMP
  uint64_t ptp_ns, delta;
  ptp_ns = ptp_timesync_read_time(ptp);
  delta = ptp_ns - rx_ns;
  if (unlikely(delta > RX_MAX_DELTA)) {
    err("%s(%d), rx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, rx_ns,
        delta);
    ptp->stat_rx_sync_err++;
  }
#endif

#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
#endif
  ptp_t_result_clear(ptp);
  ptp->t2 = rx_ns;
  ptp->t2_sequence_id = msg->hdr.sequence_id;
  ptp->t2_vlan = vlan;
  ptp->t2_mode = mode;
  dbg("%s(%d), t2 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t2,
      ptp->t2_sequence_id, ptp_get_raw_time(ptp));
  MT_USDT_PTP_MSG(ptp->port, 2, ptp->t2);

  return 0;
}

static int ptp_parse_follow_up(struct mt_ptp_impl* ptp,
                               struct mt_ptp_follow_up_msg* msg) {
  if (msg->hdr.sequence_id != ptp->t2_sequence_id) {
    dbg("%s(%d), error sequence id %d %d\n", __func__, ptp->port, msg->hdr.sequence_id,
        ptp->t2_sequence_id);
    return -EINVAL;
  }
  ptp->t1 = ptp_net_tmstamp_to_ns(&msg->precise_origin_timestamp) +
            (be64toh(msg->hdr.correction_field) >> 16);
  ptp->t1_domain_number = msg->hdr.domain_number;
  dbg("%s(%d), t1 %" PRIu64 ", ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t1,
      ptp_get_raw_time(ptp));
  MT_USDT_PTP_MSG(ptp->port, 1, ptp->t1);

#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_set(MT_PTP_DELAY_REQ_US + (ptp->port * MT_PTP_DELAY_STEP_US),
                    ptp_delay_req_handler, ptp);
#else
  ptp_delay_req_task(ptp);
#endif

  return 0;
}

static int ptp_parse_announce(struct mt_ptp_impl* ptp, struct mt_ptp_announce_msg* msg,
                              enum mt_ptp_l_mode mode, struct mt_ipv4_udp* ipv4_hdr) {
  enum mtl_port port = ptp->port;

  if (!ptp->master_initialized) {
    ptp->master_initialized = true;
    ptp->master_utc_offset = ntohs(msg->current_utc_offset);
    rte_memcpy(&ptp->master_port_id, &msg->hdr.source_port_identity,
               sizeof(ptp->master_port_id));
    rte_memcpy(&ptp->master_addr.addr_bytes[0], &ptp->master_port_id.clock_identity.id[0],
               3);
    rte_memcpy(&ptp->master_addr.addr_bytes[3], &ptp->master_port_id.clock_identity.id[5],
               3);
    info("%s(%d), master initialized, mode %s utc_offset %d domain_number %d\n", __func__,
         port, ptp_mode_str(mode), ptp->master_utc_offset, msg->hdr.domain_number);
    ptp_print_port_id(port, &ptp->master_port_id);
    if (mode == MT_PTP_L4) {
      struct mt_ipv4_udp* dst_udp = &ptp->dst_udp;

      rte_memcpy(dst_udp, ipv4_hdr, sizeof(*dst_udp));
      rte_memcpy(&dst_udp->ip.src_addr, &ptp->sip_addr[0], MTL_IP_ADDR_LEN);
      rte_memcpy(&dst_udp->ip.dst_addr, &ptp->mcast_group_addr[0], MTL_IP_ADDR_LEN);
      dst_udp->ip.total_length =
          htons(sizeof(struct mt_ipv4_udp) + sizeof(struct mt_ptp_sync_msg));
      dst_udp->ip.hdr_checksum = 0;
      dst_udp->udp.dgram_len =
          htons(sizeof(struct rte_udp_hdr) + sizeof(struct mt_ptp_sync_msg));
    }

    /* point ptp fn to eth phc */
    if (mt_user_ptp_tsc_source(ptp->impl))
      warn("%s(%d), skip as ptp force to tsc\n", __func__, port);
    else if (mt_user_ptp_time_fn(ptp->impl))
      warn("%s(%d), skip as user provide ptp source already\n", __func__, port);
    else
      mt_if(ptp->impl, port)->ptp_get_time_fn = ptp_from_eth;
  }

  return 0;
}

static int ptp_parse_delay_resp(struct mt_ptp_impl* ptp,
                                struct mt_ptp_delay_resp_msg* msg) {
  if (!ptp_port_id_equal(&msg->requesting_port_identity, &ptp->our_port_id)) {
    /* not our request resp */
    return 0;
  }

  if (ptp->t4) {
    dbg("%s(%d), t4 already get\n", __func__, ptp->port);
    return -EIO;
  }

  if (ptp->t3_sequence_id != ntohs(msg->hdr.sequence_id)) {
    err("%s(%d), mismatch sequence_id get %d expect %d\n", __func__, ptp->port,
        msg->hdr.sequence_id, ptp->t3_sequence_id);
    ptp->stat_t3_sequence_id_mismatch++;
    return -EIO;
  }
  ptp->t4 = ptp_net_tmstamp_to_ns(&msg->receive_timestamp) -
            (be64toh(msg->hdr.correction_field) >> 16);
  dbg("%s(%d), t4 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t4,
      ptp->t3_sequence_id, ptp_get_raw_time(ptp));
  MT_USDT_PTP_MSG(ptp->port, 4, ptp->t4);

  /* all time get */
  if (ptp->t3 && ptp->t2 && ptp->t1) {
    ptp_parse_result(ptp);
  }

  return 0;
}

static void ptp_stat_clear(struct mt_ptp_impl* ptp) {
  ptp->stat_delta_cnt = 0;
  ptp->stat_delta_sum = 0;
  ptp->stat_delta_min = INT_MAX;
  ptp->stat_delta_max = INT_MIN;
  ptp->stat_correct_delta_cnt = 0;
  ptp->stat_correct_delta_sum = 0;
  ptp->stat_correct_delta_min = INT_MAX;
  ptp->stat_correct_delta_max = INT_MIN;
  ptp->stat_path_delay_cnt = 0;
  ptp->stat_path_delay_sum = 0;
  ptp->stat_path_delay_min = INT_MAX;
  ptp->stat_path_delay_max = INT_MIN;
  ptp->stat_rx_sync_err = 0;
  ptp->stat_tx_sync_err = 0;
  ptp->stat_result_err = 0;
  ptp->stat_sync_timeout_err = 0;
  ptp->stat_sync_cnt = 0;
  if (ptp->phc2sys_active) ptp->phc2sys.stat_delta_max = 0;
}

static void ptp_sync_from_user(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint64_t target_ns = mt_get_ptp_time(impl, port);
  uint64_t raw_ns = ptp_get_raw_time(ptp);
  int64_t delta = (int64_t)target_ns - raw_ns;
  uint64_t abs_delta = labs(delta);
  uint64_t expect_abs_delta = abs(ptp->expect_result_avg) * 2;

  if (expect_abs_delta) {
    if (abs_delta > expect_abs_delta) delta = ptp->expect_result_avg;
  } else {
    if (abs_delta < 10000) {
      ptp->expect_result_sum += delta;
      ptp->expect_result_cnt++;
      if (ptp->expect_result_cnt > 1000) {
        ptp->expect_result_avg = ptp->expect_result_sum / ptp->expect_result_cnt;
        info("%s(%d), expect delta %d, sum %d\n", __func__, port, ptp->expect_result_avg,
             ptp->expect_result_sum);
      }
    }
  }

  ptp->delta_result_cnt++;
  ptp_timesync_adjust_time(ptp, delta);
  ptp->ptp_delta += delta;
  dbg("%s(%d), delta %" PRId64 "\n", __func__, port, delta);
  ptp->connected = true;

  /* update status */
  ptp->stat_delta_min = RTE_MIN(delta, ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(delta, ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += labs(delta);
}

static void ptp_sync_from_user_handler(void* param) {
  struct mt_ptp_impl* ptp = param;

  ptp_sync_from_user(ptp->impl, ptp);
  rte_eal_alarm_set(MT_PTP_TP_SYNC_MS * 1000, ptp_sync_from_user_handler, ptp);
}

#ifdef WINDOWSENV
int obtain_systime_privileges() {
  HANDLE hProcToken = NULL;
  TOKEN_PRIVILEGES tp = {0};
  LUID luid;

  if (!LookupPrivilegeValue(NULL, SE_SYSTEMTIME_NAME, &luid)) {
    err("%s, failed to lookup privilege value. hr=0x%08lx\n", __func__,
        HRESULT_FROM_WIN32(GetLastError()));
    return -1;
  }

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                        &hProcToken)) {
    err("%s, failed to open process token. hr=0x%08lx\n", __func__,
        HRESULT_FROM_WIN32(GetLastError()));
    return -1;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  if (!AdjustTokenPrivileges(hProcToken, FALSE, &tp, 0, NULL, NULL)) {
    err("%s, failed to adjust process token privileges. hr=0x%08lx\n", __func__,
        HRESULT_FROM_WIN32(GetLastError()));
    return -1;
  }

  if (hProcToken) CloseHandle(hProcToken);

  info("%s, succ\n", __func__);
  return 0;
}
#endif

static void phc2sys_init(struct mt_ptp_impl* ptp) {
  memset(&ptp->phc2sys.servo, 0, sizeof(struct mt_pi_servo));
  memset(&ptp->servo, 0, sizeof(struct mt_pi_servo));
#ifndef WINDOWSENV
  ptp->phc2sys.realtime_hz = sysconf(_SC_CLK_TCK);
#else
  /* init precise systime adjustment functions */
  HANDLE hDll;

  hDll = LoadLibrary("api-ms-win-core-sysinfo-l1-2-4.dll");
  win_get_systime_adj = (PGSTAP)GetProcAddress(hDll, "GetSystemTimeAdjustmentPrecise");
  win_set_systime_adj = (PSSTAP)GetProcAddress(hDll, "SetSystemTimeAdjustmentPrecise");

  if (obtain_systime_privileges()) return;

  /* set system internal adj */
  if (!(*win_set_systime_adj)(0, TRUE)) {
    err("failed to set the system time adjustment. hr:0x%08lx\n",
        HRESULT_FROM_WIN32(GetLastError()));
    return;
  }
#endif
  ptp->phc2sys.realtime_nominal_tick = 0;
  if (ptp->phc2sys.realtime_hz > 0) {
    ptp->phc2sys.realtime_nominal_tick =
        (1000000 + ptp->phc2sys.realtime_hz / 2) / ptp->phc2sys.realtime_hz;
  }
  ptp->phc2sys.locked = false;
  ptp->phc2sys.stat_sync_keep = 0;
  ptp->phc2sys_active = true;
  info("%s(%d), succ\n", __func__, ptp->port);
}

static int ptp_rxq_mbuf_handle(struct mt_ptp_impl* ptp, struct rte_mbuf* m) {
  struct mt_ipv4_udp* ipv4_hdr;
  struct mt_ptp_header* hdr;
  size_t hdr_offset;

  hdr_offset = sizeof(struct rte_ether_hdr);
  ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct mt_ipv4_udp*, hdr_offset);
  hdr_offset += sizeof(*ipv4_hdr);
  hdr = rte_pktmbuf_mtod_offset(m, struct mt_ptp_header*, sizeof(struct mt_udp_hdr));
  mt_ptp_parse(ptp, hdr, false, MT_PTP_L4, m->timesync, ipv4_hdr);

  return 0;
}

static int ptp_rxq_tasklet_handler(void* priv) {
  struct mt_ptp_impl* ptp = priv;
  uint16_t rx;
  struct rte_mbuf* pkt[MT_PTP_RX_BURST_SIZE];

  /* MT_PTP_UDP_GEN_PORT */
  rx = mt_rxq_burst(ptp->gen_rxq, &pkt[0], MT_PTP_RX_BURST_SIZE);
  for (uint16_t i = 0; i < rx; i++) {
    ptp_rxq_mbuf_handle(ptp, pkt[i]);
  }
  rte_pktmbuf_free_bulk(&pkt[0], rx);

  /* MT_PTP_UDP_EVENT_PORT */
  rx = mt_rxq_burst(ptp->event_rxq, &pkt[0], MT_PTP_RX_BURST_SIZE);
  for (uint16_t i = 0; i < rx; i++) {
    ptp_rxq_mbuf_handle(ptp, pkt[i]);
  }
  rte_pktmbuf_free_bulk(&pkt[0], rx);

  return 0;
}

static int ptp_init(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp,
                    enum mtl_port port) {
  uint16_t port_id = mt_port_id(impl, port);
  struct rte_ether_addr mac;
  int ret;
  uint8_t* ip = &ptp->sip_addr[0];
  struct mt_interface* inf = mt_if(impl, port);

  ret = mt_macaddr_get(impl, port, &mac);
  if (ret < 0) {
    err("%s(%d), macaddr get fail %d\n", __func__, port, ret);
    return ret;
  }

  uint16_t magic = MT_PTP_CLOCK_IDENTITY_MAGIC;
  struct mt_ptp_port_id* our_port_id = &ptp->our_port_id;
  uint8_t* id = &our_port_id->clock_identity.id[0];
  memcpy(&id[0], &mac.addr_bytes[0], 3);
  memcpy(&id[3], &magic, 2);
  memcpy(&id[5], &mac.addr_bytes[3], 3);
  our_port_id->port_number = htons(port_id);  // now always
  ptp_print_port_id(port_id, our_port_id);

  rte_memcpy(ip, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);

  ptp->impl = impl;
  ptp->port = port;
  ptp->port_id = port_id;
  ptp->mbuf_pool = mt_sys_tx_mempool(impl, port);
  ptp->master_initialized = false;
  ptp->t3_sequence_id = 0x1000 * port;
  ptp->coefficient = 1.0;
  ptp->kp = impl->user_para.kp < 1e-15 ? MT_PTP_DEFAULT_KP : impl->user_para.kp;
  ptp->ki = impl->user_para.ki < 1e-15 ? MT_PTP_DEFAULT_KI : impl->user_para.ki;
  ptp->use_pi = (impl->user_para.flags & MTL_FLAG_PTP_PI);
  if (ptp->use_pi)
    info("%s(%d), use pi controller, kp %e, ki %e\n", __func__, port, ptp->kp, ptp->ki);
  if (mt_user_phc2sys_service(impl) && (MTL_PORT_P == port)) phc2sys_init(ptp);

  struct mtl_init_params* p = mt_get_user_params(impl);
  if (p->flags & MTL_FLAG_PTP_UNICAST_ADDR) {
    ptp->master_addr_mode = MT_PTP_UNICAST_ADDR;
    info("%s(%d), MT_PTP_UNICAST_ADDR\n", __func__, port);
  } else {
    ptp->master_addr_mode = MT_PTP_MULTICAST_ADDR;
  }
  ptp->qbv_enabled =
      ((ST21_TX_PACING_WAY_TSN == p->pacing) && (MT_DRV_IGC == inf->drv_info.drv_type));
  ptp->locked = false;
  ptp->stat_sync_keep = 0;

  ptp_stat_clear(ptp);
  ptp_coefficient_result_reset(ptp);

  if (!mt_user_ptp_service(impl)) {
    if (mt_if_has_offload_timestamp(impl, port)) {
      if (!mt_if_has_timesync(impl, port)) {
        ptp->no_timesync = true;
        warn("%s(%d), ptp running without timesync support\n", __func__, port);
      }
      info("%s(%d), ptp sync from user for hw offload timestamp\n", __func__, port);
      ptp_sync_from_user(impl, ptp);
      rte_eal_alarm_set(MT_PTP_TP_SYNC_MS * 1000, ptp_sync_from_user_handler, ptp);
      ptp->connected = true;
      ptp->locked = true;
      ptp->active = true;
    }
    return 0;
  }

  if (mt_user_no_system_rxq(impl)) {
    warn("%s(%d), disabled as no system rx queues\n", __func__, port);
    return 0;
  }

  inet_pton(AF_INET, "224.0.1.129", ptp->mcast_group_addr);

  if (mt_has_cni(impl, port) && !mt_drv_mcast_in_dp(impl, port)) {
    /* join mcast only if cni path, no cni use socket which has mcast in the data path */
    ret = mt_mcast_join(impl, mt_ip_to_u32(ptp->mcast_group_addr), 0, port);
    if (ret < 0) {
      err("%s(%d), join ptp multicast group fail\n", __func__, port);
      return ret;
    }
    mt_mcast_l2_join(impl, &ptp_l2_multicast_eaddr, port);
  } else {
    /* create rx socket queue if no CNI path */
    struct mt_rxq_flow flow;
    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, ptp->mcast_group_addr, MTL_IP_ADDR_LEN);
    rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.flags = MT_RXQ_FLOW_F_FORCE_SOCKET;
    flow.dst_port = MT_PTP_UDP_GEN_PORT;

    ptp->gen_rxq = mt_rxq_get(impl, port, &flow);
    if (!ptp->gen_rxq) {
      warn("%s(%d), gen_rxq get fail\n", __func__, port);
      return 0; /* likely no permission, no ptp */
    }

    flow.dst_port = MT_PTP_UDP_EVENT_PORT;
    ptp->event_rxq = mt_rxq_get(impl, port, &flow);
    if (!ptp->event_rxq) {
      err("%s(%d), event_rxq get fail\n", __func__, port);
      return -ENOMEM;
    }

    struct mtl_tasklet_ops ops;
    memset(&ops, 0x0, sizeof(ops));
    ops.priv = ptp;
    ops.name = "ptp";
    ops.handler = ptp_rxq_tasklet_handler;
    ptp->rxq_tasklet = mtl_sch_register_tasklet(impl->main_sch, &ops);
    if (!ptp->rxq_tasklet) {
      err("%s(%d), rxq tasklet fail\n", __func__, port);
      mt_cni_uinit(impl);
      return -EIO;
    }
  }

  ptp->active = true;
  if (!mt_if_has_timesync(impl, port)) {
    ptp->no_timesync = true;
    ptp->calibrate_t2_t3 = true;
    warn("%s(%d), ptp running without timesync support\n", __func__, port);
  }
  info("%s(%d), sip: %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

static int ptp_uinit(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;

  rte_eal_alarm_cancel(ptp_sync_from_user_handler, ptp);
#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
#endif
  rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
  rte_eal_alarm_cancel(ptp_monitor_handler, ptp);
#ifdef MT_PTP_USE_TX_TIME_STAMP
  if (ptp->qbv_enabled) rte_eal_alarm_cancel(ptp_delay_req_read_tx_time_handler, ptp);
#endif

  if (!ptp->active) return 0;

  if (mt_has_cni(impl, port) && !mt_drv_mcast_in_dp(impl, port)) {
    mt_mcast_l2_leave(impl, &ptp_l2_multicast_eaddr, port);
    mt_mcast_leave(impl, mt_ip_to_u32(ptp->mcast_group_addr), 0, port);
  }

  if (ptp->rxq_tasklet) {
    mtl_sch_unregister_tasklet(ptp->rxq_tasklet);
    ptp->rxq_tasklet = NULL;
  }
  if (ptp->gen_rxq) {
    mt_rxq_put(ptp->gen_rxq);
    ptp->gen_rxq = NULL;
  }
  if (ptp->event_rxq) {
    mt_rxq_put(ptp->event_rxq);
    ptp->event_rxq = NULL;
  }

  info("%s(%d), succ\n", __func__, port);
  return 0;
}

int mt_ptp_parse(struct mt_ptp_impl* ptp, struct mt_ptp_header* hdr, bool vlan,
                 enum mt_ptp_l_mode mode, uint16_t timesync,
                 struct mt_ipv4_udp* ipv4_hdr) {
  enum mtl_port port = ptp->port;

  if (!ptp->active) return 0;

  dbg("%s(%d), message_type %d\n", __func__, port, hdr->message_type);
  // mt_ptp_print_port_id(port, &hdr->source_port_identity);

  if (hdr->message_type != PTP_ANNOUNCE) {
    if (!ptp->master_initialized) {
      dbg("%s(%d), master not initialized, message_type %d, mode %s\n", __func__, port,
          hdr->message_type, ptp_mode_str(mode));
      return -EINVAL;
    }
    if (!ptp_port_id_equal(&hdr->source_port_identity, &ptp->master_port_id)) {
      dbg("%s(%d), source_port_identity not our master, message_type %d, mode %s\n",
          __func__, port, hdr->message_type, ptp_mode_str(mode));
#ifdef DEBUG
      ptp_print_port_id(port, &hdr->source_port_identity);
#endif
      return -EINVAL;
    }
  }

  switch (hdr->message_type) {
    case PTP_SYNC:
      ptp_parse_sync(ptp, (struct mt_ptp_sync_msg*)hdr, vlan, mode, timesync);
      break;
    case PTP_FOLLOW_UP:
      ptp_parse_follow_up(ptp, (struct mt_ptp_follow_up_msg*)hdr);
      break;
    case PTP_DELAY_RESP:
      ptp_parse_delay_resp(ptp, (struct mt_ptp_delay_resp_msg*)hdr);
      break;
    case PTP_ANNOUNCE:
      ptp_parse_announce(ptp, (struct mt_ptp_announce_msg*)hdr, mode, ipv4_hdr);
      break;
    case PTP_DELAY_REQ:
      break;
    case PTP_PDELAY_REQ:
      break;
    default:
      err("%s(%d), unknown message_type %d\n", __func__, port, hdr->message_type);
      return -EINVAL;
  }

  /* read to clear rx timesync status */
  // struct timespec timestamp;
  // ptp_timesync_read_rx_time(ptp, &timestamp, timesync);
  return 0;
}

static int ptp_stat(void* priv) {
  struct mt_ptp_impl* ptp = priv;
  enum mtl_port port = ptp->port;
  char date_time[64];
  struct timespec spec;
  struct tm t;
  uint64_t ns;

  ns = mt_get_ptp_time(ptp->impl, port);
  mt_ns_to_timespec(ns, &spec);
  spec.tv_sec -= ptp->master_utc_offset; /* display with utc offset */
  localtime_r(&spec.tv_sec, &t);
  strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &t);
  notice("PTP(%d): time %" PRIu64 ", %s\n", port, ns, date_time);

  if (!ptp->active) return 0;

  if (ptp->stat_delta_cnt) {
    if (ptp->phc2sys_active) {
      notice("PTP(%d): system clock offset max %" PRId64 ", %s\n", port,
             ptp->phc2sys.stat_delta_max, ptp->phc2sys.locked ? "locked" : "not locked");
    }
    notice("PTP(%d): delta avg %" PRId64 ", min %" PRId64 ", max %" PRId64 ", cnt %d\n",
           port, ptp->stat_delta_sum / ptp->stat_delta_cnt, ptp->stat_delta_min,
           ptp->stat_delta_max, ptp->stat_delta_cnt);
  } else {
    notice("PTP(%d): not connected\n", port);
  }
  if (ptp->stat_correct_delta_cnt)
    notice("PTP(%d): correct_delta avg %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", cnt %d\n",
           port, ptp->stat_correct_delta_sum / ptp->stat_correct_delta_cnt,
           ptp->stat_correct_delta_min, ptp->stat_correct_delta_max,
           ptp->stat_correct_delta_cnt);
  if (ptp->stat_path_delay_cnt)
    notice("PTP(%d): path_delay avg %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", cnt %d\n",
           port, ptp->stat_path_delay_sum / ptp->stat_path_delay_cnt,
           ptp->stat_path_delay_min, ptp->stat_path_delay_max, ptp->stat_path_delay_cnt);
  notice(
      "PTP(%d): mode %s, sync cnt %d, expect avg %d:%d@%fs t2_t1_delta %d t4_t3_delta "
      "%d\n",
      port, ptp_mode_str(ptp->t2_mode), ptp->stat_sync_cnt, ptp->expect_result_avg,
      ptp->expect_correct_result_avg, (float)ptp->expect_result_period_ns / NS_PER_S,
      ptp->expect_t2_t1_delta_avg, ptp->expect_t4_t3_delta_avg);
  if (ptp->stat_rx_sync_err || ptp->stat_result_err || ptp->stat_tx_sync_err)
    notice("PTP(%d): rx time error %d, tx time error %d, delta result error %d\n", port,
           ptp->stat_rx_sync_err, ptp->stat_tx_sync_err, ptp->stat_result_err);
  if (ptp->stat_sync_timeout_err)
    err("PTP(%d): sync timeout %d\n", port, ptp->stat_sync_timeout_err);

  if (ptp->calibrate_t2_t3) {
    notice("PTP(%d): t2_t1_delta_calibrate %d t4_t3_delta_calibrate %d\n", port,
           ptp->stat_t2_t1_delta_calibrate, ptp->stat_t4_t3_delta_calibrate);
    ptp->stat_t2_t1_delta_calibrate = 0;
    ptp->stat_t4_t3_delta_calibrate = 0;
  }
  if (ptp->stat_t3_sequence_id_mismatch) {
    err("PTP(%d): t3 sequence id mismatch %d\n", port, ptp->stat_t3_sequence_id_mismatch);
    ptp->stat_t3_sequence_id_mismatch = 0;
  }

  ptp_stat_clear(ptp);

  return 0;
}

int mt_ptp_init(struct mtl_main_impl* impl) {
  int socket = mt_socket_id(impl, MTL_PORT_P);
  int ret;
  int num_port = mt_num_ports(impl);

  for (int i = 0; i < num_port; i++) {
    /* only probe on the MTL_PORT_P */
    if ((i != MTL_PORT_P) && !mt_if_has_offload_timestamp(impl, i)) continue;

    struct mt_ptp_impl* ptp = mt_rte_zmalloc_socket(sizeof(*ptp), socket);
    if (!ptp) {
      err("%s(%d), ptp malloc fail\n", __func__, i);
      mt_ptp_uinit(impl);
      return -ENOMEM;
    }

    ret = ptp_init(impl, ptp, i);
    if (ret < 0) {
      err("%s(%d), ptp_init fail %d\n", __func__, i, ret);
      mt_rte_free(ptp);
      mt_ptp_uinit(impl);
      return ret;
    }

    mt_stat_register(impl, ptp_stat, ptp, "ptp");

    /* assign ptp instance */
    impl->ptp[i] = ptp;
  }

  return 0;
}

int mt_ptp_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_ptp_impl* ptp;

  for (int i = 0; i < num_ports; i++) {
    ptp = mt_get_ptp(impl, i);
    if (!ptp) continue;

    mt_stat_unregister(impl, ptp_stat, ptp);

    ptp_uinit(impl, ptp);

    /* free the memory */
    mt_rte_free(ptp);
    impl->ptp[i] = NULL;
  }

  return 0;
}

uint64_t mt_get_raw_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  return ptp_get_raw_time(mt_get_ptp(impl, port));
}

static uint64_t mbuf_hw_time_stamp(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                                   enum mtl_port port) {
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  uint64_t time_stamp =
      *RTE_MBUF_DYNFIELD(mbuf, impl->dynfield_offset, rte_mbuf_timestamp_t*);
  time_stamp += ptp->ptp_delta;
  return ptp_correct_ts(ptp, time_stamp);
}

uint64_t mt_mbuf_time_stamp(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                            enum mtl_port port) {
  if (mt_if_has_offload_timestamp(impl, port))
    return mbuf_hw_time_stamp(impl, mbuf, port);
  else
    return mtl_ptp_read_time(impl);
}

int mt_ptp_wait_stable(struct mtl_main_impl* impl, enum mtl_port port, int timeout_ms) {
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  uint64_t start_ts = mt_get_tsc(impl);
  int retry = 0;

  if (!ptp->active) return 0;

  while (ptp->delta_result_cnt <= 5) {
    if (mt_aborted(impl)) {
      err("%s, fail as user aborted\n", __func__);
      return -EIO;
    }

    if (timeout_ms >= 0) {
      int ms = (mt_get_tsc(impl) - start_ts) / NS_PER_MS;
      if (ms > timeout_ms) {
        err("%s(%d), fail as timeout to %d ms\n", __func__, port, timeout_ms);
        return -ETIMEDOUT;
      }
    }
    retry++;
    if (0 == (retry % 500))
      info("%s(%d), wait PTP stable, timeout %d ms\n", __func__, port, timeout_ms);
    mt_sleep_ms(10);
  }

  return 0;
}

uint64_t mt_ptp_internal_time(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);

  if (!ptp->active) {
    err("%s(%d), ptp not active\n", __func__, port);
    return 0;
  }

  return ptp_get_correct_time(ptp);
}