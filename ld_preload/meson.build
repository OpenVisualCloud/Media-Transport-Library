# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

project('mtl_preload', 'c', default_options: ['buildtype=release'],
        version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),)

#detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

cc = meson.get_compiler('c')

mtl_dep = dependency('mtl', required : true)
libdl_dep = cc.find_library('dl', required : true)

# build udp preload
subdir('udp')
