/*
 * Kahawai common struct and functions
 * Copyright (c) 2023 Intel
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>

#include "libavutil/rational.h"

#include "kahawai_common.h"

const KahawaiFpsDecs fps_table[] = {
    {ST_FPS_P50, 5000 - 100, 5000 + 100},
    {ST_FPS_P29_97, 2997 - 100, 2997 + 100},    {ST_FPS_P25, 2500 - 100, 2500 + 100},
    {ST_FPS_P60, 6000 - 100, 6000 + 100},       {ST_FPS_P30, 3000 - 100, 3000 + 100},
    {ST_FPS_P24, 2400 - 100, 2400 + 100},       {ST_FPS_P23_98, 2398 - 100, 2398 + 100},
    {ST_FPS_P119_88, 11988 - 100, 11988 + 100}
};

mtl_handle shared_st_handle = NULL;
unsigned int active_session_cnt = 0;
struct mtl_init_params param = {0};

enum st_fps get_fps_table(AVRational framerate)
{
  int ret;
  unsigned int fps = framerate.num * 100 / framerate.den;

  for (ret = 0; ret < sizeof(fps_table)/sizeof(KahawaiFpsDecs); ++ret) {
    if ((fps >= fps_table[ret].min) && (fps <= fps_table[ret].max)) {
      return fps_table[ret].st_fps;
    }
  }
  return ST_FPS_MAX;
}