/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for RxTxApp's parse_json.c. Includes the production .c directly
 * so the static st_json_parse_tx_fmd() becomes visible in this translation
 * unit, and stubs the app log-level accessors it calls through err()/info().
 */

#include "app/parse_json_harness.h"

#include <string.h>

#include "parse_json.c"

void app_set_log_level(enum mtl_log_level level) {
  (void)level;
}

enum mtl_log_level app_get_log_level(void) {
  return MTL_LOG_LEVEL_ERR;
}

int ut_parse_tx_fmd_fps(const char* fps_str) {
  json_object* obj = json_object_new_object();
  json_object_object_add(obj, "start_port", json_object_new_int(20000));
  json_object_object_add(obj, "type", json_object_new_string("frame"));
  json_object_object_add(obj, "fastmetadata_data_item_type", json_object_new_int(1));
  json_object_object_add(obj, "fastmetadata_k_bit", json_object_new_int(0));
  json_object_object_add(obj, "fastmetadata_fps", json_object_new_string(fps_str));
  json_object_object_add(obj, "fastmetadata_url", json_object_new_string("test.txt"));

  st_json_fastmetadata_session_t fmd;
  memset(&fmd, 0, sizeof(fmd));
  int rc = st_json_parse_tx_fmd(0, obj, &fmd);

  json_object_put(obj);
  return rc;
}

int ut_parse_json_not_valid_rc(void) {
  return -ST_JSON_NOT_VALID;
}
