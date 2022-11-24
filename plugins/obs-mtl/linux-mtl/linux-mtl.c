/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "linux-mtl.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("linux-mtl", "en-US")
MODULE_EXPORT const char* obs_module_description(void) {
  return "Linux MTL input/output";
}

extern struct obs_source_info mtl_input;
extern struct obs_output_info mtl_output;

bool obs_module_load(void) {
  obs_register_source(&mtl_input);
  obs_register_output(&mtl_output);

  obs_data_t* obs_settings = obs_data_create();

  obs_apply_private_data(obs_settings);
  obs_data_release(obs_settings);

  return true;
}

enum st_frame_fmt obs_to_mtl_format(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_UYVY: /* UYVY can be converted from YUV422BE10 */
      return ST_FRAME_FMT_UYVY;
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
      return ST_FRAME_FMT_YUV420CUSTOM8;
    case VIDEO_FORMAT_YUY2:
    case VIDEO_FORMAT_YVYU:
      return ST_FRAME_FMT_YUV422CUSTOM8;
    default:
      return ST_FRAME_FMT_MAX;
  }
}

enum st_fps obs_to_mtl_fps(uint32_t fps_num, uint32_t fps_den) {
  switch (fps_num) {
    case 30000:
      return ST_FPS_P29_97;
    case 60000:
      return ST_FPS_P59_94;
    case 30:
      return ST_FPS_P30;
    case 60:
      return ST_FPS_P60;
    case 25:
      return ST_FPS_P25;
    case 24:
      return ST_FPS_P24;
    case 50:
      return ST_FPS_P50;
    default:
      return ST_FPS_MAX;
  }
}