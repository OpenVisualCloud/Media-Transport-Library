# Copyright (C) 2021 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials,
# and your use of them is governed by the express license under which they
# were provided to you ("License").
# Unless the License provides otherwise, you may not use, modify, copy,
# publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no
# express or implied warranties, other than those that are expressly stated
# in the License.

project('st_dpdk', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '0.7.0',)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Enviroment: ' + exec_env)
#enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

#get external variables
add_global_arguments('-D__ST_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--tags', '--dirty', '--always').stdout().strip() + '"', language : 'c')
add_global_arguments('-D__ST_BUILD__="' + meson.project_version() + '"', language : 'c')

if get_option('enable_kni') == true
  add_global_arguments('-DST_HAS_KNI', language : 'c')
endif

st_dpdk_lib = []

#add include directory
st_dpdk_include_dir = [ include_directories('include'), ]

#install header files
subdir('include')

#build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(
  name : meson.project_name(),
  version : meson.project_version(),
  libraries : st_dpdk_lib,
  filebase : 'lib' + meson.project_name(),
  description : 'Intel(R) ST(SMPTE ST) Library based on DPDK'
)
