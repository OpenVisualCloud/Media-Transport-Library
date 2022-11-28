/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_MT_HEAD_H_
#define _MT_LIB_MT_HEAD_H_

enum mt_handle_type {
  MT_HANDLE_UNKNOWN = 0,
  MT_HANDLE_MAIN,

  MT_HANDLE_TX_VIDEO,
  MT_HANDLE_TX_AUDIO,
  MT_HANDLE_TX_ANC,
  MT_HANDLE_RX_VIDEO,
  MT_HANDLE_RX_AUDIO,
  MT_HANDLE_RX_ANC,
  MT_HANDLE_RX_VIDEO_R,
  MT_ST22_HANDLE_TX_VIDEO,
  MT_ST22_HANDLE_RX_VIDEO,
  MT_ST22_HANDLE_PIPELINE_TX,
  MT_ST22_HANDLE_PIPELINE_RX,
  MT_ST22_HANDLE_PIPELINE_ENCODE,
  MT_ST22_HANDLE_PIPELINE_DECODE,
  MT_ST20_HANDLE_PIPELINE_TX,
  MT_ST20_HANDLE_PIPELINE_RX,
  MT_ST20_HANDLE_PIPELINE_CONVERT,
  MT_ST22_HANDLE_DEV_ENCODE,
  MT_ST22_HANDLE_DEV_DECODE,
  MT_ST20_HANDLE_DEV_CONVERT,

  MT_HANDLE_UDMA,
  MT_HANDLE_MAX,
};

enum mt_session_port {
  MT_SESSION_PORT_P = 0, /* primary session(logical) port */
  MT_SESSION_PORT_R,     /* redundant session(logical) port */
  MT_SESSION_PORT_MAX,
};

#endif
