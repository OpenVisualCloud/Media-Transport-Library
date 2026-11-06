/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* Compiles lib/src/ipmx/ipmx.h as a build without enable_ipmx sees it. */

#undef MTL_HAS_IPMX
#undef MTL_HAS_USDT
#include "ipmx/ipmx_stub_harness.h"

#include "ipmx/ipmx.h"

int ut_ipmx_stub_tx_ops_check(const struct st20_tx_ops* ops) {
  return ipmx_tx_ops_check(ops);
}

int ut_ipmx_stub_rx_ops_check(const struct st20_rx_ops* ops) {
  return ipmx_rx_ops_check(ops);
}

int ut_ipmx_stub_st22_tx_ops_check(const struct st22_tx_ops* ops) {
  return ipmx_st22_tx_ops_check(ops);
}

int ut_ipmx_stub_st22_rx_ops_check(const struct st22_rx_ops* ops) {
  return ipmx_st22_rx_ops_check(ops);
}

int ut_ipmx_stub_st22p_codec_check(enum st22_codec codec) {
  return ipmx_st22p_codec_check("ut", codec);
}

int ut_ipmx_stub_st22p_tx_ops_check(const struct st22p_tx_ops* ops) {
  return ipmx_st22p_tx_ops_check(ops);
}

int ut_ipmx_stub_jxs_boxes_fill(void) {
  return ipmx_jxs_boxes_fill(NULL, 1920, 1080, ST_FPS_P59_94, 1920 * 1080 * 3 / 8);
}

int ut_ipmx_stub_jxs_frame_check(void) {
  uint8_t cs[64] = {0};
  uint32_t fail_cnt = 0;
  return ipmx_jxs_frame_check("ut", 0, NULL, cs, sizeof(cs), 1920, 1080, &fail_cnt);
}
