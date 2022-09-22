# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('st_dpdk', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '22.09.0',)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Enviroment: ' + exec_env)
# enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

# get external variables
add_global_arguments('-D__ST_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always').stdout().strip() + '"', language : 'c')
add_global_arguments('-D__ST_BUILD__="' + meson.project_version() + '"', language : 'c')

if get_option('enable_kni') == true
  add_global_arguments('-DST_HAS_KNI', language : 'c')
endif

if is_windows
if get_option('enable_tap') == true
  add_global_arguments('-DST_HAS_TAP', language : 'c')
endif
endif

st_dpdk_lib = []

# add include directory
st_dpdk_include_dir = [ include_directories('include'), ]

# install header files
subdir('include')

# build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(
  name : meson.project_name(),
  version : meson.project_version(),
  libraries : st_dpdk_lib,
  filebase : 'lib' + meson.project_name(),
  description : 'Intel(R) ST(SMPTE ST) Library based on DPDK'
)
