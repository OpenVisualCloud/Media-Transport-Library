# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project(
    'mtl_rdma',
    'c',
    default_options: ['buildtype=release'],
    version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),
)

cc = meson.get_compiler('c')

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

if is_windows
    message('not supported on Windows')
    subdir_done()
endif

# rdma dependencies check
libibvers_dep = dependency('libibverbs', required: false)
librdmacm_dep = dependency('librdmacm', required: false)
if not libibvers_dep.found() or not librdmacm_dep.found()
    message('rdma dependencies not found')
    subdir_done()
endif

mtl_rdma_c_args = []
if get_option('buildtype') != 'debug'
    mtl_rdma_c_args += ['-Werror']
endif
mtl_rdma_c_args += ['-Wall']

pthread_dep = cc.find_library('pthread', required: true)
asan_dep = []
if get_option('enable_asan') == true
    message('Enable -fsanitize=address')
    mtl_rdma_c_args += ['-fsanitize=address']
    asan_dep = cc.find_library('asan', required: true)
endif

mtl_rdma_header_file = files('mtl_rdma_api.h')
install_headers(mtl_rdma_header_file, subdir: meson.project_name())

mtl_rdma_sources = files('mt_rdma.c', 'mt_rdma_rx.c', 'mt_rdma_tx.c')

mtl_rdma_lib = shared_library(
    meson.project_name(),
    mtl_rdma_sources,
    dependencies: [libibvers_dep, librdmacm_dep, asan_dep, pthread_dep],
    install: true,
    c_args: mtl_rdma_c_args,
)

pkg = import('pkgconfig')
pkg.generate(
    name: meson.project_name(),
    version: meson.project_version(),
    libraries: mtl_rdma_lib,
    filebase: meson.project_name(),
    description: 'Intel® Media Transport Library - RDMA',
)