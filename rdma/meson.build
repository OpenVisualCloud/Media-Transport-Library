# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project(
    'mtl_rdma',
    'c',
    default_options: ['buildtype=release'],
    version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),
)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

if is_windows
    message('not supported on Windows')
    subdir_done()
endif

message('BUILD Environment: ' + exec_env)

mtl_rdma_c_args = []
if get_option('buildtype') != 'debug'
    mtl_rdma_c_args += ['-Werror']
endif
mtl_rdma_c_args += ['-Wall']

asan_dep = []
if get_option('enable_asan') == true
    message('Enable -fsanitize=address')
    mtl_rdma_c_args += ['-fsanitize=address']
    asan_dep = cc.find_library('asan', required: true)
endif

# rdma dependencies check
libibvers_dep = dependency('libibverbs', required: true)
librdmacm_dep = dependency('librdmacm', required: true)

mtl_rdma_sources = files() # todo: add sources

mtl_rdma_lib = shared_library(
    meson.project_name(),
    mtl_rdma_sources,
    dependencies: [libibvers_dep, librdmacm_dep, asan_dep],
    install: true,
    c_args: mtl_rdma_c_args,
)

mtl_rdma_header_file = files('mtl_rdma_api.h')

install_headers(mtl_rdma_header_file, subdir: meson.project_name())

