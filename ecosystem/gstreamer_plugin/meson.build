# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project('gstreamer-mtl-st20tx', 'c',
        version: run_command(find_program('cat'), files('../../VERSION'), check: true).stdout().strip(), license : 'LGPL')

cc = meson.get_compiler('c')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

gst_dep = dependency('gstreamer-1.0', version : '>=1.19',
    required : true, fallback : ['gstreamer', 'gst_dep'])
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.19',
  fallback : ['gstreamer', 'gst_base_dep'])

plugin_c_args = [
  '-Wno-redundant-decls',
  '-Wno-discarded-qualifiers',
  '-Wno-declaration-after-statement',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-stringop-truncation',
]

inc_dirs = include_directories(
    '/usr/include/glib-2.0',
    '/usr/include/gstreamer-1.0',
    '/usr/lib/x86_64-linux-gnu/glib-2.0/include',
    '/usr/local/include/mtl'
)

gstreamer_video_dep = dependency('gstreamer-video-1.0')
mtl_dep             = dependency('mtl')

# The mtl_tx_sink Plugin
 gstmtl_tx_sink_sources = [
  'gstmtltxsink.c',
  ]

gstmtl_tx_sinkexample = library('gstmtltxsink',
  gstmtl_tx_sink_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_video_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)