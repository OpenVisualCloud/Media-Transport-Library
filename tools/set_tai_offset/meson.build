# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('set_tai_offset', 'c',
  default_options: ['buildtype=release', 'b_pie=true'],
)

cc = meson.get_compiler('c')

# Compiler hardening, see doc/build.md
add_project_arguments('-fstack-protector-strong', '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security', language : 'c')
if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none' and not ' '.join(get_option('c_args')).contains('_FORTIFY_SOURCE')
  fortify = cc.get_define('__USE_FORTIFY_LEVEL', prefix : '#include <string.h>',
    args : ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']) == '3' ? '3' : '2'
  add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=' + fortify, language : 'c')
endif
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'c')

executable('set_tai_offset', 'set_tai_offset.c',
  install: false,
)
