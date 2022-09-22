/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <obs/obs-module.h>
#include <obs/util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("linux-kahawai", "en-US")
MODULE_EXPORT const char* obs_module_description(void) {
  return "Kahawai(st2110) sources";
}

extern struct obs_source_info kahawai_input;

bool obs_module_load(void) {
  obs_register_source(&kahawai_input);

  obs_data_t* obs_settings = obs_data_create();

  obs_apply_private_data(obs_settings);
  obs_data_release(obs_settings);

  return true;
}
