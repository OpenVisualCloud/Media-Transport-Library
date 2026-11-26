# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The GStreamer plugins' unit tests: header code only, no MTL and no GStreamer.
# script/build_gstreamer_plugin.sh unit builds and runs them.
project('mtl_gstreamer_plugin_unit_tests', 'cpp', default_options : ['buildtype=release', 'warning_level=2', 'b_pie=true'])

# Compiler hardening, see doc/build.md
add_project_arguments('-fstack-protector-strong', '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security', '-Wimplicit-fallthrough', language : 'cpp')
add_project_arguments('-D_GLIBCXX_ASSERTIONS', language : 'cpp')
foreach lang : ['cpp']
  if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none' and not ' '.join(get_option(lang + '_args')).contains('_FORTIFY_SOURCE')
    fortify = meson.get_compiler(lang).get_define('__USE_FORTIFY_LEVEL', prefix : '#include <string.h>',
      args : ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']) == '3' ? '3' : '2'
    add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=' + fortify, language : lang)
  endif
endforeach
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'cpp')

gtest_dep = dependency('gtest', main : true, required : true)

executable('GstPluginUnitTest', 'pts_tai_test.cpp',
  cpp_args : ['-Wall', '-Werror'],
  dependencies : [gtest_dep],
  install : false)
