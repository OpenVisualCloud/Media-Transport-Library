/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — JSON file loading helper
 */

#ifndef POC_JSON_UTILS_H
#define POC_JSON_UTILS_H

#include <stddef.h>

/* Load entire file into malloc'd buffer.  Caller must free().
 * Returns NULL on error. */
char* poc_load_file(const char* path, size_t* out_len);

#endif /* POC_JSON_UTILS_H */
