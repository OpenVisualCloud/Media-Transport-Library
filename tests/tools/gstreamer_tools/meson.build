# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project('gstreamer_mtl_tools', 'c',
        version: run_command(find_program('cat'), files('../../../VERSION'), check: true).stdout().strip(), license : 'LGPL')

cc = meson.get_compiler('c')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

plugin_c_args = [
  '-Wno-redundant-decls',
  '-Wno-discarded-qualifiers',
  '-Wno-declaration-after-statement',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-stringop-truncation'
]

gst_dep = dependency('gstreamer-1.0', version : '>=1.19', required : true)
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.19', required : true)
mtl_dep = dependency('mtl', required : true)

if run_command('test', '-d', '/usr/local/include/gstreamer-1.0', check: false).returncode() == 0
    gstreamer_inc_dir  = '/usr/local/include/gstreamer-1.0'
else
    gstreamer_inc_dir  = '/usr/include/gstreamer-1.0'
endif

if run_command('test', '-d', '/usr/local/include/glib-2.0', check: false).returncode() == 0
  glib_2_0_inc_dir  = '/usr/local/include/glib-2.0'
else
  glib_2_0_inc_dir  = '/usr/include/glib-2.0'
endif

inc_dirs = include_directories(
  '/usr/local/include/mtl',
  glib_2_0_inc_dir,
  gstreamer_inc_dir
)

gst_mtl_anc_generator = library('ancgenerator',
  'gst_anc_generator.c',
  dependencies : [gst_dep, gstbase_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

gst_mtl_time_inserter = library('timeinserter',
  'gst_time_inserter.c',
  dependencies : [gst_dep, gstbase_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)
