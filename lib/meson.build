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

cc = meson.get_compiler('c')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

# add dependences
dpdk_dep = dependency('libdpdk', required : true)
libm_dep = cc.find_library('m', required : true)
libpthread_dep = cc.find_library('pthread', required : true)
libdl_dep = cc.find_library('dl', required : true)
if not is_windows
  libnuma_dep = cc.find_library('numa', required : true)
endif
jsonc_dep = dependency('json-c', required : true)

# add source file
subdir('src')

st_dpdk_c_args = []

# enable warning as error
st_dpdk_c_args += ['-Werror']
st_dpdk_c_args += ['-Wall']

#simd build option, enable sse4.2 default
st_dpdk_c_args += ['-msse4.2']

# link option
st_dpdk_link_c_args = []

if is_windows
  libnuma_dep = []
  dpdk_root_dir = get_option('dpdk_root_dir')
  dpdkcflags = run_command('pkg-config', '--cflags', 'libdpdk')
  dpdklibs = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  st_dpdk_include_dir += include_directories('''.\windows\''')
  st_dpdk_include_dir += include_directories(dpdk_root_dir + '''\lib\eal\windows\include\''')
  st_dpdk_include_dir += include_directories(dpdk_root_dir + '''\lib\kni\''')
  st_dpdk_c_args += ['-static','-DWINDOWSENV','-D_WIN32_WINNT=0x0600','-Wformat=0','-Wno-maybe-uninitialized','-mno-ms-bitfields', '-Og']
  st_dpdk_link_c_args += ['-static','-limagehlp', '-lsetupapi', '-lWs2_32']
endif

if cc.has_argument('-mavx2')
  st_dpdk_c_args += ['-DST_HAS_AVX2']
endif

if cc.has_argument('-mavx512vl')
  st_dpdk_c_args += ['-DST_HAS_AVX512']
endif

if cc.has_argument('-mavx512vbmi2')
  st_dpdk_c_args += ['-DST_HAS_AVX512_VBMI2']
endif

# build library
if is_windows
st_dpdk_lib += static_library(meson.project_name(), sources,
  include_directories : st_dpdk_include_dir,
  c_args : st_dpdk_c_args,
  link_args : st_dpdk_link_c_args,
  dependencies: [dpdk_dep, libm_dep, libnuma_dep, libpthread_dep, libdl_dep, jsonc_dep],
  install: true
)
else
st_dpdk_lib += shared_library(meson.project_name(), sources,
  include_directories : st_dpdk_include_dir,
  c_args : st_dpdk_c_args,
  link_args : st_dpdk_link_c_args,
  dependencies: [dpdk_dep, libm_dep, libnuma_dep, libpthread_dep, libdl_dep, jsonc_dep],
  install: true
)
endif