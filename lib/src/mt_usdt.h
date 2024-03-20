/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _MT_LIB_USDT_HEAD_H_
#define _MT_LIB_USDT_HEAD_H_

#ifdef MTL_HAS_USDT
#include <sys/sdt.h>

#define MT_DTRACE_PROBE(provider, probe) DTRACE_PROBE(provider, probe)
#define MT_DTRACE_PROBE1(provider, probe, parm1) DTRACE_PROBE1(provider, probe, parm1)
#define MT_DTRACE_PROBE2(provider, probe, parm1, parm2) \
  DTRACE_PROBE2(provider, probe, parm1, parm2)
#define MT_DTRACE_PROBE3(provider, probe, parm1, parm2, parm3) \
  DTRACE_PROBE3(provider, probe, parm1, parm2, parm3)
#else

#define MT_DTRACE_PROBE(provider, probe) \
  do {                                   \
  } while (0)
#define MT_DTRACE_PROBE1(provider, probe, parm1) \
  do {                                           \
  } while (0)
#define MT_DTRACE_PROBE2(provider, probe, parm1, parm2) \
  do {                                                  \
  } while (0)
#define MT_DTRACE_PROBE3(provider, probe, parm1, parm2, parm3) \
  do {                                                         \
  } while (0)

#endif

#define MT_PTP_MSG_PROBE(port, stage, value) \
  MT_DTRACE_PROBE3(ptp, ptp_msg, port, stage, value)
#define MT_PTP_RESULT_PROBE(port, delta, correct) \
  MT_DTRACE_PROBE3(ptp, ptp_result, port, delta, correct)

#endif
