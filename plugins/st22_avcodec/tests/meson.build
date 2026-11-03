# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The plugin's tests, against the installed MTL and FFmpeg the plugin is built against.
# script/build_st22_avcodec_plugin.sh unit builds and runs them.
project('st22_avcodec_plugin_tests', 'c', 'cpp', default_options : ['buildtype=release', 'warning_level=2', 'b_pie=true'])

# Compiler hardening, see doc/build.md
hardening_c = run_command(files('../../../script/hardening_flags.py'), 'c', get_option('optimization'),
  get_option('b_sanitize'), meson.get_compiler('c').cmd_array(), get_option('c_args'),
  check : true).stdout().split('\n')
hardening_cpp = run_command(files('../../../script/hardening_flags.py'), 'c++', get_option('optimization'),
  get_option('b_sanitize'), meson.get_compiler('cpp').cmd_array(), get_option('cpp_args'),
  check : true).stdout().split('\n')
add_project_arguments(hardening_c[0].split(), language : 'c')
add_project_arguments(hardening_cpp[0].split(), language : 'cpp')
add_project_link_arguments(hardening_cpp[1].split(), language : 'cpp')

# The flags of the plugin build, as the harness compiles the plugin in place.
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

mtl_dep = dependency('mtl', required : true)
avcodec_dep = dependency('libavcodec', required : true)
avutil_dep = dependency('libavutil', required : true)
gtest_dep = dependency('gtest', main : true, required : true)
threads_dep = dependency('threads', required : true)

executable('St22AvcodecTest', 'st22_avcodec_harness.c', 'st22_avcodec_test.cpp',
  c_args : ['-Wall', '-Werror', '-msse4.2'],
  cpp_args : ['-Wall', '-Werror'],
  # A local install (MTL_PLUGIN_PREFIX) is on no loader path.
  build_rpath : ':'.join([mtl_dep.get_variable(pkgconfig : 'libdir'),
                          avcodec_dep.get_variable(pkgconfig : 'libdir')]),
  dependencies : [mtl_dep, avcodec_dep, avutil_dep, gtest_dep, threads_dep],
  install : false)
