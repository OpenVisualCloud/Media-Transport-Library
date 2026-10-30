# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The FFmpeg plugin's unit tests, against the installed MTL the plugin is built
# against. script/build_ffmpeg_plugin.sh unit builds and runs them.
project('mtl_ffmpeg_plugin_unit_tests', 'c', 'cpp', default_options : ['buildtype=release'])

mtl_dep = dependency('mtl', required : true)
gtest_dep = dependency('gtest', main : true, required : true)
threads_dep = dependency('threads', required : true)

executable('FfmpegPluginUnitTest', 'mtl_common_harness.c', 'mtl_common_test.cpp',
  c_args : ['-Wall', '-Werror'],
  cpp_args : ['-Wall', '-Werror'],
  # A local install (MTL_INSTALL_PREFIX) is on no loader path.
  build_rpath : mtl_dep.get_variable(pkgconfig : 'libdir'),
  dependencies : [mtl_dep, gtest_dep, threads_dep],
  install : false)
