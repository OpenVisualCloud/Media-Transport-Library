# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('st22 encoder plugins based on avcodec', 'c', default_options: ['buildtype=release'],
        version: run_command(find_program('cat'), files('../../VERSION'), check: true).stdout().strip(),)

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

cc = meson.get_compiler('c')

mtl_dep = dependency('mtl', required : true)
libpthread_dep = cc.find_library('pthread', required : true)
avcodec_dep = dependency('libavcodec', required : true)
avutil_dep = dependency('libavutil', required : true)

# Compiler hardening, see doc/build.md
hardening = run_command(files('../../script/hardening_flags.py'), 'c', get_option('optimization'),
  get_option('b_sanitize'), cc.cmd_array(), get_option('c_args'), check : true).stdout().split('\n')
add_project_arguments(hardening[0].split(), language : 'c')
add_project_link_arguments(hardening[1].split(), language : 'c')

st22_avcodec_plugin_c_args = []
# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  st22_avcodec_plugin_c_args += ['-Werror']
endif
st22_avcodec_plugin_c_args += ['-Wall']
#simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512?
st22_avcodec_plugin_c_args += ['-msse4.2']

st22_avcodec_plugin_ld_args = []

st22_avcodec_plugin_sources = files('st22_avcodec_plugin.c')

# build st22 avcodec plugin lib
shared_library('st_plugin_st22_avcodec', st22_avcodec_plugin_sources,
  c_args : st22_avcodec_plugin_c_args,
  link_args : st22_avcodec_plugin_ld_args,
  dependencies: [mtl_dep, libpthread_dep, avcodec_dep, avutil_dep],
  install: true
)