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
hardening = run_command(files('../../../script/hardening_flags.py'), 'c', get_option('optimization'),
  get_option('b_sanitize'), cc.cmd_array(), get_option('c_args'), check : true).stdout().split('\n')
add_project_arguments(hardening[0].split(), language : 'c')
add_project_link_arguments(hardening[1].split(), language : 'c')

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