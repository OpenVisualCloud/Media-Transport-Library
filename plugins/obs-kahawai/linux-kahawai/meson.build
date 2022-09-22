# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('Intel(R) linux-kahawai obs plugin', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '0.1',)

cc = meson.get_compiler('c')

# deps
libobs = dependency('libobs', required : true)
libst_dpdk = dependency('libst_dpdk', required : true)
pthread_dep = cc.find_library('pthread', required : true)

# sources
sources = files(
	'linux-kahawai.c',
	'kahawai-input.c',
)

# args
linux_kahawai_c_args = []
linux_kahawai_c_args += ['-UHAVE_OBSCONFIG_H']

# build library
shared_library('linux-kahawai', sources,
  dependencies: [libobs, libst_dpdk, pthread_dep,],
  c_args : linux_kahawai_c_args,
  install: true,
  install_dir : '/usr/local/lib/obs-plugins/'
)

# copy locale files
install_subdir('data/locale', install_dir : '/usr/local/share/obs/obs-plugins/linux-kahawai/')