# SPDX-License-Identifier: BSD-3-Clause

add_languages('cpp', required : true)

unit_cpp_args = ['-Wall', '-Werror']
unit_c_args = ['-Wall', '-Werror']
unit_link_args = ['-Wl,--allow-multiple-definition']

unit_include_dirs = [
  include_directories('../../include'),
  include_directories('../../lib/src'),
  include_directories('.'),
]

gtest_dep = dependency('gtest', required : true)
libpthread_dep = meson.get_compiler('c').find_library('pthread', required : true)

unit_sources = [
  'common/ut_common.c',
  'session/st40_harness.c',
  'session/st40_test.cpp',
  'session/st30_harness.c',
  'session/st30_test.cpp',
  'session/st20_harness.c',
  'session/st20_test.cpp',
  'pipeline/st40p_harness.c',
  'pipeline/st40p_test.cpp',
  'main.cpp',
]

executable('UnitTest', unit_sources,
  c_args : unit_c_args,
  cpp_args : unit_cpp_args,
  link_args : unit_link_args,
  include_directories : unit_include_dirs,
  dependencies : [mtl_internal_dep, dpdk_dep, libm_dep, gtest_dep, libpthread_dep,
                  usdt_provider_header_dep],
  install : false)
