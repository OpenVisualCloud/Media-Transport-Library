# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The FFmpeg plugin's unit tests, with MTL and FFmpeg stubbed.
# script/build_ffmpeg_plugin.sh unit builds and runs them.
project('mtl_ffmpeg_plugin_unit_tests', 'c', 'cpp', default_options : ['buildtype=release'])

subdir('mtl')

gtest_dep = dependency('gtest', main : true, required : true)
threads_dep = dependency('threads', required : true)

executable('FfmpegPluginUnitTest', 'mtl_common_harness.c', 'mtl_common_test.cpp',
  c_args : ['-Wall', '-Werror'],
  cpp_args : ['-Wall', '-Werror'],
  dependencies : [gtest_dep, threads_dep],
  install : false)
