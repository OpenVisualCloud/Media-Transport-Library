/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — JSON file loading helper
 */

#include <stdio.h>
#include <stdlib.h>

#include "poc_json_utils.h"

char* poc_load_file(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);

  char* buf = (char*)malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t rd = fread(buf, 1, (size_t)len, f);
  fclose(f);

  buf[rd] = '\0';
  if (out_len) *out_len = rd;
  return buf;
}
