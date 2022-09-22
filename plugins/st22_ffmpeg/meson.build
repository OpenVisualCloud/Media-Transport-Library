# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('Intel(R) st22 encoder plugins based on ffmpeg', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '0.1',)

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

cc = meson.get_compiler('c')

libst_dpdk_dep = dependency('libst_dpdk', required : true)
libpthread_dep = cc.find_library('pthread', required : true)
ffmpeg_avcodec_dep = dependency('libavcodec', required : true)
ffmpeg_avutil_dep = dependency('libavutil', required : true)

st22_ffmpeg_plugin_c_args = []
#enable warning as error
st22_ffmpeg_plugin_c_args += ['-Werror']
st22_ffmpeg_plugin_c_args += ['-Wall']
#simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
st22_ffmpeg_plugin_c_args += ['-msse4.2']

st22_ffmpeg_plugin_ld_args = []

st22_ffmpeg_plugin_sources = files('st22_ffmpeg_plugin.c')

# build st22 ffmpeg plugin lib
shared_library('st_plugin_st22_ffmpeg', st22_ffmpeg_plugin_sources,
  c_args : st22_ffmpeg_plugin_c_args,
  link_args : st22_ffmpeg_plugin_ld_args,
  dependencies: [libst_dpdk_dep, libpthread_dep, ffmpeg_avcodec_dep, ffmpeg_avutil_dep],
  install: true
)