/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "st_config.h"

#include <json-c/json.h>

#include "st_log.h"

#if (JSON_C_VERSION_NUM >= ((0 << 16) | (13 << 8) | 0)) || \
    (JSON_C_VERSION_NUM < ((0 << 16) | (10 << 8) | 0))
static inline json_object* _json_object_get(json_object* obj, const char* key) {
  return json_object_object_get(obj, key);
}
#else
static inline json_object* _json_object_get(json_object* obj, const char* key) {
  json_object* value;
  int ret = json_object_object_get_ex(obj, key, &value);
  if (ret) return value;
  err("%s, can not get object with key: %s!\n", __func__, key);
  return NULL;
}
#endif

static int config_parse_plugins(struct st_main_impl* impl, json_object* plugins_array) {
  if (json_object_get_type(plugins_array) != json_type_array) {
    err("%s, type not array\n", __func__);
    return -EIO;
  }

  int num_plugins = json_object_array_length(plugins_array);
  dbg("%s, num_plugins %d\n", __func__, num_plugins);
  for (int i = 0; i < num_plugins; i++) {
    json_object* plugin_obj = json_object_array_get_idx(plugins_array, i);
    if (!plugin_obj) continue;
    json_object* obj;
    obj = _json_object_get(plugin_obj, "enabled");
    if (obj && !json_object_get_boolean(obj)) continue;
    const char* name = NULL;
    obj = _json_object_get(plugin_obj, "name");
    if (obj) name = json_object_get_string(obj);
    const char* path = NULL;
    obj = _json_object_get(plugin_obj, "path");
    if (obj) path = json_object_get_string(obj);
    if (!name || !path) continue;
    st_plugin_register(impl, path);
  }

  return 0;
}

static int config_parse_json(struct st_main_impl* impl, const char* filename) {
  json_object* root_object = json_object_from_file(filename);
  if (root_object == NULL) {
    warn("%s, open json file %s fail\n", __func__, filename);
    return -EIO;
  }
  info("%s, parse %s with json-c version: %s\n", __func__, filename, json_c_version());

  /* parse plugins for system */
  json_object* plugins_array = _json_object_get(root_object, "plugins");
  if (plugins_array) config_parse_plugins(impl, plugins_array);

  json_object_put(root_object);
  return 0;
}

int st_config_init(struct st_main_impl* impl) {
  const char* cfg_path = getenv("KAHAWAI_CFG_PATH");

  if (cfg_path) {
    info("%s, KAHAWAI_CFG_PATH: %s\n", __func__, cfg_path);
    config_parse_json(impl, cfg_path);
  } else {
    config_parse_json(impl, "kahawai.json");
  }

  return 0;
}

int st_config_uinit(struct st_main_impl* impl) { return 0; }
