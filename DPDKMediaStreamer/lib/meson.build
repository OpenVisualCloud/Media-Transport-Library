# Copyright (C) 2020-2021 Intel Corporation.
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

#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#add DPDK dependences
dpdk = dependency('libdpdk')


cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : true)
numa_dep = cc.find_library('numa', required : true)
thread_dep = dependency('threads')

#add include directory
include_dir += include_directories('include')

#add source file
subdir('src')

#install header file (temporary, till only API functions will be used)
subdir('include')

lib_c_args = []

#enable warning as error
lib_c_args += [ '-Werror' ]

#simd build option
lib_c_args += ['-mavx2']
compiler = meson.get_compiler('c')
if compiler.has_header('rte_build_config.h')
  add_avx512f = run_command('grep', 'RTE_CPUFLAG_AVX512F', '/usr/local/include/rte_build_config.h')
  if add_avx512f.returncode() == 0
    if add_avx512f.stdout().contains('RTE_CPUFLAG_AVX512F')
      lib_c_args += ['-mavx512f']
    endif
  endif
endif

#build static library
self_lib = static_library('MediaStreamer', sources, include_directories : include_dir, c_args : lib_c_args, dependencies: [m_dep, numa_dep, thread_dep], install: true)
