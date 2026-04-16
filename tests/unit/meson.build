# SPDX-License-Identifier: BSD-3-Clause

add_languages('cpp', required : true)

unit_cpp_args = ['-Wall', '-Werror']
unit_c_args = ['-Wall', '-Werror']
unit_link_args = ['-Wl,--allow-multiple-definition']

unit_include_dirs = [
  include_directories('../../include'),
  include_directories('../../lib/src'),
]

gtest_dep = dependency('gtest', required : true)
libpthread_dep = meson.get_compiler('c').find_library('pthread', required : true)

unit_sources = [
  'st40_rx_redundancy_harness.c',
  'st40_rx_redundancy_test.cpp',
  'st30_rx_redundancy_harness.c',
  'st30_rx_redundancy_test.cpp',
  'st20_rx_redundancy_harness.c',
  'st20_rx_redundancy_test.cpp',
]

executable('UnitTest', unit_sources,
  c_args : unit_c_args,
  cpp_args : unit_cpp_args,
  link_args : unit_link_args,
  include_directories : unit_include_dirs,
  dependencies : [mtl_internal_dep, dpdk_dep, libm_dep, gtest_dep, libpthread_dep,
                  usdt_provider_header_dep],
  install : false)
