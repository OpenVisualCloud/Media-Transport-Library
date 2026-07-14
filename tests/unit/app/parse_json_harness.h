/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for st_json_parse_tx_fmd() fps-string validation unit tests.
 */

#ifndef _UT_PARSE_JSON_HARNESS_H_
#define _UT_PARSE_JSON_HARNESS_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Build a minimal, otherwise-valid tx-fastmetadata JSON session object with
 * "fastmetadata_fps" set to `fps_str` and call st_json_parse_tx_fmd() on it.
 * Returns its return code (0 = success, negative = ST_JSON_* error). */
int ut_parse_tx_fmd_fps(const char* fps_str);

/* The negative return code st_json_parse_tx_fmd() uses for an unrecognized
 * fastmetadata_fps string (-ST_JSON_NOT_VALID). */
int ut_parse_json_not_valid_rc(void);

#ifdef __cplusplus
}
#endif

#endif /* _UT_PARSE_JSON_HARNESS_H_ */
