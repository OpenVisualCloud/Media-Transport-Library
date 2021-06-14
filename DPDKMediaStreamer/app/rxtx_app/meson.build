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

#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#add include directory
include_dir += include_directories('include')

#add DPDK dependences
dpdk = dependency('libdpdk')

sdl2_lib = meson.get_compiler('c').find_library('SDL2')
sdl2_dep = declare_dependency(dependencies: [ sdl2_lib])

m_dep = meson.get_compiler('c').find_library('m', required : true)

uselocalmslib = get_option('uselocalmslib')
if uselocalmslib == false
    mediastreamer = dependency('libMediaStreamer', required : true)
else
    mediastreamer = dependency('libMediaStreamer_null', required : false)
endif

if not mediastreamer.found()
    mediastreamer_inc = include_directories('../../lib/include','../../include')
    mediastreamer_lib = 'libMediaStreamer.a'
    mediastreamer_lib_dir = '../../build/lib'
    mediastreamer = declare_dependency(
        link_args : [mediastreamer_lib_dir + '/' + mediastreamer_lib],
        include_directories : mediastreamer_inc)
endif


add_project_link_arguments('-lnuma', language: 'c')

lib_c_args = []

#enable warning as error
lib_c_args += [ '-Werror' ]

lib_c_args += ['-mavx2']
compiler = meson.get_compiler('c')
if compiler.has_header('rte_build_config.h')
  add_avx512f = run_command('grep', 'RTE_CPUFLAG_AVX512F', '/usr/local/include/rte_build_config.h')
  if add_avx512f.returncode() == 0
    if add_avx512f.stdout().contains('RTE_CPUFLAG_AVX512F')
      lib_c_args += ['-mavx512f']
    endif
  endif
endif

rxtx_app_sources = [
	common_app_src,
	'rxtx_app.c',
	'rx_view.c'
]

thread = ['-pthread']

staticdpdklibs = run_command('pkg-config', '--static', '--cflags', '--libs', 'libdpdk')
if staticdpdklibs.returncode() == 0
add_project_link_arguments([staticdpdklibs.stdout().strip().split()], language: 'c')
add_project_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#build executable
executable('RxTxApp', rxtx_app_sources,
			c_args : lib_c_args,
			link_args: thread,
			include_directories : include_dir,
			dependencies: [mediastreamer,sdl2_dep, m_dep])
else
#build executable
executable('RxTxApp', rxtx_app_sources,
			c_args : lib_c_args,
			link_args: thread,
			include_directories : include_dir,
			dependencies: [dpdk, mediastreamer,sdl2_dep, m_dep])

endif
