# Copyright (C) 2020-2021 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials,
# and your use of them is governed by the express license under which they
# were provided to you ("License").
# Unless the License provides otherwise, you may not use, modify, copy,
# publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no
# express or implied warranties, other than those that are expressly stated
# in the License.

project('MediaStreamer', 'c',
                default_options: ['c_std=gnu11', 'buildtype=release'],
        		version: '1.1',)

#enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

#get external variables
add_global_arguments('-DGIT="'+ run_command('git', 'describe', '--always').stdout().strip() + '"', language : 'c')
add_global_arguments('-DBUILD="' + meson.project_version() + '"', language : 'c')

self_lib = []

#add include directory
include_dir = [ include_directories('include'), ]

#install header files
subdir('include')

#build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(
		name : meson.project_name(),
		version : meson.project_version(),
		libraries : self_lib,
		filebase : 'lib' + meson.project_name(),
             	description : 'Intel(R) Media Streaming Library based on DPDK'
)
