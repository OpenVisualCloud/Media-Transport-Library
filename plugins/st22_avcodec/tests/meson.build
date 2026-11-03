# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The plugin's tests, against the installed MTL and FFmpeg the plugin is built against.
# script/build_st22_avcodec_plugin.sh unit builds and runs them.
project('st22_avcodec_plugin_tests', 'c', 'cpp', default_options : ['buildtype=release', 'warning_level=2', 'b_pie=true'])

# Compiler hardening, see doc/build.md
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
