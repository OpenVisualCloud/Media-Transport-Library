# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

mtl_deprecated_header_files = files(
  'deprecated/mudp_api.h',
  'deprecated/mudp_sockfd_api.h',
  'deprecated/mudp_sockfd_internal.h',
  'deprecated/mudp_win.h',
)

mtl_header_files = files('mtl_api.h', 'st_api.h', 'st_convert_api.h', 'st_convert_internal.h',
  'st_pipeline_api.h', 'st20_api.h', 'st30_api.h', 'st40_api.h', 'st41_api.h', 'mtl_lcore_shm_api.h',
  'mtl_sch_api.h', 'st30_pipeline_api.h', 'st40_pipeline_api.h') + mtl_deprecated_header_files

if is_windows
  mtl_header_files += files('deprecated/mudp_win.h')
endif

install_headers(mtl_header_files, subdir : meson.project_name())

# experimental features
mtl_experimental_header_files = files('experimental/st20_combined_api.h')

experimental_header_install_path = meson.project_name() + '/experimental'

install_headers(mtl_experimental_header_files, subdir : experimental_header_install_path)
