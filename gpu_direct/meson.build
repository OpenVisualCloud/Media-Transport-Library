# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project(
    'mtl_gpu_direct',
    'c',
    default_options: ['buildtype=release'],
    version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),
)

cc = meson.get_compiler('c')
toolchain = cc.get_id()

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
libze_loader_dep = cc.find_library('libze_loader', required : true)


if is_windows
    message('not supported on Windows')
    subdir_done()
endif

# gpu_direct dependencies check


mtl_gpu_direct_c_args = []
if get_option('buildtype') != 'debug'
    mtl_gpu_direct_c_args += ['-Werror']
endif
mtl_gpu_direct_c_args += ['-Wall']

mtl_gpu_direct_header_file = files('gpu.h')
install_headers(mtl_gpu_direct_header_file, subdir: meson.project_name())

mtl_gpu_direct_sources = files('gpu.c')

mtl_gpu_direct_lib = shared_library(
    meson.project_name(),
    mtl_gpu_direct_sources,
    c_args: mtl_gpu_direct_c_args,
    dependencies: [libze_loader_dep],
    install: true
)

pkg = import('pkgconfig')
pkg.generate(
    name: meson.project_name(),
    version: meson.project_version(),
    libraries: mtl_gpu_direct_lib,
    filebase: meson.project_name(),
    description: 'Media Transport Library - gpu_direct',
)
