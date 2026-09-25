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

# Intel SDL / OpenSSF compiler hardening, see doc/build.md
add_project_arguments(cc.get_supported_arguments(['-fstack-protector-strong',
  '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security']), language : 'c')
# keep the level a compiler (Ubuntu 24.04 gcc: 3) or CFLAGS already set
if get_option('optimization') not in ['0', 'plain'] and cc.get_define('_FORTIFY_SOURCE', args : '-O2') == ''
  add_project_arguments('-D_FORTIFY_SOURCE=2', language : 'c')
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