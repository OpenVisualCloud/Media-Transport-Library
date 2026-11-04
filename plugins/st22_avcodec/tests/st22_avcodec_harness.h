/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef PLUGINS_ST22_AVCODEC_TESTS_ST22_AVCODEC_HARNESS_H
#define PLUGINS_ST22_AVCODEC_TESTS_ST22_AVCODEC_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opens a new H.264 encoder session of the plugin, encodes one flat 128x64 frame into a
 * codestream buffer of buffer_size bytes at dst and closes the session. Returns what
 * avcodec_encode_frame() returns, or 1 if the session does not open. */
int ut_avcodec_encode_frame(uint8_t* dst, size_t buffer_size, size_t* data_size);

#ifdef __cplusplus
}
#endif

#endif
