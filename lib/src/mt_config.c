/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_config.h"

#include "mt_log.h"

static int config_parse_plugins(struct mtl_main_impl* impl, json_object* plugins_array) {
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
    obj = mt_json_object_get(plugin_obj, "enabled");
    if (obj && !json_object_get_boolean(obj)) continue;
    const char* name = NULL;
    obj = mt_json_object_get(plugin_obj, "name");
    if (obj) name = json_object_get_string(obj);
    const char* path = NULL;
    obj = mt_json_object_get(plugin_obj, "path");
    if (obj) path = json_object_get_string(obj);
    if (!name || !path) continue;
    st_plugin_register(impl, path);
  }

  return 0;
}

static int config_parse_json(struct mtl_main_impl* impl, const char* filename) {
  json_object* root_object = json_object_from_file(filename);
  if (root_object == NULL) {
    warn("%s, open json file %s fail\n", __func__, filename);
    return -EIO;
  }
  info("%s, parse %s with json-c version: %s\n", __func__, filename, json_c_version());

  /* parse plugins for system */
  json_object* plugins_array = mt_json_object_get(root_object, "plugins");
  if (plugins_array) config_parse_plugins(impl, plugins_array);

  json_object_put(root_object);
  return 0;
}

int mt_config_init(struct mtl_main_impl* impl) {
  const char* cfg_path = getenv("KAHAWAI_CFG_PATH");

  if (cfg_path) {
    info("%s, KAHAWAI_CFG_PATH: %s\n", __func__, cfg_path);
    config_parse_json(impl, cfg_path);
  } else {
    config_parse_json(impl, "kahawai.json");
  }

  return 0;
}

int mt_config_uinit(struct mtl_main_impl* impl) {
  MTL_MAY_UNUSED(impl);
  return 0;
}
