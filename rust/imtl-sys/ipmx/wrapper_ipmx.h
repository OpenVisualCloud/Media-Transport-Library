/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <mtl/ipmx/ipmx_api.h>

/* bindgen cannot expand the MTL_BIT32() flag macros, but evaluates enum values */
/* bindgen makes this carrier enum public: use the ST2x_*_FLAG_IPMX re-exports of
 * src/ipmx.rs */
enum ipmx_sys_flag {
  IPMX_SYS_ST20_TX_FLAG = ST20_TX_FLAG_IPMX,
  IPMX_SYS_ST20_RX_FLAG = ST20_RX_FLAG_IPMX,
  IPMX_SYS_ST22_TX_FLAG = ST22_TX_FLAG_IPMX,
  IPMX_SYS_ST22_RX_FLAG = ST22_RX_FLAG_IPMX,
};
