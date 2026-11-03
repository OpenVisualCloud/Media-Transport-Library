# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The FFmpeg plugin's unit tests, with MTL and FFmpeg stubbed.
# script/build_ffmpeg_plugin.sh unit builds and runs them.
project('mtl_ffmpeg_plugin_unit_tests', 'c', 'cpp', default_options : ['buildtype=release', 'warning_level=2', 'b_pie=true'])

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

subdir('mtl')

gtest_dep = dependency('gtest', main : true, required : true)
threads_dep = dependency('threads', required : true)

executable('FfmpegPluginUnitTest', 'mtl_common_harness.c', 'mtl_common_test.cpp',
  c_args : ['-Wall', '-Werror'],
  cpp_args : ['-Wall', '-Werror'],
  dependencies : [gtest_dep, threads_dep],
  install : false)
