# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

# mtlm_api.h and mtl_mproto.h are the control socket of MtlManager. They are
# here, not under manager/, because they are public API: the library speaks that
# protocol, and so may any other program on the host. The manager project
# installs the same two files, so it stays installable on its own.
mtl_header_files = files('mtl_api.h', 'st_api.h', 'st_convert_api.h', 'st_convert_internal.h',
  'st_pipeline_api.h', 'st20_api.h', 'st30_api.h', 'st40_api.h', 'st41_api.h', 'mtl_lcore_shm_api.h',
  'mtl_sch_api.h', 'st30_pipeline_api.h', 'st40_pipeline_api.h',
  'mtlm_api.h', 'mtl_mproto.h')

install_headers(mtl_header_files, subdir : meson.project_name())

# experimental features
mtl_experimental_header_files = files('experimental/st20_combined_api.h')

experimental_header_install_path = meson.project_name() + '/experimental'

install_headers(mtl_experimental_header_files, subdir : experimental_header_install_path)
