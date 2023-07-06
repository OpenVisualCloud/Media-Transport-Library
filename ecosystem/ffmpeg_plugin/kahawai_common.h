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

#include <mtl/st_pipeline_api.h>

#include "libavutil/rational.h"

typedef struct KahawaiFpsDecs {
  enum st_fps st_fps;
  unsigned int min;
  unsigned int max;
} KahawaiFpsDecs;

enum st_fps kahawai_fps_to_st_fps(AVRational framerate);
mtl_handle kahawai_init(char* port, char* local_addr, int enc_session_cnt,
                        int dec_session_cnt, char* dma_dev);
mtl_handle kahawai_get_handle();
void kahawai_set_handle(mtl_handle handle);
