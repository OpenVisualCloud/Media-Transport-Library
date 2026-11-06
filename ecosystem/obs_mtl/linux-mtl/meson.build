# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('linux-mtl', 'c', default_options: ['buildtype=release'],
        version: run_command(find_program('cat'), files('../../../VERSION'), check: true).stdout().strip(),)

cc = meson.get_compiler('c')

# deps
libobs = dependency('libobs', required : true)
mtl = dependency('mtl', required : true)
pthread_dep = cc.find_library('pthread', required : true)

libobs_libdir = libobs.get_variable(pkgconfig: 'libdir')
libobs_prefix = libobs.get_variable(pkgconfig: 'prefix')

# sources
sources = files(
	'linux-mtl.c',
	'mtl-input.c',
  'mtl-output.c',
)

# Compiler hardening, see doc/build.md
add_project_arguments('-fstack-protector-strong', '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security', language : 'c')
if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none' and not ' '.join(get_option('c_args')).contains('_FORTIFY_SOURCE')
  fortify = cc.get_define('__USE_FORTIFY_LEVEL', prefix : '#include <string.h>',
    args : ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']) == '3' ? '3' : '2'
  add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=' + fortify, language : 'c')
endif
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'c')

# args
linux_mtl_c_args = []
linux_mtl_c_args += ['-UHAVE_OBSCONFIG_H']
linux_mtl_c_args += ['-Wno-stringop-truncation']

# build library
shared_library(meson.project_name(), sources,
  dependencies: [libobs, mtl, pthread_dep,],
  c_args : linux_mtl_c_args,
  install: true,
  install_dir : libobs_libdir + '/obs-plugins/'
)

# copy locale files
install_subdir('data/locale', install_dir : libobs_prefix + '/share/obs/obs-plugins/linux-mtl/')