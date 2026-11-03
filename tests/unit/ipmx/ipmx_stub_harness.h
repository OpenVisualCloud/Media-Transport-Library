/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_STUB_HARNESS_H_
#define _IPMX_STUB_HARNESS_H_

#include "st20_api.h"
#include "st_pipeline_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the ipmx_*_ops_check() a build without enable_ipmx compiles */
int ut_ipmx_stub_tx_ops_check(const struct st20_tx_ops* ops);
int ut_ipmx_stub_rx_ops_check(const struct st20_rx_ops* ops);
int ut_ipmx_stub_st22_tx_ops_check(const struct st22_tx_ops* ops);
int ut_ipmx_stub_st22_rx_ops_check(const struct st22_rx_ops* ops);
int ut_ipmx_stub_st22p_codec_check(enum st22_codec codec);

#ifdef __cplusplus
}
#endif

#endif
