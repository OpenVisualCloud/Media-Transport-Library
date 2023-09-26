/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_DEV_AF_XDP_HEAD_H_
#define _MT_LIB_DEV_AF_XDP_HEAD_H_

#include "../mt_main.h"

#ifdef MTL_HAS_XDP_BACKEND

int mt_dev_xdp_init(struct mt_interface* inf);
int mt_dev_xdp_uinit(struct mt_interface* inf);

#else

static inline int mt_dev_xdp_init(struct mt_interface* inf) {
  MTL_MAY_UNUSED(inf);
  return -ENOTSUP;
}

static inline int mt_dev_xdp_uinit(struct mt_interface* inf) {
  MTL_MAY_UNUSED(inf);
  return -ENOTSUP;
}
#endif

#endif
