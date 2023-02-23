/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ST_MAIN_H_
#define _MT_LIB_ST_MAIN_H_

#include "../mt_dev.h"
#include "../mt_dma.h"
#include "../mt_main.h"
#include "../mt_mcast.h"
#include "../mt_rss.h"
#include "../mt_sch.h"
#include "../mt_simd.h"
#include "../mt_util.h"

static inline enum st21_tx_pacing_way st_tx_pacing_way(struct mtl_main_impl* impl,
                                                       enum mtl_port port) {
  return mt_if(impl, port)->tx_pacing_way;
}

#endif
