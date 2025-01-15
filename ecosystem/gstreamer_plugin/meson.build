# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project('gstreamer_mtl_plugins', 'c',
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
  '-Wno-stringop-truncation'
]

if run_command('test', '-d', '/usr/local/include/gstreamer-1.0', check: false).returncode() == 0
    gstreamer_inc_dir = '/usr/local/include/gstreamer-1.0'
else
    gstreamer_inc_dir = '/usr/include/gstreamer-1.0'
endif

inc_dirs = include_directories(
    '/usr/include/glib-2.0',
    '/usr/lib/x86_64-linux-gnu/glib-2.0/include',
    '/usr/local/include/mtl',
    gstreamer_inc_dir
)

gstreamer_video_dep = dependency('gstreamer-video-1.0')
gstreamer_audio_dep = dependency('gstreamer-audio-1.0')
mtl_dep             = dependency('mtl')

gst_mtl_common_sources = [
  'gst_mtl_common.c'
]

# mtl_st20p_tx Plugin
gst_mtl_st20p_tx_sources = [
  'gst_mtl_st20p_tx.c'
]

gst_mtl_st20p_tx = library('gstmtl_st20p_tx',
  gst_mtl_common_sources + gst_mtl_st20p_tx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_video_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st20p_rx Plugin
gst_mtl_st20p_rx_sources = [
  'gst_mtl_st20p_rx.c'
]

gst_mtl_st20p_rx = library('gstmtl_st20p_rx',
  gst_mtl_common_sources + gst_mtl_st20p_rx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_video_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st30p_rx Plugin
gst_mtl_st30p_rx_sources = [
  'gst_mtl_st30p_rx.c'
]

gst_mtl_st30p_rx = library('gstmtl_st30p_rx',
  gst_mtl_common_sources + gst_mtl_st30p_rx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_audio_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st30p_tx Plugin
gstmtl_st30p_tx_sources = [
    'gst_mtl_st30p_tx.c',
]

gstmtl_st30p_tx = library('gstmtl_st30p_tx',
    gst_mtl_common_sources + gstmtl_st30p_tx_sources,
    dependencies : [gst_dep, gstbase_dep, gstreamer_audio_dep, mtl_dep],
    install : true,
    install_dir : plugins_install_dir,
    include_directories: inc_dirs,
    c_args: plugin_c_args
)

# mtl_st40_rx Plugin
gst_mtl_st40_rx_sources = [
  'gst_mtl_st40_rx.c'
]

gst_mtl_st40_rx = library('gstmtl_st40_rx',
  gst_mtl_common_sources + gst_mtl_st40_rx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_audio_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)
