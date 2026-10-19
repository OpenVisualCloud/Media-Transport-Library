/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_AVCODEC_ST22_AVCODEC_HARNESS_H
#define TESTS_UNIT_AVCODEC_ST22_AVCODEC_HARNESS_H

#include <mtl/st_pipeline_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Clears the packet the stub encoder returns and zeroes its reference count. */
void ut_avcodec_reset(void);
/** Each avcodec_receive_packet() until ut_avcodec_reset() returns size bytes at data. */
void ut_avcodec_set_packet(uint8_t* data, int size);
/** Packets the stub encoder handed out that were not unreferenced yet. */
int ut_avcodec_packet_refs(void);
/** Runs the plugin's avcodec_encode_frame() on a small YUV422P source into dst. */
int ut_avcodec_encode_frame(struct st_frame* dst);

#ifdef __cplusplus
}
#endif

#endif
