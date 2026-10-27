/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/**
 * @file ipmx_api.h
 *
 * Interfaces for IPMX (VSF TR-10), only present in a library built with enable_ipmx.
 *
 */

#include "../st_pipeline_api.h"

#ifndef _IPMX_API_HEAD_H_
/** Macro for re-include protect */
#define _IPMX_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * IPMX sender timing seen by one RX session port, VSF TR-10-1 section 8.1 only (not a
 * TP-1 section 13.5 pass, which also needs the TR-10-9 section 11.2 RTCP and first
 * packet timing), accumulated since the timing parser started. Levels are in packets.
 *
 * A frame is judged, CINST included, only if the port had delivered all of it when the
 * session completed the frame: a burst that makes the RX drop packets shows as
 * unmeasured, not failed, and with ST 2022-7 the later port is usually unmeasured.
 * Meaningful only with NIC RX timestamps; with software ones untimed packets make CINST
 * read low.
 *
 * Known limitation: the VRX drain assumes height / vtotal = 1080 / 1125, so a sender with
 * other blanking, e.g. CVT-RB 1200 / 1235, can read as VRX underflow.
 */
struct ipmx_rx_tp_stat {
  /** CINST limit, MAX(16, INT(NPACKETS / (21600 x TFRAME))) */
  int32_t cmax;
  /** VRX limit, CMAX x 2 */
  int32_t vrx_full;
  /** the max CINST measured, 0 until a frame is measured */
  int32_t cinst_max;
  /** the max VRX level measured, INT32_MIN until a frame is measured */
  int32_t vrx_max;
  /**
   * the min VRX level measured, INT32_MAX until measured. Below 0 is an underflow and
   * fails the frame, per the ST 2110-21 VRX rules TR-10-1 refers to; stricter than the
   * TR-10 TP-1 pass criteria, which check only the max
   */
  int32_t vrx_min;
  /** frames within all the limits */
  uint32_t frames_compliant;
  /** frames outside a limit */
  uint32_t frames_failed;
  /** frames not judged: not all delivered by this port, or no packet timed */
  uint32_t frames_unmeasured;
  /** the cause of the last failed frame, empty if none failed */
  char failed_cause[64];
};

/**
 * Get the IPMX timing result of an st20 RX session created with ST20_RX_FLAG_IPMX and
 * ST20_RX_FLAG_TIMING_PARSER_STAT or ST20_RX_FLAG_TIMING_PARSER_META. The RX tasklet
 * keeps updating it, so a read may mix two frames.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param port
 *   The session port.
 * @param stat
 *   The pointer to save the result.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX or the timing parser not enabled on the session.
 *   - -EBUSY: the timing parser not started yet, it waits for the stream detection.
 *   - -EINVAL: port not used by the session.
 */
int ipmx_st20_rx_get_tp_stat(st20_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_tp_stat* stat);

/**
 * ipmx_st20_rx_get_tp_stat() of the transport session of an st20p RX pipeline created
 * with ST20P_RX_FLAG_IPMX and ST20P_RX_FLAG_TIMING_PARSER_STAT or _META.
 */
int ipmx_st20p_rx_get_tp_stat(st20p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_tp_stat* stat);

#if defined(__cplusplus)
}
#endif

#endif
