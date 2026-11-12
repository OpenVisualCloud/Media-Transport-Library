# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The FFmpeg plugin's unit tests, with MTL and FFmpeg stubbed.
# script/build_ffmpeg_plugin.sh unit builds and runs them.
project('mtl_ffmpeg_plugin_unit_tests', 'c', 'cpp', default_options : ['buildtype=release', 'b_pie=true'])

# Compiler hardening, see doc/getting_started/build.md
add_project_arguments('-fstack-protector-strong', '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security', '-Wimplicit-fallthrough', language : ['c', 'cpp'])
add_project_arguments('-D_GLIBCXX_ASSERTIONS', language : 'cpp')
foreach lang : ['c', 'cpp']
  if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none' and not ' '.join(get_option(lang + '_args')).contains('_FORTIFY_SOURCE')
    fortify = meson.get_compiler(lang).get_define('__USE_FORTIFY_LEVEL', prefix : '#include <string.h>',
      args : ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']) == '3' ? '3' : '2'
    add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=' + fortify, language : lang)
  endif
endforeach
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'cpp')

subdir('mtl')

gtest_dep = dependency('gtest', main : true, required : true)
threads_dep = dependency('threads', required : true)

executable('FfmpegPluginUnitTest', 'mtl_common_harness.c', 'mtl_common_test.cpp',
  c_args : ['-Wall', '-Werror'],
  cpp_args : ['-Wall', '-Werror'],
  dependencies : [gtest_dep, threads_dep],
  install : false)
