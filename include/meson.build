# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

mtl_header_files = files('mtl_api.h', 'st_api.h', 'st_convert_api.h', 'st_convert_internal.h', 'st_pipeline_api.h', 'st20_api.h', 'st30_api.h', 'st40_api.h',
  'st20_redundant_api.h', 'mudp_api.h', 'mudp_sockfd_api.h', 'mudp_sockfd_internal.h', 'mtl_lcore_shm_api.h')

if is_windows
  mtl_header_files += files('mudp_win.h')
endif

install_headers(mtl_header_files, subdir : meson.project_name())
