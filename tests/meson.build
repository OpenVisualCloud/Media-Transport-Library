# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_test', 'cpp', default_options: ['buildtype=release'],
        version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Environment: ' + exec_env)

cpp_c = meson.get_compiler('cpp')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

mtl = dependency('mtl', required : true)
gtest = dependency('gtest', required : true)
libpthread = cpp_c.find_library('pthread', required : true)
if not is_windows
  libnuma = cpp_c.find_library('numa', required : true)
else
  libnuma=[]
endif
libopenssl = dependency('openssl', required : true)

# add source file
subdir('integration_tests')

test_c_args = []
test_cpp_args = []
test_ld_args = []

is_gtest_newer_than_1_9_0 = gtest.version().version_compare('>=1.9.0')
if is_gtest_newer_than_1_9_0
  message('Gtest version: > 1.9.0; ' +  gtest.version())
  test_c_args += ['-DMTL_GTEST_AFTER_1_9_0']
  test_cpp_args += ['-DMTL_GTEST_AFTER_1_9_0']
endif

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  test_c_args += ['-Werror']
  test_cpp_args += ['-Werror']
else
  add_project_arguments('-DMTL_DEBUG=1', language : ['c', 'cpp'])
endif
test_c_args += ['-Wall']
test_cpp_args += ['-Wall']

# enable error on  Variable Length Arrays
test_cpp_args += ['-Wvla']

if is_windows
  test_c_args += ['-DWINDOWSENV']
  test_cpp_args += ['-DWINDOWSENV']
  ws2_32_dep = cpp_c.find_library('ws2_32', required: true)
else
  ws2_32_dep = [] # add this when the code uses hton/ntoh
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  test_c_args += ['-fsanitize=address']
  test_cpp_args += ['-fsanitize=address']
  asan_dep = cpp_c.find_library('asan', required : true)
endif

# build test executable
executable('KahawaiTest', sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  include_directories: [test_inc],
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, gtest, libnuma, libopenssl, libpthread, ws2_32_dep]
)

if not is_windows
# build ufd test executable
executable('KahawaiUfdTest', ufd_sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, gtest, libopenssl]
)

# build upl test executable
executable('KahawaiUplTest', upl_sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, gtest, libopenssl]
)
endif
